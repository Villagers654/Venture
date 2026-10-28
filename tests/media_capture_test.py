"""Check a real source-triggered host WAV and telemetry marker in an isolated process."""
from pathlib import Path
import array
import json
import math
import os
import subprocess
import sys
import tempfile
import wave

root=Path(__file__).resolve().parents[1]
output=root/'build/media-tests'
output.mkdir(parents=True,exist_ok=True)
if not output.resolve().is_relative_to(root.resolve()):
    raise RuntimeError('Media capture test root escaped the workspace')
directory=Path(tempfile.mkdtemp(prefix='media-capture-',dir=output)).resolve()
env=dict(os.environ)
env.update(VENTURE_AUDIO_CAPTURE_WAV=str(directory/'mix.wav'),VENTURE_AUDIO_CAPTURE_SECONDS='0.1',
    VENTURE_AUDIO_CAPTURE_AFTER_API='MediaPlayer',VENTURE_AUDIO_CAPTURE_API_DELAY='0.02',
    VENTURE_AUDIO_CAPTURE_SOURCE_SHA256='73bca38a087a9d2017d4e576d572bc0d21de3039269631234a382fe641884507',
    VENTURE_AV_TELEMETRY=str(directory/'events.jsonl'))
subprocess.run([sys.argv[1],'--no-skip=true',
    '--test-case=source-triggered host capture uses the actual decoded media stream'],env=env,check=True)
events=[json.loads(line) for line in (directory/'events.jsonl').read_text().splitlines()]
markers=[e for e in events if e.get('kind')=='audio_capture_window']
assert len(markers)==1 and markers[0]['source_id']>0
started=[e for e in events if e.get('kind')=='media_audio_start']
assert started and started[0]['source_id']==markers[0]['source_id']
assert started[0]['source_sha256']==env['VENTURE_AUDIO_CAPTURE_SOURCE_SHA256']
with wave.open(str(directory/'mix.wav'),'rb') as audio:
    assert (audio.getframerate(),audio.getnchannels(),audio.getsampwidth())==(48000,2,2)
    seconds=audio.getnframes()/48000
    pcm=array.array('h',audio.readframes(audio.getnframes()))
    if sys.byteorder!='little':pcm.byteswap()
# The test's Null device is paced by OS timers (about 15.6 ms on Windows), so the window can end a little early.
assert 0.08<=seconds<=0.11
assert math.sqrt(sum(v*v for v in pcm)/len(pcm))/32768>0.02
print('Decoded media source identity, trigger, and audible host WAV verified')
