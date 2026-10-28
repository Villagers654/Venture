#include "vtest.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <sys/auxv.h>
#include <sys/system_properties.h>
#include <string>

struct Seen { bool test = false, peer = false; };
static int inspect(dl_phdr_info* info, size_t, void* opaque) {
    auto& seen = *static_cast<Seen*>(opaque);
    const char* name = strrchr(info->dlpi_name, '/'); name = name ? name + 1 : info->dlpi_name;
    if (strcmp(name, "libvtest_dl.so") == 0) seen.test = true;
    if (strcmp(name, "libvtest_dl_peer.so") == 0) seen.peer = true;
    return 0;
}
struct Nested { int calls = 0; bool stable = true; };
static int nested(dl_phdr_info* info, size_t size, void* opaque) {
    auto& state = *static_cast<Nested*>(opaque);
    ++state.calls;
    const auto before = *info;
    const std::string name(info->dlpi_name);
    Seen seen;
    state.stable = dl_iterate_phdr(inspect, &seen) == 0 && seen.test && seen.peer &&
        size == sizeof(*info) && info->dlpi_addr == before.dlpi_addr &&
        info->dlpi_phdr == before.dlpi_phdr && info->dlpi_phnum == before.dlpi_phnum &&
        name == info->dlpi_name;
    return 73;
}
// Plugin discovery needs bionic lookup scope, error lifetime and loader metadata.
extern "C" int vtest_main(int, char**) {
    Vtest q("dl"); dlerror();
    CHECK(dlopen("libvtest_intentionally_missing.so", RTLD_NOW) == nullptr);
    CHECK(dlerror() != nullptr && dlerror() == nullptr);
    void* library = dlopen("libvtest_dl_peer.so", RTLD_LAZY | RTLD_GLOBAL); REQUIRE(library);
    auto add = reinterpret_cast<int (*)(int)>(dlsym(library, "vtest_dl_add"));
    auto data = static_cast<int*>(dlsym(library, "vtest_dl_value"));
    REQUIRE(add && data); CHECK(add(1) == 42 && *data == 41);
    CHECK(dlsym(RTLD_DEFAULT, "vtest_dl_add") == reinterpret_cast<void*>(add));
    CHECK(dlsym(library, "vtest_dl_missing") == nullptr && dlerror() != nullptr && dlerror() == nullptr);
    void* second = dlopen("libvtest_dl_peer.so", RTLD_NOW | RTLD_LOCAL); REQUIRE(second);
    CHECK(dlsym(second, "vtest_dl_add") == reinterpret_cast<void*>(add));
    CHECK(dlclose(second) == 0 && add(1) == 42);
    q.step("dl: NOW LAZY GLOBAL symbols refcounts errors OK");
    Dl_info info{}; CHECK(dladdr(reinterpret_cast<void*>(add), &info) != 0);
    CHECK(info.dli_fname && strstr(info.dli_fname, "libvtest_dl_peer.so"));
    CHECK(info.dli_sname && strcmp(info.dli_sname, "vtest_dl_add") == 0);
    Seen seen; CHECK(dl_iterate_phdr(inspect, &seen) == 0 && seen.test && seen.peer);
    Nested state;
    CHECK(dl_iterate_phdr(nested, &state) == 73 && state.calls == 1 && state.stable);
    seen = {};
    CHECK(dl_iterate_phdr(inspect, &seen) == 0 && seen.test && seen.peer);
    // Sort by choosing a fixed order, never print runtime/system library paths.
    q.step("loaded: libvtest_dl.so libvtest_dl_peer.so");
    unsigned long page = getauxval(AT_PAGESZ); CHECK(page >= 4096 && (page & (page - 1)) == 0);
    (void)getauxval(AT_HWCAP); // Zero is valid if the runtime advertises no extras.
    char sdk[PROP_VALUE_MAX] = {}; int length = __system_property_get("ro.build.version.sdk", sdk);
    char* end = nullptr; long version = strtol(sdk, &end, 10);
    CHECK(length > 0 && end && *end == 0 && version >= __ANDROID_API__);
    q.step("dl: auxv and SDK property OK");
    CHECK(dlclose(library) == 0);
    return q.finish();
}
