#include "support.h"
#include "hle/openxr/passthrough.h"
#include "xr/passthrough_source_sbn1.h"
#include "xr/tracked_camera.h"
#include "core/process.h"
#include "xr/host_xr.h"
#include <doctest/doctest.h>
#include <deque>
#include <limits>

using namespace venture;
using namespace venture::hle::openxr;
using namespace venture::test::passthrough;
namespace sbn1 = venture::xr::passthrough_source_sbn1;

namespace {
// The layer's form at its place in the ordinary path (forwarded or host form).
const XrCompositionLayerBaseHeader* host_form(const XrCompositionLayerBaseHeader* layer, PassthroughLayerStorage& storage) {
  PassthroughLayerForm form;
  REQUIRE(passthrough_layer_form(target, layer, storage, form) == XR_SUCCESS);
  CHECK(form.guest.empty());
  return form.forward ? form.forward : form.host;
}
struct Frame {
  XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  std::array<XrCompositionLayerProjectionView, 2> views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
  XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
  explicit Frame(Fixture& fixture, XrSwapchain chain = XR_NULL_HANDLE) {
    room.layerHandle = fixture.layer;
    scene.space = space;
    scene.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    scene.viewCount = 2; scene.views = views.data();
    for (unsigned i = 0; i < 2; ++i) {
      views[i].pose.orientation.w = 1; views[i].pose.position.x = i ? .032f : -.032f;
      views[i].fov = {-.8f, .8f, .8f, -.8f};
      views[i].subImage = {chain, {{0, 0}, {4, 4}}, i};
    }
    layers = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&room), reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene)};
    info.displayTime = 1234; info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    info.layerCount = chain ? 2 : 1; info.layers = layers.data();
  }
  XrResult prepare(PassthroughFrame& out) {
    out = PassthroughFrame{};
    bool handled = false;
    auto result = prepare_passthrough_frame(target, info, out, handled);
    if (XR_SUCCEEDED(result) && handled) CHECK(info.layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB);
    return result;
  }
};
}
TEST_CASE("SBN1 records bind the conversation and define every byte") {
  sbn1::Nonce nonce{}; nonce[3] = 127;
  for (bool v2 : {true, false}) {
    CAPTURE(v2);
    const auto request = sbn1::request(42, nonce, v2);
    CHECK(sbn1::read(request, 4, 2) == 1);
    CHECK(sbn1::read(request, 6, 2) == (v2 ? 2u : 1u));
    CHECK(sbn1::read(request, 8, 2) == 128);
    CHECK(sbn1::read(request, 12, 4) == 1); // profile 1: premultiplied SDR RGBA
    CHECK(sbn1::read(request, 16, 4) == 42);
    CHECK(std::equal(nonce.begin(), nonce.end(), request.begin() + 24));
    // The answer: kind 2, server PID, then the grant (binding and generation) or nothing.
    auto answer = request;
    sbn1::write(answer, 4, 2, 2); sbn1::write(answer, 20, 4, 7);
    const auto refused = sbn1::reply(answer, 42, 7, nonce, v2);
    REQUIRE(refused); CHECK_FALSE(refused->rgba);
    auto grant = answer;
    sbn1::write(grant, 10, 2, v2 ? 5 : 1); grant[40] = 9; sbn1::write(grant, 56, 8, 17);
    const auto granted = sbn1::reply(grant, 42, 7, nonce, v2);
    REQUIRE(granted); CHECK(granted->rgba); CHECK(granted->generation == 17);
    CHECK(granted->budget == v2);
    CHECK_FALSE(sbn1::reply(grant, 43, 7, nonce, v2));      // another client
    CHECK_FALSE(sbn1::reply(grant, 42, 8, nonce, v2));      // another server
    CHECK_FALSE(sbn1::reply(grant, 42, 7, {}, v2));         // no nonce
    CHECK_FALSE(sbn1::reply(grant, 42, 7, nonce, !v2));     // another version
    CHECK_FALSE(sbn1::reply(request, 42, 7, nonce, v2));    // a request is not an answer
    for (unsigned byte : {0u, 4u, 6u, 8u, 12u, 16u, 20u, 24u, 39u, 64u, 127u}) {
      auto corrupt = grant; corrupt[byte] ^= 128;
      CAPTURE(byte);
      CHECK_FALSE(sbn1::reply(corrupt, 42, 7, nonce, v2));
    }
    // A grant needs a binding and a generation; a refusal carries neither.
    auto no_generation = grant; sbn1::write(no_generation, 56, 8, 0);
    CHECK_FALSE(sbn1::reply(no_generation, 42, 7, nonce, v2));
    auto stray = answer; stray[40] = 1;
    CHECK_FALSE(sbn1::reply(stray, 42, 7, nonce, v2));
    auto unknown = grant; sbn1::write(unknown, 10, 2, 8);
    CHECK_FALSE(sbn1::reply(unknown, 42, 7, nonce, v2));
  }
}
TEST_CASE("Passthrough keeps the upstream compatibility declaration without a usable backend") {
  Fixture fixture;
  CHECK(capabilities().contains("XR_FB_passthrough"));
  CHECK_FALSE(capabilities().contains("XR_FB_triangle_mesh"));
  configured = false;
  CHECK(capabilities().contains("XR_FB_passthrough"));
  CHECK_FALSE(local_passthrough());
  XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  XrPassthroughFB feature{};
  CHECK(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_ERROR_FEATURE_UNSUPPORTED);
  XrPassthroughLayerCreateInfoFB layer_create{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  XrPassthroughLayerFB layer{};
  CHECK(guest::xrCreatePassthroughLayerFB(target, &layer_create, &layer) == XR_ERROR_FEATURE_UNSUPPORTED);
  process_config().abi = "armeabi-v7a";
  CHECK(capabilities().contains("XR_FB_passthrough"));
  CHECK_FALSE(local_passthrough());
  CHECK(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_ERROR_FEATURE_UNSUPPORTED);
  process_config().abi = "arm64-v8a";
  fixture.create(true);
  CHECK(capabilities().contains("XR_FB_passthrough"));
}
TEST_CASE("Passthrough opt-out blocks native and local creation and properties") {
  Fixture fixture;
  REQUIRE(capabilities().contains("XR_FB_passthrough"));
  process_config().passthrough = false;
  for (const bool native : {false, true}) {
    if (native) xr::host_xr().enabled_extensions.insert("XR_FB_passthrough");
    CHECK(capabilities().contains("XR_FB_passthrough"));
    CHECK_FALSE(local_passthrough());
    XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB, nullptr, ~XrPassthroughCapabilityFlagsFB(0)};
    XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer, XR_TRUE};
    CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&older)));
    CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&newer)));
    CHECK(older.supportsPassthrough == XR_FALSE); CHECK(older.next == &newer);
    CHECK(newer.capabilities == 0);
    XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    XrPassthroughFB feature{};
    CHECK(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_ERROR_FEATURE_UNSUPPORTED);
    XrPassthroughLayerCreateInfoFB layer_create{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    XrPassthroughLayerFB layer{};
    CHECK(guest::xrCreatePassthroughLayerFB(target, &layer_create, &layer) == XR_ERROR_FEATURE_UNSUPPORTED);
    CHECK(native_creates == 0); CHECK_FALSE(requested);
  }
  process_config().passthrough = true;
  fixture.create(true);
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
  CHECK_FALSE(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&older))); // host fills it
  CHECK(native_creates == 1);
}
TEST_CASE("Passthrough lifecycle validates handles and refuses unimplemented camera effects") {
  Fixture fixture; fixture.create();
  CHECK_FALSE(requested);
  XrPassthroughFB second{};
  XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  CHECK(guest::xrCreatePassthroughFB(target, &create, &second) == XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB);
  XrPassthroughLayerCreateInfoFB projected{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, fixture.feature, 0, XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB};
  XrPassthroughLayerFB layer{};
  CHECK(guest::xrCreatePassthroughLayerFB(target, &projected, &layer) == XR_ERROR_FEATURE_UNSUPPORTED);
  CHECK(layer == XR_NULL_HANDLE);
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, 1, {0, 0, 0, 0}};
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  style.textureOpacityFactor = .5f;
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  style.textureOpacityFactor = std::numeric_limits<float>::quiet_NaN();
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(guest::xrPassthroughLayerResumeFB(XR_NULL_HANDLE) == XR_ERROR_HANDLE_INVALID);
  auto old_layer = fixture.layer;
  CHECK(guest::xrDestroyPassthroughFB(fixture.feature) == XR_SUCCESS);
  CHECK(guest::xrPassthroughLayerResumeFB(old_layer) == XR_ERROR_HANDLE_INVALID);
  state().enabled.erase("XR_FB_passthrough");
  CHECK(guest::xrCreatePassthroughFB(target, &create, &second) == XR_ERROR_FUNCTION_UNSUPPORTED);
}
TEST_CASE("Passthrough native handles are forwarded and host errors propagate") {
  Fixture fixture; fixture.create(true);
  CHECK(native_creates == 1);
  CHECK(fixture.feature == reinterpret_cast<XrPassthroughFB>(0x300));
  native_result = XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB;
  CHECK(guest::xrPassthroughStartFB(fixture.feature) == native_result);
  native_result = XR_SUCCESS;
  Frame frame(fixture);
  CHECK(validate_passthrough_layer(target, frame.layers[0]) == XR_SUCCESS);
  PassthroughFrame output; bool handled = true;
  CHECK(prepare_passthrough_frame(target, frame.info, output, handled) == XR_SUCCESS);
  CHECK_FALSE(handled); CHECK_FALSE(requested);
  CHECK(destroy_passthrough_session(target) == XR_SUCCESS);
  CHECK(native_destroys == 2);
}
TEST_CASE("Passthrough properties preserve output chains and omit depth capability") {
  Fixture fixture;
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer, XR_FALSE};
  CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&older)));
  CHECK(older.supportsPassthrough == XR_TRUE); CHECK(older.next == &newer);
  CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&newer)));
  CHECK(newer.capabilities == (XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB));
  state().enabled.erase("XR_FB_passthrough");
  CHECK_FALSE(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&older)));
}
TEST_CASE("Passthrough header erratum accepts native composition extension chains") {
  Fixture fixture; fixture.create(true);
  state().enabled.insert("XR_KHR_composition_layer_color_scale_bias");
  xr::host_xr().enabled_extensions.insert("XR_KHR_composition_layer_color_scale_bias");
  XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR,
      nullptr, {1, 1, 1, 1}, {0, 0, 0, 0}};
  Frame frame(fixture);
  frame.room.next = &color;
  CHECK(validate_passthrough_layer(target, frame.layers[0]) == XR_SUCCESS);
  CHECK(validate_chain(&color, false, XR_TYPE_PASSTHROUGH_CREATE_INFO_FB) == XR_ERROR_VALIDATION_FAILURE);
}
TEST_CASE("Passthrough source intent follows submitted running layers and is revoked on pause") {
  Fixture fixture; fixture.create(); Frame frame(fixture); PassthroughFrame output;
  CHECK(frame.prepare(output) == XR_SUCCESS);
  CHECK(requested); CHECK(output.permission == 0); CHECK(output.info.layerCount == 0);
  grant = true;
  CHECK(frame.prepare(output) == XR_SUCCESS);
  CHECK(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.permission != 0); CHECK(passthrough_permission_current(output.permission));
  CHECK(guest::xrPassthroughLayerPauseFB(fixture.layer) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK_FALSE(passthrough_permission_current(output.permission));
  CHECK(frame.prepare(output) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK(guest::xrPassthroughLayerResumeFB(fixture.layer) == XR_SUCCESS);
  CHECK(frame.prepare(output) == XR_SUCCESS); CHECK(requested);
  CHECK(guest::xrPassthroughPauseFB(fixture.feature) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK(guest::xrPassthroughStartFB(fixture.feature) == XR_SUCCESS);
  CHECK(frame.prepare(output) == XR_SUCCESS); CHECK(requested);
  frame.info.layerCount = 0;
  bool handled = true;
  CHECK(prepare_passthrough_frame(target, frame.info, output, handled) == XR_SUCCESS);
  CHECK_FALSE(handled); CHECK_FALSE(requested);
}
TEST_CASE("Passthrough ignores disabled composition extensions without changing current eyes") {
  Fixture fixture; fixture.create(); grant = true; auto chain = fixture.chain();
  fill(chain, {{{1, 0, 0, .5f}, {0, 1, 0, .25f}}});
  Frame frame(fixture, chain); PassthroughFrame output;
  XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR,
      nullptr, {0, 0, 0, 0}, {1, 1, 1, 1}};
  // Spirit Sling submits this node without enabling its extension. A host-only
  // enable must not accidentally apply it to the guest's pixels, either.
  xr::host_xr().enabled_extensions.insert("XR_KHR_composition_layer_color_scale_bias");
  frame.scene.next = &color;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.permission != 0);
  CHECK(output.projection.next == nullptr);
  CHECK(near(pixel(output.views[0].subImage.swapchain), {255, 0, 0, 128}));
  CHECK(near(pixel(output.views[1].subImage.swapchain), {0, 255, 0, 64}));
  CHECK(frame.scene.next == &color);
  frame.room.next = &color;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  // Enabling the extension makes it meaningful: refuse an unsupported camera
  // transform rather than silently discarding its requested effect.
  state().enabled.insert("XR_KHR_composition_layer_color_scale_bias");
  CHECK(frame.prepare(output) == XR_ERROR_FEATURE_UNSUPPORTED);
  CHECK_FALSE(requested);
  frame.room.next = nullptr;
  CHECK(frame.prepare(output) == XR_ERROR_FEATURE_UNSUPPORTED);
  state().enabled.erase("XR_KHR_composition_layer_color_scale_bias");
  color.next = &color;
  CHECK(frame.prepare(output) == XR_ERROR_VALIDATION_FAILURE);
  CHECK_FALSE(requested);
}
TEST_CASE("Passthrough splits actual array eyes and preserves fractional and additive RGBA") {
  for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
    CAPTURE(format);
    Fixture fixture; fixture.create(); grant = true;
    auto chain = fixture.chain(2, format);
    fill(chain, {{{.25f, .5f, .75f, .5f}, {.75f, .25f, .5f, 0}}});
    auto original_left = pixel(chain, 0), original_right = pixel(chain, 1);
    Frame frame(fixture, chain); PassthroughFrame output;
    REQUIRE(frame.prepare(output) == XR_SUCCESS); // ordinary bootstrap stereo pair
    REQUIRE(frame.prepare(output) == XR_SUCCESS);
    CHECK(output.permission != 0); CHECK(output.info.layerCount == 1);
    CHECK(output.views[0].subImage.swapchain != output.views[1].subImage.swapchain);
    for (unsigned eye = 0; eye < 2; ++eye) {
      CHECK(output.views[eye].subImage.imageArrayIndex == 0);
      CHECK(output.views[eye].pose.position.x == frame.views[eye].pose.position.x);
      CHECK(output.views[eye].fov.angleLeft == frame.views[eye].fov.angleLeft);
      auto copied = pixel(output.views[eye].subImage.swapchain);
      auto expected = eye ? original_right : original_left;
      for (unsigned channel = 0; channel < 4; ++channel) CHECK(std::abs(int(copied[channel]) - int(expected[channel])) <= 1);
    }
    CHECK(pixel(chain, 0) == original_left); CHECK(pixel(chain, 1) == original_right);
    // Current right eye replaces its own old data; the left cannot substitute.
    fill(chain, {{{0, 1, 0, 1}, {1, 0, 0, .25f}}});
    REQUIRE(frame.prepare(output) == XR_SUCCESS);
    CHECK(pixel(output.views[0].subImage.swapchain) == std::array<unsigned char, 4>{0, 255, 0, 255});
    CHECK(pixel(output.views[1].subImage.swapchain) == std::array<unsigned char, 4>{255, 0, 0, 64});
    // Neither the guest view array nor its original source handles were edited.
    CHECK(frame.views[0].subImage.swapchain == chain); CHECK(frame.views[1].subImage.imageArrayIndex == 1);
  }
}
TEST_CASE("Passthrough translates straight alpha once and ignores undefined alpha on opaque layers") {
  Fixture fixture; fixture.create(); grant = true; auto chain = fixture.chain();
  fill(chain, {{{1, .5f, 0, .5f}, {0, .5f, 1, .25f}}});
  Frame frame(fixture, chain); PassthroughFrame output;
  frame.scene.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  auto left = pixel(output.views[0].subImage.swapchain);
  CHECK(near(left, {128, 64, 0, 128}));
  CHECK(output.projection.layerFlags == 0);
  frame.scene.layerFlags = 0;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(near(pixel(output.views[0].subImage.swapchain), {255, 128, 0, 255}));
  CHECK(near(pixel(chain, 0), {255, 128, 0, 128}));
}
TEST_CASE("Passthrough opacity changes camera coverage without darkening scene contribution") {
  Fixture fixture; fixture.create(); grant = true; auto chain = fixture.chain();
  fill(chain, {{{1, 0, 0, 0}, {0, 1, 0, .5f}}});
  Frame frame(fixture, chain); PassthroughFrame output;
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .5f, {0, 0, 0, 0}};
  REQUIRE(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  frame.room.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(near(pixel(output.views[0].subImage.swapchain), {255, 0, 0, 128}));
  auto right = pixel(output.views[1].subImage.swapchain);
  CHECK(right[1] == 255); CHECK(std::abs(int(right[3]) - 192) <= 1);
  suspend_passthrough_source(); CHECK_FALSE(requested);
}
TEST_CASE("Passthrough missing or withdrawn permission submits opaque current eyes") {
  Fixture fixture; fixture.create(); auto chain = fixture.chain();
  fill(chain, {{{1, 0, 0, 0}, {0, 1, 0, .5f}}});
  Frame frame(fixture, chain); PassthroughFrame output;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.permission == 0);
  CHECK(pixel(output.views[0].subImage.swapchain) == std::array<unsigned char, 4>{255, 0, 0, 255});
  grant = true;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(pixel(output.views[0].subImage.swapchain)[3] == 0);
  grant = false;
  REQUIRE(make_passthrough_frame_opaque(target, output) == XR_SUCCESS);
  CHECK(output.permission == 0);
  CHECK(pixel(output.views[0].subImage.swapchain)[3] == 255);
  CHECK(pixel(output.views[1].subImage.swapchain)[3] == 255);
  CHECK(guest::xrPassthroughLayerPauseFB(fixture.layer) == XR_SUCCESS);
  bool handled = true;
  REQUIRE(prepare_passthrough_frame(target, frame.info, output, handled) == XR_SUCCESS); CHECK_FALSE(requested);
  // The ordinary path leaves the paused layer out of the host frame.
  CHECK_FALSE(handled); CHECK(output.permission == 0);
  PassthroughLayerStorage storage;
  CHECK(host_form(frame.layers[0], storage) == nullptr);
  frame.layers[0] = frame.layers[1]; frame.info.layerCount = 1;
  REQUIRE(frame.prepare(output) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK(output.permission == 0);
}
TEST_CASE("Passthrough rejects unproved eyes and unsupported composition before granting a source") {
  Fixture fixture; fixture.create(); auto chain = fixture.chain();
  Frame frame(fixture, chain); PassthroughFrame output;
  frame.views[1].subImage.imageArrayIndex = 2;
  CHECK(frame.prepare(output) == XR_ERROR_SWAPCHAIN_RECT_INVALID); CHECK_FALSE(requested);
  frame.views[1].subImage.imageArrayIndex = 1;
  frame.views[1].subImage.imageRect.extent.width = 5;
  CHECK(frame.prepare(output) == XR_ERROR_SWAPCHAIN_RECT_INVALID); CHECK_FALSE(requested);
  frame.views[1].subImage.imageRect.extent.width = 4;
  std::swap(frame.layers[0], frame.layers[1]);
  CHECK(frame.prepare(output) == XR_ERROR_FEATURE_UNSUPPORTED); CHECK_FALSE(requested);
  std::swap(frame.layers[0], frame.layers[1]);
  frame.room.space = space;
  CHECK(frame.prepare(output) == XR_ERROR_LAYER_INVALID); CHECK_FALSE(requested);
  frame.room.space = XR_NULL_HANDLE;
  state().swapchains.at(chain).released_index = -1;
  CHECK(frame.prepare(output) == XR_ERROR_LAYER_INVALID); CHECK_FALSE(requested);
}
TEST_CASE("Passthrough preserves submitted rects with image flips and GLES origin conversion") {
  Fixture fixture; fixture.create(); auto chain = fixture.chain();
  Frame frame(fixture, chain); PassthroughFrame output;
  frame.views[0].subImage.imageRect = {{0, 0}, {3, 1}};
  XrCompositionLayerImageLayoutFB layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr,
      XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB};
  frame.scene.next = &layout;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.views[0].subImage.imageRect.offset.y == 3);
  CHECK(frame.views[0].subImage.imageRect.offset.y == 0);
  state().swapchains.at(chain).textures = {123}; // normalized GLES backing has a bottom-origin guest rect
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.views[0].subImage.imageRect.offset.y == 0);
  frame.scene.next = nullptr;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(output.views[0].subImage.imageRect.offset.y == 3);
}

