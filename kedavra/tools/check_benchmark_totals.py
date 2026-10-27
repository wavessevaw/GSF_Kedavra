"""Validate kfg_vktest benchmark accounting from an actual GPU run log."""
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding='utf-8')
passes = re.findall(r'^\s+(\S+)\s+([0-9.]+) \(p99 [0-9.]+\)\s*$', text, re.M)
summary = re.search(r'analysis ([0-9.]+) ms, two outputs ([0-9.]+) ms', text)
if not summary or not passes or not any(name == 'global' for name, _ in passes):
    raise SystemExit('missing benchmark passes or global motion')
analysis = sum(float(value) for name, value in passes if not name.startswith(('g1:', 'g2:')))
generation = sum(float(value) for name, value in passes if name.startswith(('g1:', 'g2:')))
if not any(name.startswith('g1:') for name, _ in passes) or not any(name.startswith('g2:') for name, _ in passes):
    raise SystemExit('missing generation passes')
# Each printed pass was rounded to 0.001 ms; allow accumulated print rounding.
tolerance = 0.0005 * (len(passes) + 1) + 1e-8
if abs(analysis-float(summary[1])) > tolerance or abs(generation-float(summary[2])) > tolerance:
    raise SystemExit(f'incorrect summary: passes analysis={analysis:.3f}, generation={generation:.3f}; '
                     f'summary analysis={summary[1]}, generation={summary[2]}')
print(f'PASS: analysis={analysis:.3f} ms, generation={generation:.3f} ms; {len(passes)} passes')
