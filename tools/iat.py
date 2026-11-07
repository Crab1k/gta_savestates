"""Print IAT slot addresses for named imports, from `dumpbin /imports` output.
Usage: python iat.py <dumpbin-imports.txt> <name>...
"""
import re
import sys

lines = open(sys.argv[1]).read().splitlines()
dll = None
base = None
idx = 0
slots = {}
for l in lines:
    m = re.match(r'^\s{4}(\S+\.(dll|DLL))\s*$', l)
    if m:
        dll, base, idx = m.group(1), None, 0
        continue
    m = re.match(r'^\s+([0-9A-F]+) Import Address Table', l)
    if m:
        base = int(m.group(1), 16)
        continue
    m = re.match(r'^\s+([0-9A-F]+)\s+(\S+)\s*$', l)
    if m and base is not None and dll:
        slots[(dll.lower(), m.group(2))] = base + 4 * idx
        idx += 1
for want in sys.argv[2:]:
    for (d, n), a in slots.items():
        if n == want:
            print(f"{d:16s} {n:24s} 0x{a:08X}")