TEST_CASE("Unknown and disabled passthrough next structures are ignored") {
  Fixture fixture;
  XrBaseInStructure unknown{XrStructureType(0x7ffffffe), nullptr};
  XrCompositionLayerColorScaleBiasKHR disabled{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  disabled.next = &unknown;
  XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, &disabled, XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
  REQUIRE(guest::xrCreatePassthroughFB(target, &create, &fixture.feature) == XR_SUCCESS);
  XrPassthroughLayerCreateInfoFB layer{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, &disabled, fixture.feature,
      XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
  REQUIRE(guest::xrCreatePassthroughLayerFB(target, &layer, &fixture.layer) == XR_SUCCESS);
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, &disabled, 1, {0,0,0,0}};
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  XrPassthroughColorMapMonoToRgbaFB unsupported{XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB};
  style.next = &unsupported;
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_ERROR_FEATURE_UNSUPPORTED);
  unknown.next = &unknown; style.next = &unknown;
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_ERROR_VALIDATION_FAILURE);
}
TEST_CASE("MR eyes fit host maximum and map cropped rectangles after flipping") {
  Fixture fixture; fixture.create(); const auto chain = fixture.chain();
  fill(chain, {{{1,0,0,.25f},{0,1,0,.75f}}});
  maximum_width = maximum_height = 2; grant = true;
  Frame frame(fixture, chain); PassthroughFrame output;
  for (auto& view : frame.views) view.subImage.imageRect = {{1,1},{2,2}};
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  for (const auto& view : output.views) {
    CHECK(view.subImage.imageRect.offset.x == 1); CHECK(view.subImage.imageRect.offset.y == 1);
    CHECK(view.subImage.imageRect.extent.width == 1); CHECK(view.subImage.imageRect.extent.height == 1);
  }
  CHECK(pixel(output.views[0].subImage.swapchain)[0] == 255);
  CHECK(pixel(output.views[1].subImage.swapchain)[1] == 255);
}
TEST_CASE("Ordinary VR leaves the MR eye path while withdrawal is pending and after retirement") {
  Fixture fixture; fixture.create(); const auto chain = fixture.chain();
  fill(chain, {{{1,0,0,0},{0,1,0,.5f}}}); grant = true;
  Frame frame(fixture, chain); PassthroughFrame mr;
  REQUIRE(frame.prepare(mr) == XR_SUCCESS); REQUIRE(frame.prepare(mr) == XR_SUCCESS);
  frame.layers[0] = frame.layers[1]; frame.info.layerCount = 1;
  withdrawal_complete = false; PassthroughFrame handoff;
  REQUIRE(frame.prepare(handoff) == XR_SUCCESS); CHECK(handoff.force_opaque); CHECK_FALSE(requested);
  withdrawal_complete = true; PassthroughFrame vr;
  REQUIRE(frame.prepare(vr) == XR_SUCCESS); CHECK_FALSE(vr.force_opaque);
}

