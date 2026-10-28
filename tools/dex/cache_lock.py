"""Cross-process cache publication lock; no shell, service, or global settings."""
from contextlib import contextmanager
import os
from pathlib import Path
import time


def filesystem_path(path):
    """Absolute OS path, including Win32's extended-length drive/UNC syntax.

    The prefix makes file operations independent of LongPathsEnabled; abspath
    normalizes dots and separators before entering the Win32 device namespace.
    """
    path = os.path.abspath(os.fspath(path))
    if os.name == "nt" and not path.startswith("\\\\?\\"):
        path = "\\\\?\\UNC\\" + path[2:] if path.startswith("\\\\") else "\\\\?\\" + path
    return Path(path)


@contextmanager
def exclusive_file(path: Path):
    path = filesystem_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt
            if lock.seek(0, 2) == 0:
                lock.write(b"\0")
                lock.flush()
            deadline = time.monotonic() + 600
            while True:
                try:
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"cache lock timed out: {path.name}")
                    time.sleep(0.1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
