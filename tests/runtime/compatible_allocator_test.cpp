#include "vulkan_callbacks_test_support.h"
#include "hle/vulkan/callbacks.h"
#include "hle/vulkan/terminal_allocator.h"
#include "core/os_quit.h"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>

// Each case leaves irreversible process state: one process per case (tests.cmake).
TEST_CASE("Compatible allocator records preserve cross-record ownership" * doctest::skip()) {
 using namespace venture;
 std::array<unsigned char,64> original{},moved{};
 unsigned calls=0;GuestAddr last_user=0;
 test::set_vulkan_callback([&](GuestAddr fn,const GuestCallArgs& args){
  ++calls;last_user=args.integers[0];
  if(fn==1)return GuestCallResult{h2g(original.data())};
  if(fn==2){std::memcpy(moved.data(),g2h(args.integers[1]),32);return GuestCallResult{h2g(moved.data())};}
  return GuestCallResult{};
 });
 VkAllocationCallbacks a{reinterpret_cast<void*>(0x111),
  reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(1,4)),
  reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(2,5)),
  reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(3,2)),nullptr,nullptr};
 auto b=a;b.pUserData=reinterpret_cast<void*>(0x222);
 auto* ha=vulkan::translate_allocator(&a);auto* hb=vulkan::translate_allocator(&b);
 REQUIRE(ha!=hb);auto copy=a;CHECK(vulkan::translate_allocator(&copy)==ha);
 auto* p=static_cast<unsigned char*>(ha->pfnAllocation(ha->pUserData,64,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 REQUIRE(p==original.data());CHECK(last_user==0x111);p[0]=91;
 auto* q=static_cast<unsigned char*>(hb->pfnReallocation(hb->pUserData,p,64,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 REQUIRE(q==moved.data());CHECK(q[0]==91);CHECK(last_user==0x222);
 ha->pfnFree(ha->pUserData,q);CHECK(last_user==0x111);
 p=static_cast<unsigned char*>(ha->pfnAllocation(ha->pUserData,64,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 p[0]=73;const auto before=calls;os_quit::stopping.store(true);
 REQUIRE(vulkan::begin_terminal_allocator_bridge());
 q=static_cast<unsigned char*>(hb->pfnReallocation(hb->pUserData,p,96,64,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 REQUIRE(q);CHECK(q[0]==73);CHECK(original[0]==73);
 ha->pfnFree(ha->pUserData,q);CHECK(calls==before);CHECK(vulkan::terminal_allocator_bridge_healthy());
 // No guest execution or resume after the irreversible terminal boundary.
 test::set_vulkan_callback({});
 std::puts("COMPATIBLE_ALLOCATORS distinct_userdata_normal_realloc_free_and_terminal_cross_record_copy PASS");
}

TEST_CASE("Throwing allocator callback cannot report clean terminal ownership" * doctest::skip()) {
 using namespace venture;
 std::array<unsigned char,64> memory{};
 test::set_vulkan_callback([&](GuestAddr fn,const GuestCallArgs&){
  if(fn==1)return GuestCallResult{h2g(memory.data())};
  throw std::runtime_error("simulated failure after an unknown ownership side effect");
 });
 VkAllocationCallbacks a{nullptr,
  reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(1,4)),
  reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(2,5)),
  reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(3,2)),nullptr,nullptr};
 auto* host=vulkan::translate_allocator(&a);
 auto* p=host->pfnAllocation(host->pUserData,64,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
 REQUIRE(p==memory.data());
 host->pfnFree(host->pUserData,p);
 os_quit::stopping.store(true);
 CHECK_FALSE(vulkan::begin_terminal_allocator_bridge());
 CHECK_FALSE(vulkan::terminal_allocator_bridge_healthy());
 test::set_vulkan_callback({});
}

TEST_CASE("Stopped guest allocator serves native calls before the terminal bridge" * doctest::skip()) {
 using namespace venture;
 std::array<unsigned char,64> guest{};
 unsigned calls=0;
 test::set_vulkan_callback([&](GuestAddr fn,const GuestCallArgs&){
  ++calls;
  return fn==1?GuestCallResult{h2g(guest.data())}:GuestCallResult{};
 });
 VkAllocationCallbacks a{nullptr,
  reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(1,4)),
  reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(2,5)),
  reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(3,2)),nullptr,nullptr};
 auto* host=vulkan::translate_allocator(&a);
 auto* g=static_cast<unsigned char*>(host->pfnAllocation(host->pUserData,64,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 REQUIRE(g==guest.data());g[0]=42;
 const auto before=calls;
 // Guest execution stopped, terminal bridge not yet active (the OS quit drain,
 // JVM stop and save flush still run): a native submit must still allocate.
 os_quit::stopping.store(true);
 auto* p=static_cast<unsigned char*>(host->pfnAllocation(host->pUserData,128,64,VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
 REQUIRE(p);CHECK(reinterpret_cast<std::uintptr_t>(p)%64==0);p[127]=7;
 auto* q=static_cast<unsigned char*>(host->pfnReallocation(host->pUserData,g,96,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
 REQUIRE(q);CHECK(q[0]==42);CHECK(guest[0]==42); // guest memory stays the stopped process's
 host->pfnFree(host->pUserData,p);
 CHECK(calls==before);
 REQUIRE(vulkan::begin_terminal_allocator_bridge());
 host->pfnFree(host->pUserData,q);
 CHECK(calls==before);CHECK(vulkan::terminal_allocator_bridge_healthy());
 test::set_vulkan_callback({});
 std::puts("STOPPED_ALLOCATOR host_alloc_realloc_free_before_bridge PASS");
}