TEST_CASE("MR downscaling keeps a one-pixel edge crop within the host image") {
  Fixture fixture; fixture.create(); const auto chain = fixture.chain();
  fill(chain, {{{1,0,0,.25f},{0,1,0,.75f}}});
  maximum_width = maximum_height = 1; grant = true;
  Frame frame(fixture, chain); PassthroughFrame output;
  for (auto& view : frame.views) view.subImage.imageRect = {{3,3},{1,1}};
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  for (const auto& view : output.views) {
    CHECK(view.subImage.imageRect.offset.x == 0); CHECK(view.subImage.imageRect.offset.y == 0);
    CHECK(view.subImage.imageRect.extent.width == 1); CHECK(view.subImage.imageRect.extent.height == 1);
  }
}

TEST_CASE("Passthrough backends follow host capability in order of fidelity, never the runtime name") {
  Fixture fixture;
  auto& host = xr::host_xr();
  const auto pick = [] { reset_passthrough_probe(); return passthrough_backend(); };
  host.runtime_name = "SteamVR/OpenXR"; // names never matter
  host.enabled_extensions = {"XR_FB_passthrough", "XR_HTC_passthrough"};
  blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ADDITIVE, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
  CHECK(pick() == PassthroughBackend::native);
  host_fb_supported = false; // the extension without a camera behind it
  CHECK(pick() == PassthroughBackend::htc);
  host.enabled_extensions.erase("XR_HTC_passthrough");
  CHECK(pick() == PassthroughBackend::blend);
  CHECK(passthrough_blend_mode() == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
  blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ADDITIVE};
  CHECK(pick() == PassthroughBackend::blend);
  CHECK(passthrough_blend_mode() == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
  blend_modes.clear();
  camera_present = true;
  CHECK(pick() == PassthroughBackend::camera);
  camera_present = false;
  CHECK(pick() == PassthroughBackend::external);
  configured = false;
  CHECK(pick() == PassthroughBackend::none);
  host.runtime_name = "Monado";
  blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
  process_config().passthrough = false;
  CHECK(pick() == PassthroughBackend::none);
  process_config().passthrough = true;
  process_config().abi = "armeabi-v7a";
  CHECK(pick() == PassthroughBackend::none);
  process_config().abi = "arm64-v8a";
  CHECK(pick() == PassthroughBackend::blend);
  host.runtime_name.clear();
}
TEST_CASE("Passthrough through host XR_HTC_passthrough keeps layer order, opacity and pauses") {
  Fixture fixture;
  xr::host_xr().enabled_extensions.insert("XR_HTC_passthrough");
  host_fb_supported = false; view_spaces = true;
  REQUIRE(passthrough_backend() == PassthroughBackend::htc);
  CHECK(local_passthrough());
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer, XR_FALSE};
  CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&older)));
  CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&newer)));
  CHECK(older.supportsPassthrough == XR_TRUE);
  CHECK(newer.capabilities == (XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB));
  fixture.create();
  CHECK(native_creates == 0); CHECK(htc_creates == 1); CHECK(spaces == 1);
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .25f, {0, 0, 0, 0}};
  REQUIRE(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  Frame frame(fixture);
  frame.room.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  REQUIRE(validate_passthrough_layer(target, frame.layers[0]) == XR_SUCCESS);
  PassthroughFrame output; bool handled = true;
  REQUIRE(prepare_passthrough_frame(target, frame.info, output, handled) == XR_SUCCESS);
  CHECK_FALSE(handled); CHECK(output.first_layer == 0);
  CHECK(output.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE); CHECK_FALSE(requested);
  PassthroughLayerStorage storage;
  const auto* layer = host_form(frame.layers[0], storage);
  REQUIRE(layer != nullptr);
  REQUIRE(layer->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC);
  const auto& htc = *reinterpret_cast<const XrCompositionLayerPassthroughHTC*>(layer);
  CHECK(htc.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
  CHECK(htc.passthrough == reinterpret_cast<XrPassthroughHTC>(std::uintptr_t(0x601)));
  CHECK(htc.space != XR_NULL_HANDLE);
  CHECK(htc.color.type == XR_TYPE_PASSTHROUGH_COLOR_HTC);
  CHECK(htc.color.alpha == doctest::Approx(.25f));
  REQUIRE(guest::xrPassthroughPauseFB(fixture.feature) == XR_SUCCESS);
  CHECK(host_form(frame.layers[0], storage) == nullptr);
  REQUIRE(guest::xrPassthroughStartFB(fixture.feature) == XR_SUCCESS);
  CHECK(host_form(frame.layers[0], storage) != nullptr);
  CHECK(guest::xrDestroyPassthroughLayerFB(fixture.layer) == XR_SUCCESS);
  CHECK(htc_destroys == 1); CHECK(spaces == 0);
  // Without a host VIEW space no HTC layer can be placed: HTC stops and, with
  // no other backend, creation fails cleanly.
  view_spaces = false; configured = false;
  XrPassthroughLayerCreateInfoFB again{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, fixture.feature,
      0, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
  XrPassthroughLayerFB failed{};
  CHECK(guest::xrCreatePassthroughLayerFB(target, &again, &failed) == XR_ERROR_FEATURE_UNSUPPORTED);
  CHECK(failed == XR_NULL_HANDLE); CHECK(htc_creates == htc_destroys); CHECK(spaces == 0);
  CHECK(passthrough_backend() == PassthroughBackend::none);
  CHECK(local_passthrough()); // the guest's objects stay local
}
TEST_CASE("Passthrough through a see-through blend mode covers lower layers and carries opacity") {
  Fixture fixture;
  blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
  REQUIRE(passthrough_backend() == PassthroughBackend::blend);
  fixture.create();
  CHECK(native_creates == 0); CHECK(htc_creates == 0);
  auto chain = fixture.chain();
  Frame frame(fixture, chain); PassthroughFrame output; bool handled = true;
  REQUIRE(prepare_passthrough_frame(target, frame.info, output, handled) == XR_SUCCESS);
  CHECK_FALSE(handled); CHECK_FALSE(requested);
  CHECK(output.first_layer == 1);
  CHECK(output.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
  CHECK(output.underlay_opacity == 1);
  PassthroughLayerStorage storage;
  CHECK(host_form(frame.layers[0], storage) == nullptr);
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, .5f, {0, 0, 0, 0}};
  REQUIRE(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_SUCCESS);
  REQUIRE(prepare_passthrough_frame(target, frame.info, output = {}, handled) == XR_SUCCESS);
  CHECK(output.underlay_opacity == doctest::Approx(.5f));
  // A game layer under the camera is hidden by it, as on a Quest.
  std::swap(frame.layers[0], frame.layers[1]);
  REQUIRE(prepare_passthrough_frame(target, frame.info, output = {}, handled) == XR_SUCCESS);
  CHECK(output.first_layer == 2);
  std::swap(frame.layers[0], frame.layers[1]);
  // Paused: ordinary VR, the guest's own blend mode.
  REQUIRE(guest::xrPassthroughLayerPauseFB(fixture.layer) == XR_SUCCESS);
  REQUIRE(prepare_passthrough_frame(target, frame.info, output = {}, handled) == XR_SUCCESS);
  CHECK(output.first_layer == 0); CHECK(output.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  CHECK(output.underlay_opacity == 1);
  REQUIRE(guest::xrPassthroughLayerResumeFB(fixture.layer) == XR_SUCCESS);
  // Optical see-through cannot hide the world: opacity is not emulated.
  blend_modes = {XR_ENVIRONMENT_BLEND_MODE_ADDITIVE};
  reset_passthrough_probe();
  REQUIRE(prepare_passthrough_frame(target, frame.info, output = {}, handled) == XR_SUCCESS);
  CHECK(output.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
  CHECK(output.underlay_opacity == 1);
  // Camera effects the blend mode cannot show are refused, as with every local backend.
  XrPassthroughLayerCreateInfoFB projected{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, fixture.feature,
      0, XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB};
  XrPassthroughLayerFB layer{};
  CHECK(guest::xrCreatePassthroughLayerFB(target, &projected, &layer) == XR_ERROR_FEATURE_UNSUPPORTED);
  style.edgeColor.a = 1;
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_ERROR_FEATURE_UNSUPPORTED);
}

TEST_CASE("Passthrough falls back to the next backend that actually starts, before the guest commits") {
  Fixture fixture;
  auto& host = xr::host_xr();
  // Native creation failing on the host: the next backend takes the guest's call.
  host.enabled_extensions = {"XR_FB_passthrough", "XR_HTC_passthrough"};
  view_spaces = true;
  native_result = XR_ERROR_RUNTIME_FAILURE;
  XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  XrPassthroughFB feature{};
  REQUIRE(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_SUCCESS);
  CHECK(native_creates == 1);
  CHECK(passthrough_backend() == PassthroughBackend::htc);
  CHECK(htc_creates == 1); CHECK(htc_destroys == 0); // HTC proved it starts; kept for the first layer
  CHECK(last_report["backend"] == "host XR_HTC_passthrough");
  CHECK(last_report["backends"][0]["status"].get<std::string>().find("xrCreatePassthroughFB failed") != std::string::npos);
  CHECK(guest::xrDestroyPassthroughFB(feature) == XR_SUCCESS);
  CHECK(htc_destroys == 1);
  native_result = XR_SUCCESS;
  // A camera that does not stream gives way to the external source.
  reset_passthrough_probe();
  host.enabled_extensions.clear();
  camera_present = true; camera_streams = false;
  CHECK(passthrough_backend() == PassthroughBackend::camera);
  REQUIRE(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_SUCCESS);
  CHECK(camera_opens == 1);
  CHECK(passthrough_backend() == PassthroughBackend::external);
  CHECK(guest::xrDestroyPassthroughFB(feature) == XR_SUCCESS);
  // Nothing starts: unsupported before the guest has an object.
  reset_passthrough_probe();
  configured = false;
  CHECK(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_ERROR_FEATURE_UNSUPPORTED);
  CHECK(camera_opens == 2);
  CHECK(feature == XR_NULL_HANDLE);
  CHECK(last_report["backend"] == "none");
}
TEST_CASE("Passthrough camera backend offers what a camera can show and stops when frames stop") {
  Fixture fixture;
  camera_present = true;
  REQUIRE(passthrough_backend() == PassthroughBackend::camera);
  const auto caps = passthrough_capabilities(PassthroughBackend::camera);
  CHECK(caps.color); CHECK(caps.opacity); CHECK(caps.layer_order);
  CHECK_FALSE(caps.depth); CHECK_FALSE(caps.styles); CHECK_FALSE(caps.projected); CHECK_FALSE(caps.forwarded);
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  CHECK(passthrough_properties(reinterpret_cast<XrBaseOutStructure*>(&newer)));
  CHECK(newer.capabilities == (XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB));
  fixture.create();
  CHECK(camera_opens == 1);
  XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, 1, {1, 0, 0, 1}};
  CHECK(guest::xrPassthroughLayerSetStyleFB(fixture.layer, &style) == XR_ERROR_FEATURE_UNSUPPORTED);
  // Frames stop: the camera is given up and the external source takes over.
  camera_stalled = true;
  Frame frame(fixture);
  PassthroughLayerStorage storage;
  PassthroughLayerForm form;
  REQUIRE(passthrough_layer_form(target, frame.layers[0], storage, form) == XR_SUCCESS);
  CHECK(form.guest.empty());
  CHECK(passthrough_backend() == PassthroughBackend::external);
  CHECK(local_passthrough());
}
TEST_CASE("Passthrough reports the external source only once it answers") {
  Fixture fixture;
  configured = false;
  CHECK(passthrough_backend() == PassthroughBackend::none);
  CHECK(last_report["backends"][4]["status"] == "no Steam Link passthrough companion answered");
  configured = true;
  reset_passthrough_probe();
  CHECK(passthrough_backend() == PassthroughBackend::external);
  CHECK(last_report["backends"][4]["status"] == "used");
  CHECK(last_report["backends"][0]["status"] == "the runtime does not offer XR_FB_passthrough");
}
TEST_CASE("OpenVR camera geometry comes from the API's transforms and layout") {
  // Vertical stereo: top/bottom = left/right, the left camera is index 1.
  const auto vertical = xr::tracked_camera_regions(2 | 16, 960, 1920, 2);
  REQUIRE(vertical.size() == 2);
  CHECK(vertical[0].y == 0); CHECK(vertical[0].height == 960); CHECK(vertical[0].camera == 1);
  CHECK(vertical[1].y == 960); CHECK(vertical[1].camera == 0);
  const auto horizontal = xr::tracked_camera_regions(2 | 32, 1280, 480, 2);
  REQUIRE(horizontal.size() == 2);
  CHECK(horizontal[1].x == 640); CHECK(horizontal[0].camera == 0); CHECK(horizontal[1].camera == 1);
  CHECK(xr::tracked_camera_regions(1, 640, 480, 1).size() == 1);
  CHECK(xr::tracked_camera_regions(2 | 16, 640, 960, 1).size() == 1);
  // A scaled rotation about Y by 90 degrees with a translation.
  const float turned[3][4] = {{0, 0, 2, .1f}, {0, 2, 0, .2f}, {-2, 0, 0, .3f}};
  const auto pose = xr::tracked_camera_pose(turned);
  CHECK(pose.position.x == doctest::Approx(.1f)); CHECK(pose.position.z == doctest::Approx(.3f));
  CHECK(pose.orientation.y == doctest::Approx(std::sqrt(.5f)));
  CHECK(pose.orientation.w == doctest::Approx(std::sqrt(.5f)));
  // Image axes (+Z forward, +Y down) are turned back to look along -Z.
  const float flipped[3][4] = {{1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, -1, 0}};
  const auto upright = xr::tracked_camera_pose(flipped);
  CHECK(std::abs(upright.orientation.w) == doctest::Approx(1));
}
