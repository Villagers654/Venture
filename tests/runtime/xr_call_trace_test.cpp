#include "core/xr_call_trace.h"
#include "hle/openxr/openxr.h"
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("XR call ring preserves order and bounds concurrent producers") {
  venture::xr_call_trace::Ring<32> ring;
  std::array<std::thread,4> producers;
  for (auto& thread:producers) thread=std::thread([&] {
    for (int i=0;i<100;++i) ring.append({.call=std::uint64_t(i)});
  });
  for (auto& thread:producers) thread.join();
  const auto records=ring.snapshot();
  REQUIRE(records.size()==32);
  CHECK(records.front().sequence==369);
  CHECK(records.back().sequence==400);
  for (std::size_t i=1;i<records.size();++i) CHECK(records[i].sequence==records[i-1].sequence+1);
}
TEST_CASE("XR call observer retains six actual API validation results") {
  using namespace venture;
  using namespace venture::hle::openxr;
  const bool tracing=xr_call_trace::enabled();
  auto* results = api_result_trace::enabled() ? api_result_trace::thread_ring() : nullptr;
  const auto result_start = results ? results->total() : 0;
  const auto before=xr_call_trace::ring().snapshot();
  CHECK(guest::xrWaitFrame(XR_NULL_HANDLE,nullptr,nullptr)==XR_ERROR_HANDLE_INVALID);
  CHECK(guest::xrBeginFrame(XR_NULL_HANDLE,nullptr)==XR_ERROR_HANDLE_INVALID);
  CHECK(guest::xrEndFrame(XR_NULL_HANDLE,nullptr)==XR_ERROR_HANDLE_INVALID);
  CHECK(guest::xrPollEvent(XR_NULL_HANDLE,nullptr)==XR_ERROR_HANDLE_INVALID);
  CHECK(guest::xrLocateViews(XR_NULL_HANDLE,nullptr,nullptr,0,nullptr,nullptr)==XR_ERROR_HANDLE_INVALID);
  CHECK(guest::xrSyncActions(XR_NULL_HANDLE,nullptr)==XR_ERROR_HANDLE_INVALID);
  if (results) {
    REQUIRE(results->total() == result_start + 6);
    for (auto i = result_start + 1; i <= result_start + 6; ++i) {
      api_result_trace::Record r;
      REQUIRE(results->read(i, r));
      CHECK(r.phase == api_result_trace::Phase::xr);
      CHECK(r.state == api_result_trace::State::returned);
      CHECK(r.result == XR_ERROR_HANDLE_INVALID);
    }
  }
  const auto records=xr_call_trace::ring().snapshot();
  REQUIRE(records.size()==before.size()+(tracing?12:0));
  for (std::size_t i=before.size();i<records.size();i+=2) {
    const auto& begin=records[i]; const auto& end=records[i+1];
    CHECK_FALSE(begin.returned); CHECK(end.returned);
    CHECK(begin.call==end.call); CHECK(begin.thread==end.thread);
    CHECK(end.ns>=begin.ns); CHECK(end.result==XR_ERROR_HANDLE_INVALID);
    CHECK(end.fields==0); // Invalid pointers were never read to populate diagnostics.
  }
}
