# Media NDK integration

The initial backend implements AMediaFormat containers and data symbols. Codec,
image-reader and muxer creation fails explicitly; extractor setDataSource fails
with AMEDIA_ERROR_UNSUPPORTED. No media output is claimed. Optional symbols can
resolve, but the null/error creation results must be respected by guest code.
There is no startup trace proving any game needs decoding to reach its first frame.

Recommended next backends: Media Foundation on Windows, and FFmpeg through
system libraries on Linux. Put backend and OS calls behind runtime/platform;
keep guest fd I/O on the libc guest fd table. Media Foundation supplies decode,
encode and demux without redistributing codecs; FFmpeg has broad format coverage.
Both need explicit GPU image transfer into the shared Vulkan device. AImageReader
requires an actual producer/consumer queue and fences before it can be advertised.
Backend references: [Media Foundation](https://learn.microsoft.com/en-us/windows/win32/medfound/microsoft-media-foundation-sdk),
[libavcodec](https://ffmpeg.org/libavcodec.html), [libavformat](https://ffmpeg.org/libavformat.html).

Measured imports below come from the 2026-10-08 r6-ndk symbols.csv survey of the
nine owned downloads. This is import coverage, not evidence of startup execution.
First Contact uses ARM32; this implementation is AArch64 only.

## com.AnotherAxiom.GorillaTag

`AImageReader_acquireLatestImage`, `AImageReader_delete`, `AImageReader_getWindow`, `AImageReader_newWithUsage (weak)`, `AImageReader_setBufferRemovedListener (weak)`, `AImageReader_setImageListener`, `AImage_delete`, `AImage_deleteAsync (weak)`, `AImage_getHardwareBuffer (weak)`, `AImage_getTimestamp`, `AImage_getWidth`, `AMEDIAFORMAT_KEY_CHANNEL_COUNT`, `AMEDIAFORMAT_KEY_COLOR_FORMAT`, `AMEDIAFORMAT_KEY_COLOR_RANGE (weak)`, `AMEDIAFORMAT_KEY_COLOR_STANDARD (weak)`, `AMEDIAFORMAT_KEY_DURATION`, `AMEDIAFORMAT_KEY_ENCODER_DELAY (weak)`, `AMEDIAFORMAT_KEY_FRAME_RATE`, `AMEDIAFORMAT_KEY_HEIGHT`, `AMEDIAFORMAT_KEY_LANGUAGE`, `AMEDIAFORMAT_KEY_MIME`, `AMEDIAFORMAT_KEY_ROTATION (weak)`, `AMEDIAFORMAT_KEY_SAMPLE_RATE`, `AMEDIAFORMAT_KEY_SLICE_HEIGHT (weak)`, `AMEDIAFORMAT_KEY_STRIDE`, `AMEDIAFORMAT_KEY_WIDTH`, `AMediaCodec_configure`, `AMediaCodec_createDecoderByType`, `AMediaCodec_createEncoderByType`, `AMediaCodec_createInputSurface`, `AMediaCodec_delete`, `AMediaCodec_dequeueInputBuffer`, `AMediaCodec_dequeueOutputBuffer`, `AMediaCodec_flush`, `AMediaCodec_getInputBuffer`, `AMediaCodec_getOutputBuffer`, `AMediaCodec_getOutputFormat`, `AMediaCodec_queueInputBuffer`, `AMediaCodec_releaseOutputBuffer`, `AMediaCodec_setOutputSurface (weak)`, `AMediaCodec_signalEndOfInputStream`, `AMediaCodec_start`, `AMediaCodec_stop`, `AMediaDataSource_delete (weak)`, `AMediaDataSource_new (weak)`, `AMediaDataSource_setClose (weak)`, `AMediaDataSource_setGetSize (weak)`, `AMediaDataSource_setReadAt (weak)`, `AMediaDataSource_setUserdata (weak)`, `AMediaExtractor_advance`, `AMediaExtractor_delete`, `AMediaExtractor_getSampleTime`, `AMediaExtractor_getSampleTrackIndex`, `AMediaExtractor_getTrackCount`, `AMediaExtractor_getTrackFormat`, `AMediaExtractor_new`, `AMediaExtractor_readSampleData`, `AMediaExtractor_seekTo`, `AMediaExtractor_selectTrack`, `AMediaExtractor_setDataSource`, `AMediaExtractor_setDataSourceCustom (weak)`, `AMediaExtractor_setDataSourceFd`, `AMediaFormat_delete`, `AMediaFormat_getFloat`, `AMediaFormat_getInt32`, `AMediaFormat_getInt64`, `AMediaFormat_getString`, `AMediaFormat_new`, `AMediaFormat_setBuffer`, `AMediaFormat_setInt32`, `AMediaFormat_setInt64`, `AMediaFormat_setString`, `AMediaMuxer_addTrack`, `AMediaMuxer_delete`, `AMediaMuxer_new`, `AMediaMuxer_start`, `AMediaMuxer_stop`, `AMediaMuxer_writeSampleData`

## com.CactusVRStudios.CactusCowboyPlantsAtWar

No surveyed libmediandk imports.

## com.ILMxLAB.VaderImmortal.ep1

No surveyed libmediandk imports.

## com.Oculus.FirstContact

No surveyed libmediandk imports.

## com.commutergames.vspeedwayalpha

No surveyed libmediandk imports.

## com.magnopus.ElixirStore

`AMEDIAFORMAT_KEY_BIT_RATE`, `AMEDIAFORMAT_KEY_COLOR_FORMAT`, `AMEDIAFORMAT_KEY_FRAME_RATE`, `AMEDIAFORMAT_KEY_HEIGHT`, `AMEDIAFORMAT_KEY_I_FRAME_INTERVAL`, `AMEDIAFORMAT_KEY_MIME`, `AMEDIAFORMAT_KEY_WIDTH`, `AMediaCodec_configure`, `AMediaCodec_createEncoderByType`, `AMediaCodec_delete`, `AMediaCodec_dequeueInputBuffer`, `AMediaCodec_dequeueOutputBuffer`, `AMediaCodec_getInputBuffer`, `AMediaCodec_getOutputBuffer`, `AMediaCodec_queueInputBuffer`, `AMediaCodec_releaseOutputBuffer`, `AMediaCodec_start`, `AMediaCodec_stop`, `AMediaFormat_delete`, `AMediaFormat_new`, `AMediaFormat_setInt32`, `AMediaFormat_setString`

## com.resolutiongames.baitsantacruz

`AImageReader_acquireLatestImage`, `AImageReader_delete`, `AImageReader_getWindow`, `AImageReader_newWithUsage (weak)`, `AImageReader_setBufferRemovedListener (weak)`, `AImageReader_setImageListener`, `AImage_delete`, `AImage_deleteAsync (weak)`, `AImage_getHardwareBuffer (weak)`, `AImage_getTimestamp`, `AImage_getWidth`, `AMEDIAFORMAT_KEY_CHANNEL_COUNT`, `AMEDIAFORMAT_KEY_COLOR_FORMAT`, `AMEDIAFORMAT_KEY_COLOR_RANGE (weak)`, `AMEDIAFORMAT_KEY_COLOR_STANDARD (weak)`, `AMEDIAFORMAT_KEY_DURATION`, `AMEDIAFORMAT_KEY_ENCODER_DELAY (weak)`, `AMEDIAFORMAT_KEY_FRAME_RATE`, `AMEDIAFORMAT_KEY_HEIGHT`, `AMEDIAFORMAT_KEY_LANGUAGE`, `AMEDIAFORMAT_KEY_MIME`, `AMEDIAFORMAT_KEY_ROTATION (weak)`, `AMEDIAFORMAT_KEY_SAMPLE_RATE`, `AMEDIAFORMAT_KEY_SLICE_HEIGHT (weak)`, `AMEDIAFORMAT_KEY_STRIDE`, `AMEDIAFORMAT_KEY_WIDTH`, `AMediaCodec_configure`, `AMediaCodec_createDecoderByType`, `AMediaCodec_delete`, `AMediaCodec_dequeueInputBuffer`, `AMediaCodec_dequeueOutputBuffer`, `AMediaCodec_flush`, `AMediaCodec_getInputBuffer`, `AMediaCodec_getOutputBuffer`, `AMediaCodec_getOutputFormat`, `AMediaCodec_queueInputBuffer`, `AMediaCodec_releaseOutputBuffer`, `AMediaCodec_setOutputSurface (weak)`, `AMediaCodec_start`, `AMediaCodec_stop`, `AMediaDataSource_delete (weak)`, `AMediaDataSource_new (weak)`, `AMediaDataSource_setClose (weak)`, `AMediaDataSource_setGetSize (weak)`, `AMediaDataSource_setReadAt (weak)`, `AMediaDataSource_setUserdata (weak)`, `AMediaExtractor_advance`, `AMediaExtractor_delete`, `AMediaExtractor_getSampleTime`, `AMediaExtractor_getSampleTrackIndex`, `AMediaExtractor_getTrackCount`, `AMediaExtractor_getTrackFormat`, `AMediaExtractor_new`, `AMediaExtractor_readSampleData`, `AMediaExtractor_seekTo`, `AMediaExtractor_selectTrack`, `AMediaExtractor_setDataSource`, `AMediaExtractor_setDataSourceCustom (weak)`, `AMediaExtractor_setDataSourceFd`, `AMediaFormat_delete`, `AMediaFormat_getFloat`, `AMediaFormat_getInt32`, `AMediaFormat_getInt64`, `AMediaFormat_getString`, `AMediaFormat_setInt32`

## com.resolutiongames.ignis

`AImageReader_acquireLatestImage`, `AImageReader_delete`, `AImageReader_getWindow`, `AImageReader_newWithUsage (weak)`, `AImageReader_setBufferRemovedListener (weak)`, `AImageReader_setImageListener`, `AImage_delete`, `AImage_deleteAsync (weak)`, `AImage_getHardwareBuffer (weak)`, `AImage_getTimestamp`, `AImage_getWidth`, `AMEDIAFORMAT_KEY_CHANNEL_COUNT`, `AMEDIAFORMAT_KEY_COLOR_FORMAT`, `AMEDIAFORMAT_KEY_COLOR_RANGE (weak)`, `AMEDIAFORMAT_KEY_COLOR_STANDARD (weak)`, `AMEDIAFORMAT_KEY_DURATION`, `AMEDIAFORMAT_KEY_ENCODER_DELAY (weak)`, `AMEDIAFORMAT_KEY_FRAME_RATE`, `AMEDIAFORMAT_KEY_HEIGHT`, `AMEDIAFORMAT_KEY_LANGUAGE`, `AMEDIAFORMAT_KEY_MIME`, `AMEDIAFORMAT_KEY_ROTATION (weak)`, `AMEDIAFORMAT_KEY_SAMPLE_RATE`, `AMEDIAFORMAT_KEY_SLICE_HEIGHT (weak)`, `AMEDIAFORMAT_KEY_STRIDE`, `AMEDIAFORMAT_KEY_WIDTH`, `AMediaCodec_configure`, `AMediaCodec_createDecoderByType`, `AMediaCodec_delete`, `AMediaCodec_dequeueInputBuffer`, `AMediaCodec_dequeueOutputBuffer`, `AMediaCodec_flush`, `AMediaCodec_getInputBuffer`, `AMediaCodec_getOutputBuffer`, `AMediaCodec_getOutputFormat`, `AMediaCodec_queueInputBuffer`, `AMediaCodec_releaseOutputBuffer`, `AMediaCodec_setOutputSurface (weak)`, `AMediaCodec_start`, `AMediaCodec_stop`, `AMediaDataSource_delete (weak)`, `AMediaDataSource_new (weak)`, `AMediaDataSource_setClose (weak)`, `AMediaDataSource_setGetSize (weak)`, `AMediaDataSource_setReadAt (weak)`, `AMediaDataSource_setUserdata (weak)`, `AMediaExtractor_advance`, `AMediaExtractor_delete`, `AMediaExtractor_getSampleTime`, `AMediaExtractor_getSampleTrackIndex`, `AMediaExtractor_getTrackCount`, `AMediaExtractor_getTrackFormat`, `AMediaExtractor_new`, `AMediaExtractor_readSampleData`, `AMediaExtractor_seekTo`, `AMediaExtractor_selectTrack`, `AMediaExtractor_setDataSource`, `AMediaExtractor_setDataSourceCustom (weak)`, `AMediaExtractor_setDataSourceFd`, `AMediaFormat_delete`, `AMediaFormat_getFloat`, `AMediaFormat_getInt32`, `AMediaFormat_getInt64`, `AMediaFormat_getString`, `AMediaFormat_setInt32`

## games.b4t.epicrollercoasters.oculus

`AImageReader_acquireLatestImage`, `AImageReader_acquireLatestImageAsync (weak)`, `AImageReader_delete`, `AImageReader_getWindow`, `AImageReader_newWithUsage (weak)`, `AImageReader_setBufferRemovedListener (weak)`, `AImageReader_setImageListener`, `AImage_delete`, `AImage_deleteAsync (weak)`, `AImage_getFormat`, `AImage_getHardwareBuffer (weak)`, `AImage_getHeight`, `AImage_getNumberOfPlanes`, `AImage_getPlaneData`, `AImage_getPlanePixelStride`, `AImage_getPlaneRowStride`, `AImage_getTimestamp`, `AImage_getWidth`, `AMEDIAFORMAT_KEY_CHANNEL_COUNT`, `AMEDIAFORMAT_KEY_COLOR_FORMAT`, `AMEDIAFORMAT_KEY_COLOR_RANGE (weak)`, `AMEDIAFORMAT_KEY_COLOR_STANDARD (weak)`, `AMEDIAFORMAT_KEY_DURATION`, `AMEDIAFORMAT_KEY_ENCODER_DELAY (weak)`, `AMEDIAFORMAT_KEY_FRAME_RATE`, `AMEDIAFORMAT_KEY_HEIGHT`, `AMEDIAFORMAT_KEY_LANGUAGE`, `AMEDIAFORMAT_KEY_MIME`, `AMEDIAFORMAT_KEY_ROTATION (weak)`, `AMEDIAFORMAT_KEY_SAMPLE_RATE`, `AMEDIAFORMAT_KEY_SLICE_HEIGHT (weak)`, `AMEDIAFORMAT_KEY_STRIDE`, `AMEDIAFORMAT_KEY_WIDTH`, `AMediaCodec_configure`, `AMediaCodec_createDecoderByType`, `AMediaCodec_delete`, `AMediaCodec_dequeueInputBuffer`, `AMediaCodec_dequeueOutputBuffer`, `AMediaCodec_flush`, `AMediaCodec_getInputBuffer`, `AMediaCodec_getOutputBuffer`, `AMediaCodec_getOutputFormat`, `AMediaCodec_queueInputBuffer`, `AMediaCodec_releaseOutputBuffer`, `AMediaCodec_setOutputSurface (weak)`, `AMediaCodec_start`, `AMediaCodec_stop`, `AMediaDataSource_delete (weak)`, `AMediaDataSource_new (weak)`, `AMediaDataSource_setClose (weak)`, `AMediaDataSource_setGetSize (weak)`, `AMediaDataSource_setReadAt (weak)`, `AMediaDataSource_setUserdata (weak)`, `AMediaExtractor_advance`, `AMediaExtractor_delete`, `AMediaExtractor_getSampleTime`, `AMediaExtractor_getSampleTrackIndex`, `AMediaExtractor_getTrackCount`, `AMediaExtractor_getTrackFormat`, `AMediaExtractor_new`, `AMediaExtractor_readSampleData`, `AMediaExtractor_seekTo`, `AMediaExtractor_selectTrack`, `AMediaExtractor_setDataSource`, `AMediaExtractor_setDataSourceCustom (weak)`, `AMediaExtractor_setDataSourceFd`, `AMediaFormat_delete`, `AMediaFormat_getFloat`, `AMediaFormat_getInt32`, `AMediaFormat_getInt64`, `AMediaFormat_getString`, `AMediaFormat_setInt32`
