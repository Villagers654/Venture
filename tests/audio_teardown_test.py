import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='venture-audio-teardown-') as directory:
    events = Path(directory) / 'events.jsonl'
    environment = dict(os.environ, VENTURE_AV_TELEMETRY=str(events))
    result = subprocess.run([str(binary)], env=environment, capture_output=True, timeout=30)
    assert result.returncode == 0, f'static teardown exit status {result.returncode}'
    rows = [json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()]
    assert [row['kind'] for row in rows] == ['before_static_teardown', 'audio_stats'], rows
    assert rows[-1]['streams'] == [], rows[-1]
    # Default operation does not start an optional writer or open a device.
    events.unlink()
    environment['VENTURE_AV_TELEMETRY'] = ''
    result = subprocess.run([str(binary)], env=environment, capture_output=True, timeout=30)
    assert result.returncode == 0, f'disabled telemetry exit status {result.returncode}'
    assert not events.exists()
