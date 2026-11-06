#include "support.h"
#include "lifecycle_host.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/passthrough.h"
#include "core/process.h"
#include <doctest/doctest.h>
using namespace venture;
using namespace venture::test::passthrough;
namespace xg = venture::hle::openxr::guest;
namespace {
struct Run : Fixture {
  XrInstance instance=reinterpret_cast<XrInstance>(1);
  explicit Run(const std::function<void()>& setup = {}) {
    if (setup) setup();
    hle::openxr::reset_passthrough_probe();
    host::reset();
    hle::openxr::state().instance_live=true;
    xr::host_xr().instance=instance; xr::host_xr().system=1;
    hle::openxr::track_handle(instance,"XrInstance",XrInstance(XR_NULL_HANDLE));
    auto& s=xr::session();s.handle=target;s.owner=xr::SessionOwner::openxr;
    s.running=false;s.phase=xr::Session::Phase::idle;s.state=XR_SESSION_STATE_READY;
    s.passthrough_suspended=true;s.lost=XR_SUCCESS;s.lead={};s.frame_index=0;
    create(); begin();
  }
  ~Run() { xr::session().finish_host_end(); if(xr::session().running) xr::session().end_session(); }
  XrResult begin() {
    XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO,nullptr,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    return xg::xrBeginSession(target,&info);
  }
  XrTime frame() {
    XrFrameState state{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(target,nullptr,&state)==XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(target,nullptr)==XR_SUCCESS);
    return state.predictedDisplayTime;
  }
  void poll(XrSessionState wanted) {
    host::event(wanted);XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    REQUIRE(xg::xrPollEvent(instance,&event)==XR_SUCCESS);
    REQUIRE(reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state==wanted);
  }
};
struct Submission {
  XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  std::array<XrCompositionLayerProjectionView,2> views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<const XrCompositionLayerBaseHeader*,2> layers{};
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
  Submission(Run& run,XrSwapchain chain) {
    room.layerHandle=run.layer;projection.space=space;projection.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    projection.viewCount=2;projection.views=views.data();
    for(unsigned i=0;i<2;++i){views[i].pose.orientation.w=1;views[i].fov={-.8f,.8f,.8f,-.8f};views[i].subImage={chain,{{0,0},{4,4}},i};}
    layers={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&room),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;info.layerCount=2;info.layers=layers.data();
  }
  XrResult submit(Run& run) { info.displayTime=run.frame();return xg::xrEndFrame(target,&info); }
  void vr() {info.layerCount=1;info.layers=layers.data()+1;}
};
}
TEST_CASE("Production system properties suppress native support when passthrough is off") {
  Fixture fixture;
  const auto instance = reinterpret_cast<XrInstance>(1);
  hle::openxr::track_handle(instance, "XrInstance", XrInstance(XR_NULL_HANDLE));
  xr::host_xr().enabled_extensions.insert("XR_FB_passthrough");
  for (const bool opt_in : {false, true}) {
    process_config().passthrough = opt_in;
    XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
    XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer, XR_FALSE};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &older};
    REQUIRE(xg::xrGetSystemProperties(instance, 1, &properties) == XR_SUCCESS);
    CHECK(older.supportsPassthrough == (opt_in ? XR_TRUE : XR_FALSE));
    CHECK(newer.capabilities == (opt_in ? XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB : 0));
    CHECK(properties.next == &older); CHECK(older.next == &newer);
    XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    XrPassthroughFB feature{};
    CHECK(xg::xrCreatePassthroughFB(target, &create, &feature) == (opt_in ? XR_SUCCESS : XR_ERROR_FEATURE_UNSUPPORTED));
    CHECK(native_creates == (opt_in ? 1 : 0));
  }
}
TEST_CASE("Production STOPPING final xrEndFrame never reacquires cameras until a successful restart") {
  Run run;auto chain=run.chain();fill(chain,{{{1,0,0,.25f},{0,1,0,.75f}}});grant=true;
  Submission submit(run,chain);REQUIRE(submit.submit(run)==XR_SUCCESS);REQUIRE(requested);
  submit.info.displayTime=run.frame();run.poll(XR_SESSION_STATE_STOPPING);
  REQUIRE_FALSE(requested);const auto attempts=requests;
  REQUIRE(xg::xrEndFrame(target,&submit.info)==XR_SUCCESS);CHECK_FALSE(requested);CHECK(requests==attempts);
  CHECK(pixel(host::last_frame().views[0].subImage.swapchain)[3]==255);
  REQUIRE(xg::xrEndSession(target)==XR_SUCCESS);CHECK_FALSE(xr::session().running);
  run.poll(XR_SESSION_STATE_READY);host::begin_result=XR_ERROR_RUNTIME_FAILURE;
  CHECK(run.begin()==XR_ERROR_RUNTIME_FAILURE);CHECK(xr::session().passthrough_suspended);
  host::begin_result=XR_SUCCESS;REQUIRE(run.begin()==XR_SUCCESS);
  REQUIRE(submit.submit(run)==XR_SUCCESS);CHECK(requested);
}
TEST_CASE("Production MR exit restores normal arrays and presentation without MR eye copies") {
  Run run;const auto chain=run.chain();fill(chain,{{{1,0,0,0},{0,1,0,.5f}}});grant=true;
  Submission submit(run,chain);REQUIRE(submit.submit(run)==XR_SUCCESS);REQUIRE(submit.submit(run)==XR_SUCCESS);
  submit.vr();withdrawal_complete=false;REQUIRE(submit.submit(run)==XR_SUCCESS);CHECK_FALSE(requested);
  auto frame=host::last_frame();CHECK(frame.views[0].subImage.swapchain==frame.views[1].subImage.swapchain);
  CHECK(frame.views[1].subImage.imageArrayIndex==1);CHECK(pixel(frame.views[0].subImage.swapchain)[3]==255);
  withdrawal_complete=true;REQUIRE(submit.submit(run)==XR_SUCCESS);
  const auto copies=host_acquires;REQUIRE(submit.submit(run)==XR_SUCCESS);CHECK(host_acquires==copies);
  CHECK(pixel(host::last_frame().views[0].subImage.swapchain)[3]==0);
}
TEST_CASE("Production VR after MR accepts non-RGBA8 and multisampled projections") {
  for (const auto format : {VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM}) {
    Run run;auto chain=run.chain();fill(chain,{{{1,0,0,.5f},{0,1,0,.5f}}});grant=true;
    Submission submit(run,chain);REQUIRE(submit.submit(run)==XR_SUCCESS);
    const auto ordinary=run.chain(2,format,format==VK_FORMAT_R8G8B8A8_UNORM?4:1);
    fill(ordinary,{{{1,0,0,.5f},{0,1,0,.5f}}});
    for(auto& view:submit.views)view.subImage.swapchain=ordinary;
    submit.vr();withdrawal_complete=false;REQUIRE(submit.submit(run)==XR_SUCCESS);
    withdrawal_complete=true;REQUIRE(submit.submit(run)==XR_SUCCESS);
    CHECK(host::last_frame().views[0].subImage.swapchain==hle::openxr::state().swapchains.at(ordinary).host_handle);
  }
}

