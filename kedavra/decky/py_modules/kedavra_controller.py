"""Cross-plugin controller lease. All compositor mutations use the same flock.

GFG has priority while loaded. Standalone regains control after release/death.
The pid start time prevents a reused pid from retaining ownership.
"""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import uuid

class ControllerUnavailable(RuntimeError):
    pass


def process_start(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19]
    except (OSError, IndexError):
        return None


class ControllerLease:
    def __init__(self, name, root=Path('/var/lib/kedavra')):
        self.name, self.root = name, Path(root)
        self.token = uuid.uuid4().hex
        self.pid, self.start = os.getpid(), process_start(os.getpid())
        self.closed = False

    @contextlib.contextmanager
    def _locked(self):
        self.root.mkdir(parents=True,exist_ok=True)
        with (self.root/'controller.lock').open('a+') as f:
            fcntl.flock(f,fcntl.LOCK_EX)
            yield

    def _read(self):
        try:
            record=json.loads((self.root/'controller.json').read_text())
        except FileNotFoundError:
            return {}
        except (OSError,ValueError) as e:
            raise ControllerUnavailable('Controller record unreadable') from e
        return record if process_start(record.get('pid')) == record.get('start') and record.get('start') else {}

    def _claim(self):
        if self.closed: return False
        record=self._read()
        if record.get('token')==self.token: return True
        if record and not (self.name=='gfg' and record.get('name')=='gsf'): return False
        target=self.root/'controller.json'
        tmp=target.with_suffix('.tmp')
        tmp.write_text(json.dumps(dict(name=self.name,token=self.token,pid=self.pid,start=self.start)))
        os.replace(tmp,target)
        return True

    def active(self):
        with self._locked(): return self._claim()

    @contextlib.contextmanager
    def command(self):
        with self._locked():
            if not self._claim():
                raise ControllerUnavailable('Another Kedavra controller is active')
            yield

    def release(self):
        with self._locked():
            if self._read().get('token')==self.token:
                (self.root/'controller.json').unlink(missing_ok=True)
            self.closed=True
