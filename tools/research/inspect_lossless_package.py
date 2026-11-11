#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static metadata only; never execute a supplied PE or write shader bytes.

Requires pefile (analysis environment only, not a GSF runtime dependency).
Usage: python inspect_lossless_package.py ARCHIVE.tar.gz > report.json
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
import sys
import tarfile

import pefile


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def shader_metadata(data):
    """Check container/instruction bounds, not shader semantics or compatibility."""
    if data[:4] == b'\x03\x02\x23\x07':
        valid = len(data) >= 20 and len(data) % 4 == 0
        words = struct.unpack('<%dI' % (len(data) // 4), data) if valid else ()
        entries, i = [], 5
        while valid and i < len(words):
            count, opcode = words[i] >> 16, words[i] & 65535
            if count == 0 or i + count > len(words):
                valid = False
                break
            if opcode == 15:
                if count < 4:
                    valid = False
                    break
                entries.append({'execution_model': words[i + 1]})
            i += count
        return {'format': 'SPIR-V', 'structural_bounds_ok': valid and i == len(words),
                'version_word': words[1] if words else None, 'entry_points': entries}
    if data[:4] == b'DXBC':
        valid, stages = len(data) >= 32, []
        if valid:
            size, count = struct.unpack_from('<II', data, 24)
            valid = size == len(data) and 32 + count * 4 <= len(data)
            if valid:
                for offset in struct.unpack_from('<%dI' % count, data, 32):
                    if offset + 8 > len(data):
                        valid = False
                        break
                    tag, size = struct.unpack_from('<4sI', data, offset)
                    if offset + 8 + size > len(data):
                        valid = False
                        break
                    if tag in (b'SHEX', b'SHDR'):
                        if size < 8:
                            valid = False
                            break
                        token, words = struct.unpack_from('<II', data, offset + 8)
                        valid = valid and words * 4 == size
                        stages.append({'program_type': token >> 16,
                                       'major': (token >> 4) & 15, 'minor': token & 15})
        return {'format': 'DXBC', 'structural_bounds_ok': valid, 'programs': stages}
    return {'format': 'other'}


def resource_inventory(pe):
    result = []
    for typ in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for name in typ.directory.entries:
            for lang in name.directory.entries:
                info = lang.data.struct
                data = pe.get_data(info.OffsetToData, info.Size)
                if len(data) != info.Size:
                    raise ValueError('truncated resource')
                result.append({'path': [typ.id, name.id, lang.id], 'size': len(data),
                               'sha256': sha256(data), **shader_metadata(data)})
    return result


def inspect(archive):
    selected = {'Lossless.dll', 'LosslessScaling.dll', 'LosslessScaling.exe', 'config.ini'}
    files = {}
    # Read selected members into memory without extracting paths or loading native code.
    with tarfile.open(archive, 'r:gz') as stream:
        for member in stream:
            name = Path(member.name).name
            if member.isfile() and name in selected:
                if name in files or member.size > 32 * 1024 * 1024:
                    raise ValueError('duplicate/oversized selected member: ' + name)
                files[name] = stream.extractfile(member).read()
    if set(files) != selected:
        raise ValueError('missing selected files')
    pe = pefile.PE(data=files['Lossless.dll'])
    rows = resource_inventory(pe)
    versions = {}
    for group in getattr(pe, 'FileInfo', []):
        for block in group:
            for table in getattr(block, 'StringTable', []):
                versions.update({k.decode('utf-8'): v.decode('utf-8')
                                 for k, v in table.entries.items() if k in (b'FileVersion', b'ProductVersion')})
    imports = {entry.dll.decode('ascii'): [symbol.name.decode('ascii') if symbol.name else
               'ordinal:%s' % symbol.ordinal for symbol in entry.imports]
               for entry in pe.DIRECTORY_ENTRY_IMPORT}
    return {'analysis': 'static; no execution; structural checks are not semantic validation',
            'archive_sha256': sha256(Path(archive).read_bytes()),
            'files': {name: {'size': len(data), 'sha256': sha256(data)}
                      for name, data in sorted(files.items())},
            'native_version': versions,
            'exports': [{'name': entry.name.decode('ascii'), 'rva': entry.address}
                        for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols],
            'imports': imports,
            'resource_formats': dict(collections.Counter(row['format'] for row in rows)),
            'unique_shader_hashes': {kind: len({row['sha256'] for row in rows if row['format'] == kind})
                                     for kind in ('SPIR-V', 'DXBC')},
            'resources': rows}


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print(json.dumps(inspect(sys.argv[1]), indent=2, sort_keys=True))
