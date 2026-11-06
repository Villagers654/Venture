#!/usr/bin/env python3
"""A32 descriptors for the Media NDK (libmediandk.so) exports Venture implements.

The NDK r30 AST generator (a32_descriptors.py) does not cover the media headers.
The prototypes below are the public declarations of NdkMediaFormat.h,
NdkMediaExtractor.h, NdkMediaCodec.h, NdkMediaDataSource.h, NdkMediaCrypto.h,
NdkMediaMuxer.h, NdkImageReader.h and NdkImage.h (AOSP frameworks/av/media/ndk/
include/media, Android 12), spelled as clang prints parameter types. They are
classified by the same classify() as the AST generator; nothing is inferred from
host C++ signatures. Array parameters (const AMediaUUID) are written decayed.
"""
import argparse
import re
from pathlib import Path
from a32_descriptors import ROOT, classify, emit

PROTOTYPES = '''
AMediaFormat * AMediaFormat_new(void) | NdkMediaFormat.h
media_status_t AMediaFormat_delete(AMediaFormat *) | NdkMediaFormat.h
const char * AMediaFormat_toString(AMediaFormat *) | NdkMediaFormat.h
bool AMediaFormat_getInt32(AMediaFormat *, const char *, int32_t *) | NdkMediaFormat.h
bool AMediaFormat_getInt64(AMediaFormat *, const char *, int64_t *) | NdkMediaFormat.h
bool AMediaFormat_getFloat(AMediaFormat *, const char *, float *) | NdkMediaFormat.h
bool AMediaFormat_getDouble(AMediaFormat *, const char *, double *) | NdkMediaFormat.h
bool AMediaFormat_getBuffer(AMediaFormat *, const char *, void **, size_t *) | NdkMediaFormat.h
bool AMediaFormat_getString(AMediaFormat *, const char *, const char **) | NdkMediaFormat.h
void AMediaFormat_setInt32(AMediaFormat *, const char *, int32_t) | NdkMediaFormat.h
void AMediaFormat_setInt64(AMediaFormat *, const char *, int64_t) | NdkMediaFormat.h
void AMediaFormat_setFloat(AMediaFormat *, const char *, float) | NdkMediaFormat.h
void AMediaFormat_setDouble(AMediaFormat *, const char *, double) | NdkMediaFormat.h
void AMediaFormat_setString(AMediaFormat *, const char *, const char *) | NdkMediaFormat.h
void AMediaFormat_setBuffer(AMediaFormat *, const char *, const void *, size_t) | NdkMediaFormat.h
AMediaExtractor * AMediaExtractor_new(void) | NdkMediaExtractor.h
media_status_t AMediaExtractor_delete(AMediaExtractor *) | NdkMediaExtractor.h
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *, int, off64_t, off64_t) | NdkMediaExtractor.h
media_status_t AMediaExtractor_setDataSource(AMediaExtractor *, const char *) | NdkMediaExtractor.h
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *, AMediaDataSource *) | NdkMediaExtractor.h
size_t AMediaExtractor_getTrackCount(AMediaExtractor *) | NdkMediaExtractor.h
AMediaFormat * AMediaExtractor_getTrackFormat(AMediaExtractor *, size_t) | NdkMediaExtractor.h
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *, size_t) | NdkMediaExtractor.h
media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *, size_t) | NdkMediaExtractor.h
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *, uint8_t *, size_t) | NdkMediaExtractor.h
uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *) | NdkMediaExtractor.h
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *) | NdkMediaExtractor.h
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *) | NdkMediaExtractor.h
bool AMediaExtractor_advance(AMediaExtractor *) | NdkMediaExtractor.h
media_status_t AMediaExtractor_seekTo(AMediaExtractor *, int64_t, SeekMode) | NdkMediaExtractor.h
AMediaCodec * AMediaCodec_createCodecByName(const char *) | NdkMediaCodec.h
AMediaCodec * AMediaCodec_createDecoderByType(const char *) | NdkMediaCodec.h
AMediaCodec * AMediaCodec_createEncoderByType(const char *) | NdkMediaCodec.h
media_status_t AMediaCodec_delete(AMediaCodec *) | NdkMediaCodec.h
media_status_t AMediaCodec_configure(AMediaCodec *, const AMediaFormat *, ANativeWindow *, AMediaCrypto *, uint32_t) | NdkMediaCodec.h
media_status_t AMediaCodec_start(AMediaCodec *) | NdkMediaCodec.h
media_status_t AMediaCodec_stop(AMediaCodec *) | NdkMediaCodec.h
media_status_t AMediaCodec_flush(AMediaCodec *) | NdkMediaCodec.h
uint8_t * AMediaCodec_getInputBuffer(AMediaCodec *, size_t, size_t *) | NdkMediaCodec.h
uint8_t * AMediaCodec_getOutputBuffer(AMediaCodec *, size_t, size_t *) | NdkMediaCodec.h
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *, int64_t) | NdkMediaCodec.h
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *, size_t, off_t, size_t, uint64_t, uint32_t) | NdkMediaCodec.h
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *, AMediaCodecBufferInfo *, int64_t) | NdkMediaCodec.h
AMediaFormat * AMediaCodec_getOutputFormat(AMediaCodec *) | NdkMediaCodec.h
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *, size_t, bool) | NdkMediaCodec.h
media_status_t AMediaCodec_setOutputSurface(AMediaCodec *, ANativeWindow *) | NdkMediaCodec.h
media_status_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *, size_t, int64_t) | NdkMediaCodec.h
media_status_t AMediaCodec_createInputSurface(AMediaCodec *, ANativeWindow **) | NdkMediaCodec.h
media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec *) | NdkMediaCodec.h
AMediaDataSource * AMediaDataSource_new(void) | NdkMediaDataSource.h
void AMediaDataSource_delete(AMediaDataSource *) | NdkMediaDataSource.h
void AMediaDataSource_setUserdata(AMediaDataSource *, void *) | NdkMediaDataSource.h
void AMediaDataSource_setReadAt(AMediaDataSource *, AMediaDataSourceReadAt) | NdkMediaDataSource.h
void AMediaDataSource_setGetSize(AMediaDataSource *, AMediaDataSourceGetSize) | NdkMediaDataSource.h
void AMediaDataSource_setClose(AMediaDataSource *, AMediaDataSourceClose) | NdkMediaDataSource.h
bool AMediaCrypto_isCryptoSchemeSupported(const uint8_t *) | NdkMediaCrypto.h
bool AMediaCrypto_requiresSecureDecoderComponent(const char *) | NdkMediaCrypto.h
AMediaCrypto * AMediaCrypto_new(const uint8_t *, const void *, size_t) | NdkMediaCrypto.h
void AMediaCrypto_delete(AMediaCrypto *) | NdkMediaCrypto.h
AMediaMuxer * AMediaMuxer_new(int, OutputFormat) | NdkMediaMuxer.h
media_status_t AMediaMuxer_delete(AMediaMuxer *) | NdkMediaMuxer.h
ssize_t AMediaMuxer_addTrack(AMediaMuxer *, const AMediaFormat *) | NdkMediaMuxer.h
media_status_t AMediaMuxer_start(AMediaMuxer *) | NdkMediaMuxer.h
media_status_t AMediaMuxer_stop(AMediaMuxer *) | NdkMediaMuxer.h
media_status_t AMediaMuxer_writeSampleData(AMediaMuxer *, size_t, const uint8_t *, const AMediaCodecBufferInfo *) | NdkMediaMuxer.h
media_status_t AImageReader_new(int32_t, int32_t, int32_t, int32_t, AImageReader **) | NdkImageReader.h
media_status_t AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **) | NdkImageReader.h
void AImageReader_delete(AImageReader *) | NdkImageReader.h
media_status_t AImageReader_getWindow(AImageReader *, ANativeWindow **) | NdkImageReader.h
media_status_t AImageReader_getWidth(const AImageReader *, int32_t *) | NdkImageReader.h
media_status_t AImageReader_getHeight(const AImageReader *, int32_t *) | NdkImageReader.h
media_status_t AImageReader_getFormat(const AImageReader *, int32_t *) | NdkImageReader.h
media_status_t AImageReader_getMaxImages(const AImageReader *, int32_t *) | NdkImageReader.h
media_status_t AImageReader_acquireNextImage(AImageReader *, AImage **) | NdkImageReader.h
media_status_t AImageReader_acquireLatestImage(AImageReader *, AImage **) | NdkImageReader.h
media_status_t AImageReader_acquireNextImageAsync(AImageReader *, AImage **, int *) | NdkImageReader.h
media_status_t AImageReader_acquireLatestImageAsync(AImageReader *, AImage **, int *) | NdkImageReader.h
media_status_t AImageReader_setImageListener(AImageReader *, AImageReader_ImageListener *) | NdkImageReader.h
media_status_t AImageReader_setBufferRemovedListener(AImageReader *, AImageReader_BufferRemovedListener *) | NdkImageReader.h
void AImage_delete(AImage *) | NdkImage.h
void AImage_deleteAsync(AImage *, int) | NdkImage.h
media_status_t AImage_getWidth(const AImage *, int32_t *) | NdkImage.h
media_status_t AImage_getHeight(const AImage *, int32_t *) | NdkImage.h
media_status_t AImage_getFormat(const AImage *, int32_t *) | NdkImage.h
media_status_t AImage_getCropRect(const AImage *, AImageCropRect *) | NdkImage.h
media_status_t AImage_getTimestamp(const AImage *, int64_t *) | NdkImage.h
media_status_t AImage_getNumberOfPlanes(const AImage *, int32_t *) | NdkImage.h
media_status_t AImage_getPlanePixelStride(const AImage *, int, int32_t *) | NdkImage.h
media_status_t AImage_getPlaneRowStride(const AImage *, int, int32_t *) | NdkImage.h
media_status_t AImage_getPlaneData(const AImage *, int, uint8_t **, int *) | NdkImage.h
media_status_t AImage_getHardwareBuffer(const AImage *, AHardwareBuffer **) | NdkImage.h
'''

