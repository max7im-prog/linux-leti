import argparse
import fcntl

# must match kernel exactly
SIMPLEFS_IOC_MAGIC = ord('s')

# Linux ioctl encoding (same as kernel _IO)


def IO(nr):
    return (SIMPLEFS_IOC_MAGIC << 8) | nr


def IOWR(nr):
    return (SIMPLEFS_IOC_MAGIC << 8) | nr


IOCTL_CLEAR = IO(1)
IOCTL_ERASE = IO(2)
IOCTL_GET_HASHES = IOWR(3)
IOCTL_GET_MAP = IOWR(4)


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
