"""Map GTA SA 1.0 US code/data addresses to another exe version by masked byte-pattern matching.

For each 1.0 address, a window of code bytes is taken from the 1.0 exe; 4-byte absolute addresses
(0x400000..0x1600000) and rel32 call/jmp displacements are wildcarded; the pattern is searched in the
other exe's code. A unique match gives the new code address, and the absolute operands at the same
offsets give data-address pairs (1.0 -> other).

Usage: python xver.py <gta_sa_10.exe> <other.exe> <spec.txt>
spec lines: "<name> <hex 1.0 address> [window bytes] [back bytes]"   (back: start the window before the address)
"""
import re
import struct
import sys


class PE:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        n = struct.unpack_from('<H', d, pe + 6)[0]
        opt = struct.unpack_from('<H', d, pe + 20)[0]
        self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
        self.secs = []
        for i in range(n):
            o = pe + 24 + opt + 40 * i
            vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
            ch = struct.unpack_from('<I', d, o + 36)[0]
            self.secs.append((d[o:o + 8].rstrip(b'\0').decode(), self.base + va, vs, ro, rs, ch))

    def read(self, va, n):
        for name, a, vs, ro, rs, ch in self.secs:
            if a <= va < a + rs:
                return self.data[ro + va - a: ro + va - a + n]
        return None

    def code_ranges(self):
        for name, a, vs, ro, rs, ch in self.secs:
            if ch & 0x20000000:   # executable
                yield a, self.data[ro: ro + rs]


def build_pattern(code):
    pat = []
    abs_slots = []
    i = 0
    while i < len(code):
        b = code[i]
        if b in (0xE8, 0xE9) and i + 5 <= len(code):
            pat.append(re.escape(bytes([b])) + b'....')
            i += 5
            continue
        if i + 4 <= len(code):
            v = struct.unpack_from('<I', code, i)[0]
            if 0x401000 <= v < 0x1600000:
                pat.append(b'....')
                abs_slots.append((i, v))
                i += 4
                continue
        pat.append(re.escape(bytes([b])))
        i += 1
    return re.compile(b''.join(pat), re.DOTALL), abs_slots


def main():
    a, b = PE(sys.argv[1]), PE(sys.argv[2])
    for line in open(sys.argv[3]):
        line = line.split('#')[0].split()
        if not line:
            continue
        name, va = line[0], int(line[1], 16)
        win = int(line[2]) if len(line) > 2 else 40
        back = int(line[3]) if len(line) > 3 else 0
        code = a.read(va - back, win)
        rx, slots = build_pattern(code)
        hits = []
        for base, blob in b.code_ranges():
            for m in rx.finditer(blob):
                hits.append(base + m.start())
        if len(hits) != 1:
            print(f'{name:28s} 1.0 0x{va:08X}: {len(hits)} matches ' + ' '.join(f'0x{h:X}' for h in hits[:6]))
            continue
        h = hits[0] + back
        pairs = []
        for off, v in slots:
            nv = struct.unpack_from('<I', b.read(hits[0] + off, 4))[0]
            pairs.append(f'{v:X}->{nv:X}')
        print(f'{name:28s} 1.0 0x{va:08X} -> 0x{h:08X} (delta {h - va:+#x})  ' + ' '.join(pairs))


main()