TEST_CASE("Production ordinary VR after MR retains its depth chain and async host ownership") {
  for (const bool paused : {false, true}) {
  Run run;const auto color=run.chain();fill(color,{{{1,0,0,.5f},{0,1,0,.5f}}});grant=true;
  Submission submit(run,color);REQUIRE(submit.submit(run)==XR_SUCCESS);
  const auto depth=run.chain(2,VK_FORMAT_D32_SFLOAT);
  std::array<XrCompositionLayerDepthInfoKHR,2> depths{};
  hle::openxr::state().enabled.insert("XR_KHR_composition_layer_depth");
  xr::host_xr().enabled_extensions.insert("XR_KHR_composition_layer_depth");
  for(unsigned i=0;i<2;++i) {
    depths[i]={XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,nullptr,{depth,{{0,0},{4,4}},i},0,1,.1f,100.f};
    submit.views[i].next=&depths[i];
  }
  if (paused) REQUIRE(xg::xrPassthroughLayerPauseFB(run.layer)==XR_SUCCESS);
  else submit.vr();
  run.poll(XR_SESSION_STATE_FOCUSED);submit.info.displayTime=run.frame();
  auto& session=xr::session();session.lead.enabled=true;session.lead.async=true;session.lead.lead_ns=1'000'000;
  REQUIRE(xg::xrEndFrame(target,&submit.info)==XR_SUCCESS);
  CHECK(session.lead.async_ends==1);session.finish_host_end();
  CHECK(host::last_frame().depth);
  CHECK(host::last_frame().views[0].subImage.swapchain==hle::openxr::state().swapchains.at(color).host_handle);
  }
}

