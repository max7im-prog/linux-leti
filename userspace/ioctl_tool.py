import argparse
import fcntl
import struct
import os

# MUST match kernel exactly
SIMPLEFS_IOC_MAGIC = ord('s')

# correct Linux ioctl encoding


def IOC(dir, type, nr, size):
    return (dir << 30) | (type << 8) | nr | (size << 16)


def IO(type, nr):
    return IOC(0, type, nr, 0)


def IOWR(type, nr, size):
    return IOC(2, type, nr, size)


IOCTL_CLEAR = IO(SIMPLEFS_IOC_MAGIC, 1)
IOCTL_ERASE = IO(SIMPLEFS_IOC_MAGIC, 2)
IOCTL_GET_HASHES = IOWR(SIMPLEFS_IOC_MAGIC, 3, 256)
IOCTL_GET_MAP = IOWR(SIMPLEFS_IOC_MAGIC, 4, 256)


def ioctl(fd, cmd, arg=None):
    if arg is None:
        return fcntl.ioctl(fd, cmd)
    return fcntl.ioctl(fd, cmd, arg)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dev", required=True)
    p.add_argument("cmd")
    args = p.parse_args()

    with open(args.dev, "rb+", buffering=0) as f:
        fd = f.fileno()

        if args.cmd == "clear":
            ioctl(fd, IOCTL_CLEAR)
            print("OK clear")

        elif args.cmd == "erase":
            ioctl(fd, IOCTL_ERASE)
            print("OK erase")

        else:
            print("unknown cmd")


if __name__ == "__main__":
    main()
