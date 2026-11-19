// VrApi frame submission, shared by every host (hle/vrapi/host.h): the frame
// protocol around one SubmitFrame2 (waiting and beginning the frame, layer
// validation and translation, ending it), parameterized by the host's
// swapchain images. `Images` is constructed for one submission while the
// VrApi and session locks are held and provides:
//
//   Images(const Submit &, const FenceMap *fences);
//   XrResult ready() const;    // false: the frame fails ("SubmitFrame queue")
//   // Before a layer is translated; returns the eye whose image goes first.
//   int begin_layer(const LayerHeader &, const XrView *eyes);
//   XrResult image(TextureSwapChain *, int slot, Rect, int eye, bool bottom_up, XrSwapchainSubImage &);
//   DrawnLayers *drawn();      // nullptr: layers no runtime layer shows exactly are left out
//   XrResult gather(std::vector<const XrCompositionLayerBaseHeader *> &);  // drawn layers' sources
//   XrResult finish(std::vector<const XrCompositionLayerBaseHeader *> &);  // images handed over
#pragma once
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/log.h"
#include "core/wait.h"
#include "hle/vrapi/layers.h"
#include "hle/vrapi/state.h"

namespace venture::hle::vrapi {
// Completion fences of the legacy SubmitFrame, per ring slot.
using FenceMap = std::map<std::pair<TextureSwapChain *, int>, std::vector<std::uint64_t>>;

// A rejected VrApi layer must not strand an xrBeginFrame on its producer
// thread. End without layers, retaining the submission's original error.
struct FrameCleanup {
  ~FrameCleanup() {
    auto &s = session();
    if (s.phase != Session::Phase::begun)
      return;
    XrFrameEndInfo discard{XR_TYPE_FRAME_END_INFO};
    discard.displayTime = s.frame.predictedDisplayTime;
    discard.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    auto r = s.end(&discard);
    if (XR_SUCCEEDED(r)) {
      complete_frame();
    } else
      failure("SubmitFrame2 discard", r);
  }
};

template <class Images>
std::int32_t submit_frame(Mobile *mobile, const Submit *frame, const FenceMap *fences) {
  if (!mobile || !frame) return invalid;
  auto r = ensure_frame(frame->frame_index, mobile);
  if (XR_FAILED(r)) return result(r, "SubmitFrame2 wait");
  std::unique_lock lock(state().mutex);
  std::unique_lock sl(session().mutex);
  if (!valid(mobile))
    return invalid;
  if (state().final_frame)
    return invalid_operation;
  if (!state().token || state().frame_index != frame->frame_index)
    return invalid_operation;
  if (session().phase == Session::Phase::waited) {
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = session().begin(&bi);
    if (XR_FAILED(r))
      return result(r, "SubmitFrame2 begin");
    wake_frame_waiters();
  }
  FrameCleanup cleanup;
  if ((frame->layer_count && !frame->layers) || frame->layer_count > 16 ||
      frame->swap_interval < 1 || frame->swap_interval > 4 ||
      !std::isfinite(frame->display_time) || frame->display_time < 0)
    return invalid;
  // FLUSH is satisfied by synchronous image copying/submission. FINAL closes
  // submission until Leave/Enter; no process-wide shutdown is implied.
  if (frame->flags & ~(2u | 4u | 64u))
    return unsupported;
  // The host runtime sets the frame rate, as it does for any PC OpenXR
  // application: a larger VrApi swap interval (a Quest half-rate budget) still
  // renders every host frame instead of repeating content.
  if (session().logged_swap_interval != frame->swap_interval) {
    session().logged_swap_interval = frame->swap_interval;
    log::info("vrapi", "Swap interval={} requested; guest cadence follows the host display={} Hz",
        frame->swap_interval, session().display_timing.rate());
  }
  Images images(*frame, fences);
  if (auto prepared = images.ready(); XR_FAILED(prepared)) return result(prepared, "SubmitFrame queue");
  std::vector<LayerStorage> storage(frame->layer_count);
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr,
                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                          session().frame.predictedDisplayTime, state().view};
  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrView eye_offsets[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  std::uint32_t count = 0;
  r = ::xrLocateViews(session().handle, &locate, &vs, 2, &count, eye_offsets);
  if (XR_FAILED(r) || count != 2)
    return result(XR_FAILED(r) ? r : XR_ERROR_RUNTIME_FAILURE, "SubmitFrame2 eyes");
  for (std::uint32_t i = 0; i < frame->layer_count && session().frame.shouldRender; ++i) {
    const auto *header = frame->layers[i];
    if (!header)
      return invalid;
    auto image = [&](TextureSwapChain *token, int slot, Rect rect, int eye, bool bottom_up,
                     XrSwapchainSubImage &out) {
      return images.image(token, slot, rect, eye, bottom_up, out);
    };
    const auto translated = layers.size();
    const int first_eye = images.begin_layer(*header, eye_offsets);
    r = translate_layer(*header, eye_offsets, image, storage[i], layers, first_eye, images.drawn());
    if (XR_FAILED(r)) {
      {
        // Once per process: debug reports carry the rejected layer's shape.
        static std::once_flag reported;
        std::call_once(reported, [&] {
          log::warn("vrapi", "Rejected layer type={} flags={:#x} blend={}/{} completion_fence_present={} result={} reason={}",
                    header->type, header->flags, header->src_blend, header->dst_blend,
                    frame->completion_fence != 0, int(r), storage[i].reason ? storage[i].reason : "none");
          log_layer_shape(*header);
        });
      }
      // A layer the host cannot show is left out; the frame's other layers
      // still display instead of the whole frame being discarded.
      if (r == XR_ERROR_FEATURE_UNSUPPORTED) {
        layers.resize(translated);
        failure("SubmitFrame2 layer (skipped)", r);
        continue;
      }
      return result(r, "SubmitFrame2 layer");
    }
  }
  r = images.gather(layers);
  if (XR_FAILED(r)) return result(r, "SubmitFrame2 drawn layer");
  r = images.finish(layers);
  if (XR_FAILED(r)) return result(r, "SubmitFrame2 handoff batch");
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  // Legacy VrApi predicts before rendering; the native wait occurs here in
  // submission and may already target a later display after a load or stall.
  // Submit this native frame at its waited target, not an older Quest target
  // (or a queued future prediction). Preserve the guest's poses and storage.
  end.displayTime = session().frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = std::uint32_t(layers.size());
  end.layers = layers.data();
  r = session().end(&end);
  if (XR_SUCCEEDED(r)) {
    state().final_frame = (frame->flags & 4) != 0;
    auto now = monotonic_ns();
    state().submissions.push_back(now);
    while (state().submissions.size() > 1 && state().submissions.front() < now - 1'000'000'000)
      state().submissions.pop_front();
  }
  if (session().phase != Session::Phase::begun) complete_frame();
  return result(r, "SubmitFrame2 end");
}

// The legacy vrapi_SubmitFrame (one projection per layer, per-texture fences)
// as a SubmitFrame2, then submitted with the host's images.
template <class Images>
void submit_legacy_frame(Mobile *mobile, const LegacyFrame *legacy) {
  if (!legacy || legacy->type != 3 || legacy->layer_count < 0 || legacy->layer_count > 4) {
    failure("SubmitFrame", invalid);
    return;
  }
  Projection storage[4]{};
  const LayerHeader *layers[4]{};
  FenceMap fences;
  for (int i = 0; i < legacy->layer_count; ++i) {
    auto &p = storage[i];
    auto &l = legacy->layers[i];
    p.header = {1,
                std::uint32_t(l.flags),
                {l.color_scale, l.color_scale, l.color_scale, l.color_scale},
                l.src_blend,
                l.dst_blend,
                0};
    p.head = l.textures[0].head;
    if (std::memcmp(&l.textures[0].head.pose, &l.textures[1].head.pose, sizeof(Pose)) != 0) {
      failure("SubmitFrame per-eye render pose", unsupported);
      return;
    }
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = l.textures[eye];
      if (t.depth) {
        failure("SubmitFrame depth", unsupported);
        return;
      }
      p.textures[eye] = {t.color, t.index, t.tan_angles, t.rect};
      if (t.completion_fence)
        fences[{t.color, t.index}].push_back(t.completion_fence);
    }
    layers[i] = &p.header;
  }
  Submit frame{std::uint32_t(legacy->flags),
               std::uint32_t(legacy->swap_interval),
               std::uint64_t(legacy->frame_index),
               0,
               0,
               std::uint32_t(legacy->layer_count),
               layers};
  auto r = submit_frame<Images>(mobile, &frame, &fences);
  if (r < 0)
    failure("SubmitFrame", r);
}

// Validation the swapchain entry points share (swapchain_common.cpp).
bool valid_sampler(const TextureSamplerState &);
// CreateTextureSwapChain4's arguments VrApi accepts.
bool valid_swapchain_create(const SwapChainCreate &);
}  // namespace venture::hle::vrapi
