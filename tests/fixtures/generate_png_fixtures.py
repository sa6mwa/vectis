"""Original two-pixel Adam7 PNG fixture; Python standard library only."""
from pathlib import Path
import struct
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


root = Path(__file__).parent / 'png'
root.mkdir(exist_ok=True)
# For a 2x1 image only Adam7 passes 1 (pixel 0) and 6 (pixel 1) have rows.
pixels = b'\x00\xff\x00\x00\x7f' + b'\x00\x00\x00\xff\xff'
header = struct.pack('>IIBBBBB', 2, 1, 8, 6, 0, 0, 1)
(root / 'interlaced.png').write_bytes(
    b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
    + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))
