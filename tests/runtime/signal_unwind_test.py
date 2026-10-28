"""Real NDK libunwind/libc++ against translated signal frames and HLE waits."""
import json
from pathlib import Path
import subprocess
import sys


def main():
    runner, root = Path(sys.argv[1]), Path(sys.argv[2])
    if any(not (root / abi / "libsignal_unwind.so").exists() for abi in ("a64", "a32")):
        print("NDK fixtures missing: provision with tools/bootstrap.py --ndk, then rebuild")
        return 77
    rows = []
    for abi, modes in (("a64", range(10)), ("a32", (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11))):
        for mode in modes:
            completed = subprocess.run([str(runner.resolve()), str((root / abi / "libsignal_unwind.so").resolve()), str(mode)],
                                       capture_output=True, text=True, timeout=15)
            (root / f"{abi}-{mode}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
            lines = [line for line in completed.stdout.splitlines() if line.startswith("RESULT ")]
            assert completed.returncode == 0 and len(lines) == 1, (abi, mode, completed.returncode, completed.stderr)
            status, caught, destroyed, joined, owned, frames, interrupted, handler, layout = map(int, lines[0].split()[1:])
            assert status == 0 and joined == 1, (abi, mode, lines)
            if mode in (0, 2, 3, 6, 7, 9, 10, 11):
                assert (caught, destroyed, owned) == (1, 1, 1), (abi, mode, lines)
            else:
                assert frames >= 3 and interrupted >= 1 and (handler, layout) == (1, 1), (abi, mode, lines)
                if mode in (4, 8):
                    assert (destroyed, owned) == (1, 1), (abi, mode, lines)
            rows.append({"abi": abi, "mode": mode, "result": lines[0]})
            print(abi, mode, lines[0], flush=True)
    for abi in ("a64", "a32"):
        for mode in (0, 1):
            completed = subprocess.run([str(runner.resolve()), str((root / abi / "libcond_signal_ack.so").resolve()), str(mode)],
                                       capture_output=True, text=True, timeout=15)
            (root / f"ack-{abi}-{mode}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
            assert completed.returncode == 0 and 'RESULT 0 1 2 1' in completed.stdout, (abi, mode, completed.stdout, completed.stderr)
            rows.append({"abi": abi, "ack_mode": mode, "result": completed.stdout.strip()})
            print(abi, 'ack', mode, completed.stdout.strip(), flush=True)
    (root / "results.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
