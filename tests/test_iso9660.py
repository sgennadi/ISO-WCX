#!/usr/bin/env python3
"""Synthetic ISO9660 fixture test; no external image-generation tools needed."""
import pathlib
import subprocess
import sys
import tempfile

SECTOR = 2048

def record(lba, size, flags, name):
    n = len(name)
    length = 33 + n + (0 if n % 2 else 1)
    b = bytearray(length)
    b[0] = length
    b[2:6] = lba.to_bytes(4, "little")
    b[6:10] = lba.to_bytes(4, "big")
    b[10:14] = size.to_bytes(4, "little")
    b[14:18] = size.to_bytes(4, "big")
    b[25] = flags
    b[28:30] = (1).to_bytes(2, "little")
    b[30:32] = (1).to_bytes(2, "big")
    b[32] = n
    b[33:33+n] = name
    return b

def main(executable):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        image = bytearray(24 * SECTOR)
        pvd = 16 * SECTOR
        image[pvd] = 1
        image[pvd+1:pvd+6] = b"CD001"
        image[pvd+6] = 1
        image[pvd+156:pvd+190] = record(20, SECTOR, 2, b"\0")
        end = 17 * SECTOR
        image[end] = 255
        image[end+1:end+6] = b"CD001"
        image[end+6] = 1
        directory = (record(20, SECTOR, 2, b"\0") +
                     record(20, SECTOR, 2, b"\1") +
                     record(21, 5, 0, b"HELLO.TXT;1"))
        image[20*SECTOR:20*SECTOR+len(directory)] = directory
        image[21*SECTOR:21*SECTOR+5] = b"world"
        source = tmp / "test.iso"
        source.write_bytes(image)
        target = tmp / "output"
        result = subprocess.run([executable, str(source), "--extract", str(target)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (target / "HELLO.TXT").read_bytes() == b"world"
        assert "HELLO.TXT" in result.stdout
        print("ISO9660 extraction smoke test passed")

if __name__ == "__main__":
    main(sys.argv[1])
