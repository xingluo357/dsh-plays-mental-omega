"""mo-hint 管道客户端（纯标准库）。用法: python hint_cli.py [--pipe NAME] say 你要建造发电厂"""
import sys

DEFAULT_PIPE = "mo-hint"


def ask(cmd: str, pipe: str = DEFAULT_PIPE) -> str:
    path = "\\\\.\\pipe\\" + pipe
    with open(path, "r+b", buffering=0) as f:
        f.write(cmd.encode("utf-8") + b"\n")
        chunks = []
        while True:
            b = f.read(65536)
            if not b:
                break
            chunks.append(b)
            if len(b) < 65536:
                break
    return b"".join(chunks).decode("utf-8", "replace").strip()


if __name__ == "__main__":
    args = sys.argv[1:]
    pipe = DEFAULT_PIPE
    if args and args[0] in ("--pipe", "-p"):
        pipe = args[1]
        args = args[2:]
    print(ask(" ".join(args) or "ping", pipe))
