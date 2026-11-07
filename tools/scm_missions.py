"""List the missions in a GTA SA main.scm: index, offset, SCRIPT_NAME (03A4).
Usage: python scm_missions.py <main.scm>
"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()


def jump_target(off):
    # segment header starts with GOTO (0002) + int32 param: 02 00 01 <int32>
    assert data[off:off + 3] == b'\x02\x00\x01', hex(off)
    return struct.unpack_from('<i', data, off + 3)[0]


seg2 = jump_target(0)           # after global variables
seg3 = jump_target(seg2)        # after model names
seg4 = jump_target(seg3)        # after mission table
p = seg3 + 8                    # skip jump (7 bytes) + segment id byte
main_size, largest_mission, n_missions, n_exclusive = struct.unpack_from('<IIHH', data, p)
p += 12
offsets = struct.unpack_from('<%dI' % n_missions, data, p)
print(f'main size {main_size}, {n_missions} missions')
for i, off in enumerate(offsets):
    name = '?'
    for k in range(off, min(off + 64, len(data) - 12)):
        if data[k:k + 2] == b'\xa4\x03' and data[k + 2] == 0x09:
            name = data[k + 3:k + 11].split(b'\0')[0].decode('latin1')
            break
    print(f'{i:3d} 0x{off:06X} {name}')