TEST_CASE("Production paused passthrough layers retain ordinary formats without MR eye copies") {
  for (const bool pause_feature : {false, true}) {
    for (const auto format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM}) {
      Run run; const auto chain = run.chain();
      fill(chain, {{{1,0,0,.5f},{0,1,0,.5f}}}); grant = true;
      Submission submit(run, chain); REQUIRE(submit.submit(run) == XR_SUCCESS);
      REQUIRE((pause_feature ? xg::xrPassthroughPauseFB(run.feature) :
                               xg::xrPassthroughLayerPauseFB(run.layer)) == XR_SUCCESS);
      const auto ordinary = run.chain(2, format, format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 1);
      fill(ordinary, {{{1,0,0,.5f},{0,1,0,.5f}}});
      for (auto& view : submit.views) view.subImage.swapchain = ordinary;
      // A paused layer may remain in the guest's list; it must not reach the host.
      withdrawal_complete = false; REQUIRE(submit.submit(run) == XR_SUCCESS);
      CHECK_FALSE(requested);
      withdrawal_complete = true; REQUIRE(submit.submit(run) == XR_SUCCESS);
      CHECK(host::last_frame().views[0].subImage.swapchain == hle::openxr::state().swapchains.at(ordinary).host_handle);
      const auto copies = host_acquires;
      REQUIRE(submit.submit(run) == XR_SUCCESS); CHECK(host_acquires == copies);
      for (auto& view : submit.views) view.subImage.swapchain = chain;
      REQUIRE((pause_feature ? xg::xrPassthroughStartFB(run.feature) :
                               xg::xrPassthroughLayerResumeFB(run.layer)) == XR_SUCCESS);
      REQUIRE(submit.submit(run) == XR_SUCCESS); CHECK(requested);
    }
  }
}

