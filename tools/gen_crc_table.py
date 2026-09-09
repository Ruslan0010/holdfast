#!/usr/bin/env python3
"""Generate the CRC-32 lookup table embedded in firmware/src/crc32.c.

Reflected form, polynomial 0xEDB88320. Run it and paste the output; the
firmware test suite checks the embedded table against a bit-serial CRC so a
transcription error is caught.
"""
POLY = 0xEDB88320


def table():
    out = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ POLY if c & 1 else c >> 1
        out.append(c)
    return out


if __name__ == "__main__":
    t = table()
    for i in range(0, 256, 6):
        print("    " + ", ".join(f"0x{v:08X}" for v in t[i:i + 6]) + ",")
