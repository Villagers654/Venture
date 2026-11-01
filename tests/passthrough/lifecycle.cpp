#include "support.h"
#include "lifecycle_host.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/passthrough.h"
#include <doctest/doctest.h>
using namespace venture;
using namespace venture::test::passthrough;
namespace xg = venture::hle::openxr::guest;
namespace {
struct Run : Fixture {
  XrInstance instance=reinterpret_cast<XrInstance>(1);
  Run() {
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
