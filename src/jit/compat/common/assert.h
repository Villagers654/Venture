// Assertion macros for Venture's JIT, routed to Venture's logger (MIT).
#pragma once
#include <mcl/assert.hpp>
#ifndef DEBUG_ASSERT
#ifdef NDEBUG
#define DEBUG_ASSERT(...) ((void)0)
#define DEBUG_ASSERT_MSG(...) ((void)0)
#else
#define DEBUG_ASSERT(...) ASSERT(__VA_ARGS__)
#define DEBUG_ASSERT_MSG(...) ASSERT_MSG(__VA_ARGS__)
#endif
#endif
