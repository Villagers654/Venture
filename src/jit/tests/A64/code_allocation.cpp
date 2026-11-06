// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/interface/A64/a64.h"
#include "tests/A64/testenv.h"
#include "tests/native/testenv.h"

using namespace Dynarmic;

// Venture: a commit of code pages the OS refuses (the Windows commit limit)
// clears the translated code and compiles again instead of stopping the thread.
TEST_CASE("a refused code-cache commit clears the cache and the guest keeps running", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    std::size_t reports = 0, requested = 0, cache_size = 0;
    conf.code_allocation_failed = [&](std::size_t size, std::size_t, std::size_t total) {
        ++reports;
        requested = size;
        cache_size = total;
    };
    A64::Jit jit{conf};

    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    Backend::X64::BlockOfCode::FailCommitsForTesting(1);
    jit.SetPC(0);
    env.ticks_left = 2;
    CheckedRun([&]() { jit.Run(); });
    Backend::X64::BlockOfCode::FailCommitsForTesting(0);
    REQUIRE(jit.GetRegister(0) == 42);
    REQUIRE(reports == 1);
    REQUIRE(requested > 0);
    REQUIRE(cache_size == conf.code_cache_size);

    // The cleared cache keeps working for later code.
    env.code_mem[0] = 0xd28008a0;  // MOV X0, 69
    jit.ClearCache();
    jit.SetPC(0);
    env.ticks_left = 2;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}
