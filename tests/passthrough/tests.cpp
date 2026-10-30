#include "support.h"
#include "hle/openxr/passthrough.h"
#include "xr/steam_link_protocol.h"
#include "core/process.h"
#include <doctest/doctest.h>
#include <limits>

using namespace venture;
using namespace venture::hle::openxr;
using namespace venture::test::passthrough;
namespace wire = venture::xr::steam_link;

namespace {
wire::Record accepted(const wire::Nonce& nonce, bool budget = true) {
  auto record = wire::request(42, nonce, budget);
  wire::write(record, 4, 2, 2); wire::write(record, 10, 2, budget ? 5 : 1);
  wire::write(record, 20, 4, 84); record[40] = 5; wire::write(record, 56, 8, 17);
  return record;
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
    bool handled = false;
    auto result = prepare_passthrough_frame(target, info, out, handled);
    if (XR_SUCCEEDED(result)) CHECK(handled);
    return result;
  }
};
}
TEST_CASE("SBN1 parses format permission independently of operational coverage") {
  wire::Nonce nonce{}; nonce[3] = 127;
  for (bool budget : {false, true}) {
    auto record = accepted(nonce, budget);
    auto parsed = wire::reply(record, 42, 84, nonce, budget);
    REQUIRE(parsed); CHECK(parsed->rgba); CHECK(parsed->budget == budget); CHECK(parsed->generation == 17);
    record[10] |= 2; CHECK(wire::reply(record, 42, 84, nonce, budget).has_value());
    CHECK_FALSE(wire::reply(record, 42, 84, nonce, !budget));
    CHECK_FALSE(wire::reply(record, 43, 84, nonce, budget));
    CHECK_FALSE(wire::reply(record, 42, 85, nonce, budget));
    auto denied = wire::request(42, nonce, budget);
    wire::write(denied, 4, 2, 2); wire::write(denied, 20, 4, 84);
    auto negative = wire::reply(denied, 42, 84, nonce, budget);
    REQUIRE(negative); CHECK_FALSE(negative->rgba);
    for (unsigned byte : {0u, 4u, 6u, 8u, 12u, 16u, 20u, 24u, 39u, 64u, 127u}) {
      auto corrupt = accepted(nonce, budget); corrupt[byte] ^= 128;
      CAPTURE(byte); CAPTURE(budget); CHECK_FALSE(wire::reply(corrupt, 42, 84, nonce, budget));
    }
    for (unsigned byte : {10u, 40u, 56u}) {
      auto corrupt = accepted(nonce, budget); corrupt[byte] = 0;
      CHECK_FALSE(wire::reply(corrupt, 42, 84, nonce, budget));
    }
    record = accepted(nonce, budget); record[10] = 4;
    CHECK_FALSE(wire::reply(record, 42, 84, nonce, budget));
  }
  CHECK_FALSE(wire::reply(accepted(nonce), 0, 84, nonce, true));
  CHECK_FALSE(wire::reply(accepted(nonce), 42, 84, {}, true));
}
TEST_CASE("Passthrough capabilities require a real backend and A64 layouts") {
  Fixture fixture;
  CHECK(capabilities().contains("XR_FB_passthrough"));
  CHECK_FALSE(capabilities().contains("XR_FB_triangle_mesh"));
  configured = false;
  CHECK_FALSE(capabilities().contains("XR_FB_passthrough"));
  fixture.create(true);
  CHECK(capabilities().contains("XR_FB_passthrough"));
  process_config().abi = "armeabi-v7a";
  CHECK_FALSE(capabilities().contains("XR_FB_passthrough"));
  process_config().abi = "arm64-v8a";
}
TEST_CASE("Saved Steam Frame opt-out withholds the emulated passthrough API") {
  Fixture fixture;
  REQUIRE(capabilities().contains("XR_FB_passthrough"));
  process_config().steam_frame_passthrough = false;
  CHECK_FALSE(capabilities().contains("XR_FB_passthrough"));
  XrPassthroughCreateInfoFB create{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  XrPassthroughFB feature{};
  CHECK(guest::xrCreatePassthroughFB(target, &create, &feature) == XR_ERROR_FEATURE_UNSUPPORTED);
  CHECK_FALSE(requested);
  // A native host capability remains independent of this Steam Frame option.
  fixture.create(true);
  CHECK(capabilities().contains("XR_FB_passthrough"));
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
  CHECK(pixel(output.views[0].subImage.swapchain) == std::array<unsigned char, 4>{255, 0, 0, 128});
  CHECK(pixel(output.views[1].subImage.swapchain) == std::array<unsigned char, 4>{0, 255, 0, 64});
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
  CHECK(left[0] == 128); CHECK(std::abs(int(left[1]) - 64) <= 1); CHECK(left[3] == 128);
  CHECK(output.projection.layerFlags == 0);
  frame.scene.layerFlags = 0;
  REQUIRE(frame.prepare(output) == XR_SUCCESS);
  CHECK(pixel(output.views[0].subImage.swapchain) == std::array<unsigned char, 4>{255, 128, 0, 255});
  CHECK(pixel(chain, 0) == std::array<unsigned char, 4>{255, 128, 0, 128});
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
  CHECK(pixel(output.views[0].subImage.swapchain) == std::array<unsigned char, 4>{255, 0, 0, 128});
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
  REQUIRE(frame.prepare(output) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK(pixel(output.views[0].subImage.swapchain)[3] == 255);
  frame.layers[0] = frame.layers[1]; frame.info.layerCount = 1;
  REQUIRE(frame.prepare(output) == XR_SUCCESS); CHECK_FALSE(requested);
  CHECK(pixel(output.views[0].subImage.swapchain)[3] == 255);
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
