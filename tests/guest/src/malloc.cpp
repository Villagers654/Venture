#include "vtest.h"
#include <cstdlib>
#include <malloc.h>
#include <vector>

// Engines reserve big arenas, then allocate thousands of aligned small objects.
static bool aligned(void* p, size_t alignment) { return p && reinterpret_cast<uintptr_t>(p) % alignment == 0; }
extern "C" int vtest_main(int, char**) {
    Vtest q("malloc");
    auto p = static_cast<unsigned char*>(malloc(37)); REQUIRE(p);
    CHECK(aligned(p, alignof(max_align_t)) && malloc_usable_size(p) >= 37);
    memset(p, 0x5a, 37);
    auto grown = static_cast<unsigned char*>(realloc(p, 4096));
    if (!grown) { free(p); q.check(false, "realloc growth failed"); return q.finish(); }
    for (int i = 0; i < 37; ++i) CHECK(grown[i] == 0x5a);
    p = static_cast<unsigned char*>(realloc(grown, 16));
    if (!p) { free(grown); q.check(false, "realloc shrink failed"); return q.finish(); }
    for (int i = 0; i < 16; ++i) CHECK(p[i] == 0x5a);
    free(p);
    p = static_cast<unsigned char*>(calloc(128, 32)); REQUIRE(p);
    for (int i = 0; i < 4096; ++i) CHECK(p[i] == 0); free(p);
    free(nullptr);
    q.step("heap: allocate zero grow shrink usable size OK");
    void* allocation = nullptr;
    REQUIRE(posix_memalign(&allocation, 64, 101) == 0);
    CHECK(aligned(allocation, 64)); free(allocation);
    CHECK(posix_memalign(&allocation, 3, 20) != 0);
    allocation = memalign(4096, 8192); REQUIRE(allocation);
    CHECK(aligned(allocation, 4096)); free(allocation);
#if __ANDROID_API__ >= 28
    allocation = aligned_alloc(128, 512); REQUIRE(allocation);
    CHECK(aligned(allocation, 128)); free(allocation);
#endif
    q.step("heap: aligned allocation OK");
    constexpr size_t big_size = 256u * 1024u * 1024u;
    p = static_cast<unsigned char*>(malloc(big_size)); REQUIRE(p);
    // Touch every 4 KiB, including the end, rather than just reserving address space.
    for (size_t offset = 0; offset < big_size; offset += 4096) p[offset] = static_cast<unsigned char>(offset / 4096);
    p[big_size - 1] = 0xa5;
    for (size_t offset = 0; offset < big_size; offset += 4096) CHECK(p[offset] == static_cast<unsigned char>(offset / 4096));
    CHECK(p[big_size - 1] == 0xa5); free(p);
    q.step("heap: 256 MiB arena OK");
    std::vector<void*> blocks;
    for (size_t i = 0; i < 4096; ++i) {
        void* block = malloc(i % 127 + 1);
        if (!block) { q.check(false, "small allocation failed"); break; }
        memset(block, int(i % 251), i % 127 + 1); blocks.push_back(block);
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto b = static_cast<unsigned char*>(blocks[i]);
        CHECK(b[0] == i % 251 && b[i % 127] == i % 251);
        free(b);
    }
    CHECK(blocks.size() == 4096);
    q.step("heap: 4096 small allocations OK");
    return q.finish();
}
