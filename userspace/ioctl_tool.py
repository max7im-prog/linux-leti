import argparse
import fcntl
import struct

# ===== must match kernel =====
SIMPLEFS_IOC_MAGIC = ord('s')


def _io(magic, nr):
    return (magic << 8) | nr


IOCTL_CLEAR = _io(SIMPLEFS_IOC_MAGIC, 1)
IOCTL_ERASE = _io(SIMPLEFS_IOC_MAGIC, 2)

# _IOWR in Linux:


def _iowr(magic, nr, size):
    return (magic << 8) | (nr) | (size << 16)


IOCTL_GET_HASHES = _iowr(SIMPLEFS_IOC_MAGIC, 3, 8)
IOCTL_GET_MAP = _iowr(SIMPLEFS_IOC_MAGIC, 4, 128)


def ioctl_clear(fd):
    fcntl.ioctl(fd, IOCTL_CLEAR)


def ioctl_erase(fd):
    fcntl.ioctl(fd, IOCTL_ERASE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dev", required=True)
    parser.add_argument("cmd")

    args = parser.parse_args()

    with open(args.dev, "rb+", buffering=0) as f:
        fd = f.fileno()

        if args.cmd == "clear":
            ioctl_clear(fd)
            print("OK clear")

        elif args.cmd == "erase":
            ioctl_erase(fd)
            print("OK erase")

        else:
            print("unknown cmd")


if __name__ == "__main__":
    main()
