"""A guest process exit must retain status and bypass unrelated host cleanup."""
import subprocess
import sys
from fixture_wait import HANG_SECONDS
from concurrent.futures import ThreadPoolExecutor, TimeoutError

for args, expected in (([], 37), (["retain"], 23), (["grace-after-zero"], 124), (["signal-after-zero"], 139)):
    child = subprocess.run([sys.argv[1], *args], capture_output=True, text=True, timeout=HANG_SECONDS)
    assert child.returncode == expected, child
    assert "unrelated cleanup entered" not in child.stderr, child.stderr
    assert f"Terminal guest process exit status={expected};" in child.stderr, child.stderr
print("PASS 4 terminal exits: explicit status, first nonzero, grace failure after zero, fatal signal after zero")
# A native Vulkan device loss on a guest thread ends the app; a host thread only reports.
for args, expected in ((["device-lost"], 1), (["device-lost-host"], 37)):
    child = subprocess.run([sys.argv[1], *args], capture_output=True, text=True, timeout=HANG_SECONDS)
    assert child.returncode == expected, child
    assert f"Terminal guest process exit status={expected};" in child.stderr, child.stderr
    assert ("ending the app as a fatal error" in child.stderr) == (expected == 1), child.stderr
print("PASS device loss: fatal on a guest thread, report-only on a host thread")
child = subprocess.Popen([sys.argv[1], "blocked-logger"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
reader = ThreadPoolExecutor(1)
try:
    # A prefix proves the writer acquired the logger lock. Leave the remaining
    # megabyte unread so it cannot finish before the exit request.
    prefix = reader.submit(child.stderr.read, 128)
    try:
        assert prefix.result(timeout=HANG_SECONDS), f"logger produced no prefix: exit={child.poll()}"
    except TimeoutError:
        raise AssertionError(f"logger did not acquire its lock: exit={child.poll()}") from None
    child.stdin.write(b"G"); child.stdin.flush()
    try:
        assert child.wait(timeout=HANG_SECONDS) == 37, f"terminal status changed: exit={child.returncode}"
    except subprocess.TimeoutExpired:
        raise AssertionError(f"terminal exit blocked after logger prefix and exit request: exit={child.poll()}") from None
finally:
    if child.poll() is None:
        import psutil
        assert psutil.Process(child.pid).cmdline() == [sys.argv[1], "blocked-logger"]
        child.kill(); child.wait(timeout=HANG_SECONDS)
    reader.shutdown(wait=True)
    child.stdin.close(); child.stdout.close(); child.stderr.close()
print("PASS terminal exit with another thread blocked holding logging/stdio locks")
