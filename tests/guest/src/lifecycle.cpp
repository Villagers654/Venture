#include "vtest.h"
#include <dlfcn.h>
static int destruction = 0;
static void observe(int id) { destruction = destruction * 10 + id; }
extern "C" int vtest_main(int, char**) {
    Vtest q("lifecycle"); destruction = 0;
    void* lib = dlopen("libvtest_lifecycle_peer.so", RTLD_NOW | RTLD_LOCAL); REQUIRE(lib);
    auto attach = reinterpret_cast<int (*)(void (*)(int))>(dlsym(lib, "vtest_lifecycle_attach"));
    if (!CHECK(attach)) { dlclose(lib); return q.finish(); }
    CHECK(attach(observe) == 12);
    q.step("lifecycle: constructors in declaration order OK");
    CHECK(dlclose(lib) == 0 && destruction == 321);
    q.step("lifecycle: cxa_atexit and destructors in reverse order OK");
    return q.finish();
}
