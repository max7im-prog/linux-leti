#!/usr/bin/env python3
import argparse
import fcntl
import os
import stat
import struct
import sys
from typing import List

SIMPLEFS_IOC_MAGIC = ord("s")

HASH_HDR_FMT = "<II"          # capacity, count
HASH_HDR_SIZE = struct.calcsize(HASH_HDR_FMT)

MAP_FMT = "<128sQIIQ"         # name, start_sector, sector_count, hash, size
MAP_SIZE = struct.calcsize(MAP_FMT)

# Linux ioctl encoding


def _IOC(direction: int, magic: int, nr: int, size: int) -> int:
    return ((direction & 0x3) << 30) | ((size & 0x3FFF) << 16) | ((magic & 0xFF) << 8) | (nr & 0xFF)


def _IO(magic: int, nr: int) -> int:
    return _IOC(0, magic, nr, 0)


def _IOWR(magic: int, nr: int, size: int) -> int:
    return _IOC(3, magic, nr, size)


IOCTL_CLEAR = _IO(SIMPLEFS_IOC_MAGIC, 1)
IOCTL_ERASE = _IO(SIMPLEFS_IOC_MAGIC, 2)
IOCTL_GET_HASHES = _IOWR(SIMPLEFS_IOC_MAGIC, 3, HASH_HDR_SIZE)
IOCTL_GET_MAP = _IOWR(SIMPLEFS_IOC_MAGIC, 4, MAP_SIZE)


def open_target(path: str) -> int:
    st = os.stat(path)
    if stat.S_ISDIR(st.st_mode):
        flags = os.O_RDONLY | os.O_DIRECTORY
    else:
        flags = os.O_RDWR
    return os.open(path, flags)


def do_clear(fd: int) -> None:
    fcntl.ioctl(fd, IOCTL_CLEAR)


def do_erase(fd: int) -> None:
    fcntl.ioctl(fd, IOCTL_ERASE)


def do_get_hashes(fd: int, capacity: int) -> List[int]:
    if capacity <= 0:
        raise ValueError("capacity must be > 0")

    buf = bytearray(HASH_HDR_SIZE + capacity * 4)
    struct.pack_into(HASH_HDR_FMT, buf, 0, capacity, 0)

    fcntl.ioctl(fd, IOCTL_GET_HASHES, buf, True)

    _, count = struct.unpack_from(HASH_HDR_FMT, buf, 0)
    count = min(count, capacity)

    if count == 0:
        return []

    hashes = list(struct.unpack_from(f"<{count}I", buf, HASH_HDR_SIZE))
    return hashes


def do_get_map(fd: int, name: str):
    name_bytes = name.encode("utf-8")[:127]
    name_bytes = name_bytes.ljust(128, b"\x00")

    buf = bytearray(MAP_SIZE)
    struct.pack_into(MAP_FMT, buf, 0, name_bytes, 0, 0, 0, 0)

    fcntl.ioctl(fd, IOCTL_GET_MAP, buf, True)

    raw_name, start_sector, sector_count, hashv, size = struct.unpack_from(
        MAP_FMT, buf, 0)
    decoded_name = raw_name.split(
        b"\x00", 1)[0].decode("utf-8", errors="replace")

    return {
        "name": decoded_name,
        "start_sector": start_sector,
        "sector_count": sector_count,
        "hash": hashv,
        "size": size,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="SimpleFS ioctl tool")
    parser.add_argument(
        "--target", "--dev",
        dest="target",
        required=True,
        help="Mounted SimpleFS path (preferably /mnt/simplefs) or a file inside it"
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("clear", help="Zero all files")
    sub.add_parser("erase", help="Erase FS data and superblocks")

    p_hash = sub.add_parser("hashes", help="Print hashes of all files")
    p_hash.add_argument("--cap", type=int, default=20000,
                        help="Buffer capacity (number of hashes)")

    p_map = sub.add_parser("map", help="Print sector mapping for a file")
    p_map.add_argument("name", help="File name, e.g. file00001")

    args = parser.parse_args()

    fd = None
    try:
        fd = open_target(args.target)

        if args.cmd == "clear":
            do_clear(fd)
            print("OK: cleared all files")

        elif args.cmd == "erase":
            do_erase(fd)
            print("OK: erased FS (superblocks + files)")

        elif args.cmd == "hashes":
            hashes = do_get_hashes(fd, args.cap)
            for i, h in enumerate(hashes):
                print(f"{i}: {h:#010x}")

        elif args.cmd == "map":
            info = do_get_map(fd, args.name)
            print(f"name        : {info['name']}")
            print(f"start_sector : {info['start_sector']}")
            print(f"sector_count : {info['sector_count']}")
            print(f"hash         : {info['hash']:#010x}")
            print(f"size         : {info['size']}")

        return 0

    finally:
        if fd is not None:
            os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
