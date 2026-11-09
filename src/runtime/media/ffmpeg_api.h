// Internal to the media library: the dynamically resolved FFmpeg entry points.
#pragma once
#include "media/media.h"
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/log.h>
#include <libavutil/sha.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace venture::media {
struct Library::Impl {
  std::vector<void*> modules;  // HMODULE / dlopen handles
  bool ready = false;
#define MEDIA_FUNCTIONS(X) \
  X(avutil_version) X(avcodec_version) X(avformat_version) \
  X(av_malloc) X(av_free) X(av_max_alloc) X(av_rescale_q) X(av_log_set_level) \
  X(avio_alloc_context) X(avio_context_free) X(avformat_alloc_context) \
  X(av_find_input_format) X(avformat_open_input) X(avformat_find_stream_info) \
  X(avformat_close_input) X(av_read_frame) X(av_seek_frame) \
  X(av_packet_alloc) X(av_packet_free) X(av_packet_unref) X(av_new_packet) \
  X(av_packet_get_side_data) X(av_packet_new_side_data) \
  X(avcodec_find_decoder) X(avcodec_alloc_context3) X(avcodec_parameters_to_context) \
  X(avcodec_get_name) X(avcodec_get_hw_config) X(av_hwdevice_ctx_create) X(av_hwframe_transfer_data) \
  X(av_buffer_ref) X(av_buffer_unref) \
  X(avcodec_open2) X(avcodec_free_context) X(avcodec_send_packet) \
  X(avcodec_receive_frame) X(avcodec_flush_buffers) \
  X(av_frame_alloc) X(av_frame_free) X(av_frame_unref) X(av_frame_ref) X(av_frame_move_ref) \
  X(av_frame_apply_cropping) X(av_frame_copy_props) \
  X(sws_alloc_context) X(sws_scale_frame) X(sws_freeContext) \
  X(av_sample_fmt_is_planar) X(av_get_bytes_per_sample)
  // The built-in SHA implementation identifies the bounded input region.
#define MEDIA_HASH_FUNCTIONS(X) X(av_sha_alloc) X(av_sha_init) X(av_sha_update) X(av_sha_final)
#define FIELD(n) decltype(&::n) n = nullptr;
  MEDIA_FUNCTIONS(FIELD)
  MEDIA_HASH_FUNCTIONS(FIELD)
#undef FIELD
  ~Impl();
};
}  // namespace venture::media
