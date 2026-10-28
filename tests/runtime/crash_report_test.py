"""Exercise real host faults in disposable subprocesses, never the test driver."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

child, output = sys.argv[1:]
child = str(Path(child).resolve())
root = Path(output).resolve()
root.mkdir(parents=True, exist_ok=True)
# Each CTest invocation uses its own evidence directory; retain it for review.
root = root / str(time.time_ns())
root.mkdir()
env = dict(os.environ, VENTURE_DATA_DIR=str(root / "data"))
env['VENTURE_JIT_FRAME_CHAIN'] = '0'


def run(mode, index):
    log = root / f"{mode}-{index}.log"
    process = subprocess.run([child, str(log), mode, "0"], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
    text = log.read_text(encoding="utf-8")
    assert "earlier buffered info survives" in text, text
    return process.returncode, text


code, text = run("guest", 0)
assert code == 0, code
assert "Host exception" not in text
assert not list(root.glob("*.dmp"))

for index in range(5 if os.name == "nt" else 1):
    code, text = run("host", index)
    expected = 0xC0000005 if os.name == "nt" else -11
    assert (code & 0xFFFFFFFF if os.name == "nt" else code) == expected, (code, text)
    assert re.search(r"E \[runtime\] Host exception 0x[0-9a-fA-F]+ at .+\+0x[0-9a-f]+ \(thread \d+\)", text), text
    assert "E [host-stack]   #0 " in text, text
    if os.name == "nt":
        match = re.search(r"I \[runtime\] Crash dump: (.+)", text)
        assert match, text
        dump = Path(match[1])
        assert dump.parent == root
        with dump.open("rb") as stream:
            header = stream.read(32)
        assert len(header) == 32 and header[:4] == b"MDMP", header
        _, _, streams, directory, _, _, flags = struct.unpack("<IIIIIIQ", header)
        assert streams > 0 and directory >= 32 and directory < dump.stat().st_size
        assert flags & 0x1050 == 0x1050 and not flags & 0x2, flags
        with dump.open("rb") as stream:
            stream.seek(directory)
            entries = [struct.unpack("<III", stream.read(12)) for _ in range(streams)]
            exception_stream = next(entry for entry in entries if entry[0] == 6)
            stream.seek(exception_stream[2] + 8)
            assert struct.unpack("<I", stream.read(4))[0] == 0xC0000005
        assert len(list(root.glob("*.dmp"))) == min(index + 1, 3)

for mode in ("native-host", "native-source", "native-host-compare"):
    code, text = run(mode, 0)
    expected = 0xC0000005 if os.name == "nt" else -11
    assert (code & 0xFFFFFFFF if os.name == "nt" else code) == expected, (code, text)
    assert "Host exception" in text and "E [host-stack]   #0 " in text, text
if os.name == 'nt':
    for mode in ('jit64', 'jit32'):
        code, text = run(mode, 0)
        assert code & 0xFFFFFFFF == 0xC0000005, (code, text)
        assert 'guest thread dump: frame-chain crash fixture' in text, text
        assert re.search(r'E \[signal\]   #00 ', text), text
        assert re.search(r'E \[signal\]   #01 ', text), text
        hosts = re.findall(r'E \[host-stack\]   #(\d+) ([^\n]+)', text)
        # Walk through at least one anonymous translated/helper frame, then
        # recover a native image frame beyond it using registered unwind data.
        anonymous = [int(i) for i, frame in hosts if frame.startswith('unknown+')]
        assert anonymous, text
        assert any(int(i) > min(anonymous) and not frame.startswith('unknown+')
                   for i, frame in hosts), text
        assert 'Crash dump:' in text, text

before = set(root.glob("*.dmp"))
code, text = run("guest", 1)
assert code == 0 and "Host exception" not in text
assert set(root.glob("*.dmp")) == before
print(f"Crash report evidence: {root}")
