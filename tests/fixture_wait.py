"""Observable fixture progress with a hang watchdog, independent of host speed."""
import time
import sys

HANG_SECONDS = 180


def policy_poll_command(script, arguments):
    """Shorten polling in private fixtures; clocks and real deadlines stay intact."""
    return [sys.executable, "-c",
            "import os,runpy,sys,time; sleep=time.sleep; "
            "time.sleep=lambda seconds:sleep(min(seconds,.05)); "
            "sys.argv=sys.argv[1:]; sys.path.insert(0,os.path.dirname(sys.argv[0])); "
            "runpy.run_path(sys.argv[0],run_name='__main__')",
            str(script), *map(str, arguments)]


def wait_for(check, label, seconds=HANG_SECONDS, *, process=None):
    started = time.monotonic()
    while True:
        # Inspect progress first: a descheduled observer must accept an event
        # that already happened, even if it resumes after the watchdog bound.
        value = check()
        if value:
            print(f'[fixture-wait] {label}: {time.monotonic() - started:.3f}s', flush=True)
            return value
        if process is not None and process.poll() is not None:
            raise AssertionError(f'{label}: child exited with status {process.returncode}')
        elapsed = time.monotonic() - started
        if elapsed >= seconds:
            raise AssertionError(f'No progress: {label} (after {elapsed:.3f}s)')
        time.sleep(min(.05, seconds - elapsed))


def wait_for_exit_progress(process, observe, label, *, seconds=HANG_SECONDS, outer_seconds=900):
    """Bound stalled stages, rather than charging earlier progress to later work.

    observe returns (progress token, diagnostic stage). Tokens must describe
    actual work, not heartbeat/owner records or merely elapsed CPU time.
    """
    began = last_progress = time.monotonic()
    previous = object()
    while True:
        token, stage = observe()
        now = time.monotonic()
        if token != previous:
            previous, last_progress = token, now
            print(f'[fixture-progress] {label}: {stage} ({now - began:.3f}s)', flush=True)
        code = process.poll()
        if code is not None:
            return code
        if now - last_progress >= seconds:
            raise AssertionError(f'No progress: {label}: {stage} '
                                 f'(stalled {now - last_progress:.3f}s, total {now - began:.3f}s)')
        if now - began >= outer_seconds:
            raise AssertionError(f'Hang guard: {label}: {stage} (total {now - began:.3f}s)')
        time.sleep(.05)
