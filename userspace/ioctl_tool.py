#!/usr/bin/env python3
import os
import fcntl
import argparse
import struct

# должен совпадать с kernel header
SIMPLEFS_IOC_MAGIC = ord('s')

IOCTL_CLEAR = 0x00  # _IO(s, 1)
IOCTL_ERASE = 0x01  # _IO(s, 2)
IOCTL_GET_HASHES = 0xC0107303  # _IOWR(s, 3, struct simplefs_hashes_hdr)
IOCTL_GET_MAP = 0xC0287304      # _IOWR(s, 4, struct simplefs_map_req)


HASH_HDR_FMT = "II"  # capacity, count

MAP_FMT = f"{128}sQIIQ"  # name, start, sectors, hash, size


def ioctl_clear(fd):
    fcntl.ioctl(fd, IOCTL_CLEAR)


def ioctl_erase(fd):
    fcntl.ioctl(fd, IOCTL_ERASE)


def ioctl_get_hashes(fd, capacity):
    buf = struct.pack(HASH_HDR_FMT, capacity, 0)
    res = fcntl.ioctl(fd, IOCTL_GET_HASHES, buf)

    capacity, count = struct.unpack(HASH_HDR_FMT, res[:8])
    hashes = struct.unpack(f"{count}I", res[8:8 + count * 4])
    return hashes


def ioctl_get_map(fd, name):
    name_b = name.encode()[:127]
    name_b = name_b + b"\x00" * (128 - len(name_b))

    buf = struct.pack(MAP_FMT, name_b, 0, 0, 0, 0)
    res = fcntl.ioctl(fd, IOCTL_GET_MAP, buf)

    name, start, sectors, hashv, size = struct.unpack(MAP_FMT, res)

    return {
        "name": name.decode(errors="ignore").rstrip("\x00"),
        "start": start,
        "sectors": sectors,
        "hash": hashv,
        "size": size,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dev", required=True)
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("clear")
    sub.add_parser("erase")

    p_hash = sub.add_parser("hashes")
    p_hash.add_argument("--cap", type=int, default=1024)

    p_map = sub.add_parser("map")
    p_map.add_argument("name")

    args = parser.parse_args()

    fd = os.open(args.dev, os.O_RDWR)

    if args.cmd == "clear":
        ioctl_clear(fd)

    elif args.cmd == "erase":
        ioctl_erase(fd)

    elif args.cmd == "hashes":
        print(ioctl_get_hashes(fd, args.cap))

    elif args.cmd == "map":
        print(ioctl_get_map(fd, args.name))

    else:
        print("unknown command")

    os.close(fd)


if __name__ == "__main__":
    main()
