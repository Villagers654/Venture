#include "vtest.h"
#include <android/asset_manager.h>
#include <android/choreographer.h>
#include <android/native_window.h>
#include <dlfcn.h>

// Android glue in game plugins resolves these even before it has a Surface.
// The runtime can call the asset probe once it supplies a real manager and a
// fixture asset containing exactly the bytes "Venture asset\n".
extern "C" int vtest_ndk_asset_probe(AAssetManager* manager) {
    if (!manager) return -1;
    AAssetDir* dir = AAssetManager_openDir(manager, "");
    if (!dir) return -2;
    bool found = false;
    while (const char* name = AAssetDir_getNextFileName(dir)) if (strcmp(name, "vtest-asset.txt") == 0) found = true;
    AAssetDir_rewind(dir); AAssetDir_close(dir);
    if (!found) return -3;
    AAsset* asset = AAssetManager_open(manager, "vtest-asset.txt", AASSET_MODE_BUFFER);
    if (!asset) return -4;
    char buffer[32] = {}; const char expected[] = "Venture asset\n";
    constexpr auto length = sizeof(expected) - 1;
    bool ok = AAsset_getLength(asset) == length && AAsset_getRemainingLength(asset) == length;
    ok = ok && AAsset_getLength64(asset) == length && AAsset_getRemainingLength64(asset) == length;
    const void* data = AAsset_getBuffer(asset);
    ok = ok && data && memcmp(data, expected, length) == 0;
    ok = ok && AAsset_read(asset, buffer, length) == length && memcmp(buffer, expected, length) == 0;
    ok = ok && AAsset_seek(asset, 0, SEEK_SET) == 0;
    ok = ok && AAsset_seek64(asset, 0, SEEK_SET) == 0;
    AAsset_close(asset); return ok ? 0 : -5;
}
extern "C" int vtest_main(int, char**) {
    Vtest q("ndk_api");
    void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL); REQUIRE(android);
    for (const char* name : {"ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth",
                            "ANativeWindow_getHeight", "ANativeWindow_setBuffersGeometry", "ANativeWindow_lock",
                            "ANativeWindow_unlockAndPost"}) CHECK(dlsym(android, name) != nullptr);
    q.step("NDK: native window entry points OK");
#if __ANDROID_API__ >= 24
    CHECK(dlsym(android, "AChoreographer_getInstance") != nullptr);
    CHECK(dlsym(android, "AChoreographer_postFrameCallback") != nullptr);
#endif
    q.step("NDK: API-guarded choreographer entry points OK");
    CHECK(vtest_ndk_asset_probe(nullptr) == -1);
    CHECK(dlclose(android) == 0);
    q.step("NDK: asset manager probe linked OK"); return q.finish();
}
