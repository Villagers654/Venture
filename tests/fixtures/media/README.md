# Venture-authored audio fixture

`tone-aac.mp4` is a 0.25-second, 1 kHz synthetic sine wave at 48 kHz stereo.
It contains no game or third-party recording. It was authored with this command:

```text
ffmpeg -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=0.25 -ac 2 -c:a aac -b:a 64000 -movflags +faststart tone-aac.mp4
```

That authoring tool is not bundled or used by Venture. The runtime dependency
has no encoder, muxer, filter, program or protocol enabled. Tests decode this
tiny committed fixture through the same bounded AVIO and AAC paths as guests.

pattern-h264.mp4 is a synthetic FFmpeg testsrc2 pattern, 128x72 at 12 fps for one second, encoded with libx264 (yuv420p, two B frames, GOP 12, faststart). It contains no game content.

`pattern-vp9.webm` is the same synthetic testsrc2 pattern encoded with libvpx-vp9 (128x72, 12 fps, one second, one encoder thread). It exercises WebM/container duration fallback and VP9 RGBA output.

`pattern-av.mp4` combines the one-second H.264 pattern with the shorter AAC
tone, using stream copy. It verifies that video stays on consumed audio time
and continues through silence until its own end, without any game files.