TEST_CASE("Production see-through blending shows the camera under the game with the requested opacity") {
  Run run([] { blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND}; });
  const auto chain = run.chain();
  fill(chain, {{{1, 0, 0, .5f}, {0, 1, 0, 1}}});
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .5f, {0, 0, 0, 0}};
  REQUIRE(xg::xrPassthroughLayerSetStyleFB(run.layer, &style) == XR_SUCCESS);
  Submission submit(run, chain);
  submit.projection.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  auto frame = host::last_frame();
  CHECK_FALSE(requested); CHECK(native_creates == 0);
  CHECK(frame.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
  REQUIRE(frame.types.size() == 1); CHECK(frame.types[0] == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
  // Straight (1,0,0,.5) premultiplied, coverage 1-P(1-A) = .75: half the camera
  // shows where the game is half transparent.
  const auto left = pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex);
  CHECK(std::abs(int(left[0]) - 128) <= 1); CHECK(left[1] == 0); CHECK(std::abs(int(left[3]) - 191) <= 1);
  const auto right = pixel(frame.views[1].subImage.swapchain, frame.views[1].subImage.imageArrayIndex);
  CHECK(right[1] == 255); CHECK(right[3] == 255);
  // Full opacity is plain forwarding: the game's own images, its own alpha.
  style.textureOpacityFactor = 1;
  REQUIRE(xg::xrPassthroughLayerSetStyleFB(run.layer, &style) == XR_SUCCESS);
  submit.projection.layerFlags &= ~XrCompositionLayerFlags(XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  frame = host::last_frame();
  CHECK(frame.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
  CHECK(std::abs(int(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex)[3]) - 128) <= 1);
  // Paused: ordinary VR again.
  REQUIRE(xg::xrPassthroughLayerPauseFB(run.layer) == XR_SUCCESS);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  CHECK(host::last_frame().blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
}
TEST_CASE("Production color key draws the key exactly where passthrough shows and opaque game colour elsewhere") {
  using Pixel = std::array<unsigned char, 4>;
  // Opaque red; fully transparent; premultiplied green at coverage .6; dark
  // blue at coverage .2 (premultiplied). Rows 1 and 3 show the world.
  const std::array<Pixel, 4> rows{{{255, 0, 0, 255}, {0, 0, 0, 0}, {0, 153, 0, 153}, {0, 0, 51, 51}}};
  struct Case { VkFormat format; std::array<std::uint8_t, 3> key; Pixel stored; };
  // The key is sRGB as the user enters it: an sRGB swapchain stores it exactly;
  // a UNORM one holds the same colour as linear light (the runtime encodes it).
  for (const auto& test : {Case{VK_FORMAT_R8G8B8A8_UNORM, {255, 0, 255}, {255, 0, 255, 255}},
                           Case{VK_FORMAT_R8G8B8A8_UNORM, {0, 255, 0}, {0, 255, 0, 255}},
                           Case{VK_FORMAT_R8G8B8A8_SRGB, {153, 0, 153}, {153, 0, 153, 255}},
                           Case{VK_FORMAT_R8G8B8A8_UNORM, {153, 0, 153}, {81, 0, 81, 255}}}) {
    const int format = int(test.format), red = test.key[0], green = test.key[1];
    CAPTURE(format);
    CAPTURE(red);
    CAPTURE(green);
    // Capability is irrelevant: the user's choice replaces every detected backend.
    Run run([&] {
      process_config().passthrough_key = test.key;
      blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
      xr::host_xr().enabled_extensions.insert("XR_HTC_passthrough");
    });
    CHECK(hle::openxr::passthrough_backend() == hle::openxr::PassthroughBackend::color_key);
    CHECK(htc_creates == 0); CHECK(native_creates == 0);
    const auto chain = run.chain(2, test.format);
    fill_rows(chain, rows);
    Submission submit(run, chain);
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    auto frame = host::last_frame();
    CHECK_FALSE(requested);
    CHECK(frame.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    REQUIRE(frame.types.size() == 1); CHECK(frame.types[0] == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    // The host copy is opaque: the host blends it with nothing.
    CHECK_FALSE(bool(frame.projection_flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT));
    for (unsigned eye = 0; eye < 2; ++eye) {
      const auto& sub = frame.views[eye].subImage;
      CAPTURE(eye);
      CHECK(pixel(sub.swapchain, sub.imageArrayIndex, 0) == Pixel{255, 0, 0, 255});
      CHECK(pixel(sub.swapchain, sub.imageArrayIndex, 1) == test.stored);
      CHECK(pixel(sub.swapchain, sub.imageArrayIndex, 3) == test.stored);
      // Coverage at least one half: the straight colour, opaque, no key mixed in.
      if (test.format == VK_FORMAT_R8G8B8A8_UNORM)
        CHECK(near(pixel(sub.swapchain, sub.imageArrayIndex, 2), Pixel{0, 255, 0, 255}));
    }
    if (test.format != VK_FORMAT_R8G8B8A8_UNORM || test.key[0] != 255) continue;
    // Opacity .5: coverage 1 - .5(1 - A) is at least one half everywhere, so
    // no key; transparent black stays black, now opaque.
    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .5f, {0, 0, 0, 0}};
    REQUIRE(xg::xrPassthroughLayerSetStyleFB(run.layer, &style) == XR_SUCCESS);
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    frame = host::last_frame();
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 1) == Pixel{0, 0, 0, 255});
    style.textureOpacityFactor = 1;
    REQUIRE(xg::xrPassthroughLayerSetStyleFB(run.layer, &style) == XR_SUCCESS);
    // Straight alpha (UNPREMULTIPLIED) is keyed the same way.
    submit.projection.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    frame = host::last_frame();
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 1) == test.stored);
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 0) == Pixel{255, 0, 0, 255});
    submit.projection.layerFlags &= ~XrCompositionLayerFlags(XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
    // A game layer without source alpha covers the world: no key.
    submit.projection.layerFlags = 0;
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    frame = host::last_frame();
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 1)[0] == 0);
    submit.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    // Paused: ordinary VR, the game's own pixels and alpha, no key.
    REQUIRE(xg::xrPassthroughLayerPauseFB(run.layer) == XR_SUCCESS);
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    frame = host::last_frame();
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 1) == Pixel{0, 0, 0, 0});
    CHECK(bool(frame.projection_flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT));
    // Resumed: the key again.
    REQUIRE(xg::xrPassthroughLayerResumeFB(run.layer) == XR_SUCCESS);
    REQUIRE(submit.submit(run) == XR_SUCCESS);
    frame = host::last_frame();
    CHECK(pixel(frame.views[0].subImage.swapchain, frame.views[0].subImage.imageArrayIndex, 1) == test.stored);
  }
}
TEST_CASE("Production XR_HTC_passthrough layers keep the guest's layer order") {
  Run run([] {
    xr::host_xr().enabled_extensions.insert("XR_HTC_passthrough");
    host_fb_supported = false; view_spaces = true;
  });
  const auto chain = run.chain();
  fill(chain, {{{1, 0, 0, .5f}, {0, 1, 0, .5f}}});
  Submission submit(run, chain);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  const auto frame = host::last_frame();
  CHECK(frame.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  REQUIRE(frame.types.size() == 2);
  CHECK(frame.types[0] == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC);
  CHECK(frame.types[1] == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
  CHECK(htc_creates == 1); CHECK_FALSE(requested);
}
TEST_CASE("Production OpenVR camera planes are world-locked at the layer's place with its opacity") {
  Run run([] { camera_present = true; view_spaces = true; });
  CHECK(camera_opens == 1);
  const auto chain = run.chain();
  fill(chain, {{{0, 0, 1, 1}, {0, 0, 1, 1}}});
  Submission submit(run, chain);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  auto frame = host::last_frame();
  CHECK(frame.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE); CHECK_FALSE(requested);
  REQUIRE(frame.types.size() == 3);
  CHECK(frame.types[0] == XR_TYPE_COMPOSITION_LAYER_QUAD);
  CHECK(frame.types[1] == XR_TYPE_COMPOSITION_LAYER_QUAD);
  CHECK(frame.types[2] == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
  REQUIRE(frame.quads.size() == 2);
  // Each camera's own eye, its image region, sized by its focal length at the
  // plane distance and placed from the head pose and the camera's offset.
  for (unsigned eye = 0; eye < 2; ++eye) {
    const auto& quad = frame.quads[eye];
    CAPTURE(eye);
    CHECK(quad.eyeVisibility == (eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT));
    CHECK(quad.layerFlags == 0);
    CHECK(quad.subImage.imageRect.offset.y == std::int32_t(eye * 2));
    CHECK(quad.subImage.imageRect.extent.width == 4); CHECK(quad.subImage.imageRect.extent.height == 2);
    CHECK(quad.size.width == doctest::Approx(4)); CHECK(quad.size.height == doctest::Approx(2));
    CHECK(quad.pose.position.x == doctest::Approx(eye ? .03f : -.03f));
    CHECK(quad.pose.position.y == doctest::Approx(1.5f));
    CHECK(quad.pose.position.z == doctest::Approx(-2));
    const auto top = pixel(quad.subImage.swapchain, 0, std::uint32_t(quad.subImage.imageRect.offset.y));
    CHECK(near(top, eye ? std::array<unsigned char, 4>{0, 255, 0, 255} : std::array<unsigned char, 4>{255, 0, 0, 255}));
  }
  // The layer's opacity becomes the planes' alpha.
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .5f, {0, 0, 0, 0}};
  REQUIRE(xg::xrPassthroughLayerSetStyleFB(run.layer, &style) == XR_SUCCESS);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  frame = host::last_frame();
  REQUIRE(frame.quads.size() == 2);
  CHECK(frame.quads[0].layerFlags == (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT));
  CHECK(std::abs(int(pixel(frame.quads[0].subImage.swapchain, 0, 0)[3]) - 128) <= 1);
  // Paused: no camera.
  REQUIRE(xg::xrPassthroughLayerPauseFB(run.layer) == XR_SUCCESS);
  REQUIRE(submit.submit(run) == XR_SUCCESS);
  frame = host::last_frame();
  CHECK(frame.quads.empty()); REQUIRE(frame.types.size() == 1);
}
