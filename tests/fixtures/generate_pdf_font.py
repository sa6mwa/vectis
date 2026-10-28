"""Generate an original tiny TrueType fixture using only the standard library.

The mapped glyphs A and U+4E2D are rectangles. No third-party font data is used.
This prepares test data; it does not run builds or tests.
"""

from pathlib import Path
import struct


def pack(fmt, *values):
    return struct.pack(">" + fmt, *values)


def checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(padded) // 4), padded)) & 0xFFFFFFFF


def main():
    tables = {}
    tables["head"] = pack(
        "IIIIHHQQhhhhHHhhh",
        0x10000, 0x10000, 0, 0x5F0F3CF5, 3, 1000, 0, 0,
        0, 0, 500, 700, 0, 8, 2, 0, 0,
    )
    tables["hhea"] = pack(
        "IhhhHhhhhhhhhhhhH", 0x10000, 800, -200, 0, 600,
        0, 100, 500, 1, 0, 0, 0, 0, 0, 0, 0, 2,
    )
    tables["maxp"] = pack("I14H", 0x10000, 2, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
    tables["hmtx"] = pack("HhHh", 600, 0, 600, 0)
    glyph = pack("hhhhhHH", 1, 0, 0, 500, 700, 3, 0)
    glyph += bytes([1, 1, 1, 1]) + pack("4h", 0, 500, 0, -500)
    glyph += pack("4h", 0, 0, 700, 0)
    tables["glyf"] = glyph
    tables["loca"] = pack("3H", 0, 0, len(glyph) // 2)
    cmap = pack("7H", 4, 40, 0, 6, 4, 1, 2)
    cmap += pack("4H", 65, 0x4E2D, 65535, 0)
    cmap += pack("3H3h3H", 65, 0x4E2D, 65535, -64, 1-0x4E2D, 1, 0, 0, 0)
    tables["cmap"] = pack("HHHHI", 0, 1, 3, 1, 12) + cmap
    names = {
        1: "Vectis Test", 2: "Regular", 3: "VectisTest-Regular-1",
        4: "Vectis Test Regular", 5: "Version 1.0", 6: "VectisTest-Regular",
    }
    records, strings = b"", b""
    for key, value in names.items():
        encoded = value.encode("utf-16-be")
        records += pack("6H", 3, 1, 0x409, key, len(encoded), len(strings))
        strings += encoded
    tables["name"] = pack("3H", 0, len(names), 6 + len(records)) + records + strings
    tables["post"] = pack("IIhhIIIII", 0x30000, 0, -75, 50, 0, 0, 0, 0, 0)
    os2 = pack("HhHHH", 0, 600, 400, 5, 0)
    os2 += pack("11h", 650, 600, 0, 75, 650, 600, 0, 350, 50, 250, 0)
    os2 += bytes(10) + pack("4I", 1, 1 << 27, 0, 0) + b"VECT"
    os2 += pack("3H3h2H", 0x40, 65, 0x4E2D, 800, -200, 0, 800, 200)
    tables["OS/2"] = os2
    count = len(tables)
    power = 1 << (count.bit_length() - 1)
    header = pack("I4H", 0x10000, count, power * 16, power.bit_length() - 1,
                  count * 16 - power * 16)
    directory, payload, offset = b"", b"", 12 + count * 16
    head_offset = None
    for tag, data in sorted(tables.items()):
        directory += tag.encode("ascii") + pack("3I", checksum(data), offset, len(data))
        if tag == "head":
            head_offset = offset
        padded = data + b"\0" * (-len(data) % 4)
        payload += padded
        offset += len(padded)
    font = bytearray(header + directory + payload)
    adjustment = (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF
    font[head_offset + 8:head_offset + 12] = pack("I", adjustment)
    output = Path(__file__).parent / "pdf"
    output.mkdir(exist_ok=True)
    (output / "VectisTest.ttf").write_bytes(font)


if __name__ == "__main__":
    main()
