#include <cstdio>
#include <dlfcn.h>

// Device/guest PIE launcher: vtest_runner <library> [writable-directory].
int main(int argc, char** argv) {
    if (argc < 2) { std::fputs("usage: vtest_runner <library> [test-dir]\n", stderr); return 2; }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fputs("vtest_runner: dlopen failed\n", stderr); return 2; }
    auto entry = reinterpret_cast<int (*)(int, char**)>(dlsym(library, "vtest_main"));
    if (!entry) { dlclose(library); std::fputs("vtest_runner: missing vtest_main\n", stderr); return 2; }
    int result = entry(argc - 1, argv + 1);
    if (dlclose(library) != 0) return 2;
    return result;
}