# Typedefs of the same headers. '@opaque' marks incomplete handle types; records
# with a complete definition classify as Layout; enums are 32-bit on AAPCS.
ALIASES = {
    'int8_t': 'signed char', 'uint8_t': 'unsigned char', 'int16_t': 'short', 'uint16_t': 'unsigned short',
    'int32_t': 'int', 'uint32_t': 'unsigned int',
    'media_status_t': 'int', 'SeekMode': 'int', 'OutputFormat': 'int',
    'AMediaCodecBufferInfo': 'struct AMediaCodecBufferInfo', 'AImageCropRect': 'struct AImageCropRect',
    'AImageReader_ImageListener': 'struct AImageReader_ImageListener',
    'AImageReader_BufferRemovedListener': 'struct AImageReader_BufferRemovedListener',
    'AMediaDataSourceReadAt': 'ssize_t (*)(void *, off64_t, void *, size_t)',
    'AMediaDataSourceGetSize': 'ssize_t (*)(void *)',
    'AMediaDataSourceClose': 'void (*)(void *)',
}
for handle in ('AMediaFormat', 'AMediaExtractor', 'AMediaCodec', 'AMediaDataSource', 'AMediaCrypto', 'AMediaMuxer',
               'AImageReader', 'AImage', 'ANativeWindow', 'AHardwareBuffer'):
    ALIASES[handle] = '@opaque'


def rows():
    result = []
    for line in PROTOTYPES.strip().splitlines():
        declaration, header = (part.strip() for part in line.split('|'))
        m = re.match(r'(.+?)\s*\b(\w+)\((.*)\)$', declaration)
        assert m, declaration
        ret, name, params = m.groups()
        params = [] if params == 'void' else [p.strip() for p in params.split(',')]
        result.append(dict(library='libmediandk.so', symbol=name, result=classify(ret, ALIASES),
                           arguments=[classify(p, ALIASES) for p in params], variadic=False,
                           source='AOSP media/ndk ' + header))
    for r in result:
        for t in [r['result'], *r['arguments']]:
            assert t['kind'] != 'Unsupported', (r['symbol'], t)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT / 'src/runtime/core/generated')
    a = p.parse_args()
    emit(rows(), a.output / 'a32_media.cpp', 'media_descriptors')


if __name__ == '__main__':
    main()
