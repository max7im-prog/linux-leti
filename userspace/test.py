#!/usr/bin/env python3
import os
import random
import struct

BASE = "/mnt/simplefs"


def list_files():
    return sorted(
        f for f in os.listdir(BASE)
        if f.startswith("file")
    )


def write_read_check(path):
    value = random.randint(1, 10**9)

    data = struct.pack("I", value)

    with open(path, "wb") as f:
        f.write(data)

    with open(path, "rb") as f:
        out = struct.unpack("I", f.read(4))[0]

    if out != value:
        print(f"[FAIL] {path}: {value} != {out}")
    else:
        print(f"[OK]   {path}: {value}")


def main():
    files = list_files()

    print(f"files: {len(files)}")

    for f in files:
        path = os.path.join(BASE, f)
        write_read_check(path)


if __name__ == "__main__":
    main()
