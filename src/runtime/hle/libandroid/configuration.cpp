#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "android/framework_bridge.h"
#include "core/process.h"
#include <array>
#include <cstring>
namespace venture::hle::android_hle {
namespace {
// Values from AOSP configuration.h; Quest-compatible resource selection policy.
// uiModeType=VR_HEADSET, landscape, no touchscreen/keyboard/navigation.
struct Configuration {
  std::mutex mutex;
  std::array<int, 21> values{};
  std::array<char, 2> language{}, country{};
};
android::Handles<Configuration> configs{"AConfiguration"};
Configuration* create() { return configs.create(); }
void destroy(Configuration* p) { configs.erase(p); }
void from_assets(Configuration* p, GuestAddr manager) {
  auto c = configs.get(p);
  if (!c || manager != android::package_asset_manager()) return;
  std::lock_guard lock(c->mutex);
  c->values = {0, 0, 2, 1, 320, 1, 1, 2, 2, 32, 3, 2, 7, 1, 960, 540, 540, 1, 1, 1, 1};
  c->values[9] = process_config().sdk_level;
  c->language = {'e', 'n'};
  c->country = {'U', 'S'};
}
template <int I>
int get(Configuration* p) {
  auto c = configs.get(p);
  if (!c) return 0;
  std::lock_guard lock(c->mutex);
  return c->values[I];
}
template <int I>
void set(Configuration* p, int value) {
  if (auto c = configs.get(p)) {
    std::lock_guard lock(c->mutex);
    c->values[I] = value;
  }
}
template <bool Country>
void get_locale(Configuration* p, char* out) {
  auto c = configs.get(p);
  if (!c || !out) return;
  std::lock_guard lock(c->mutex);
  auto& v = Country ? c->country : c->language;
  std::memcpy(out, v.data(), 2);
}
template <bool Country>
void set_locale(Configuration* p, const char* in) {
  auto c = configs.get(p);
  if (!c || !in) return;
  std::lock_guard lock(c->mutex);
  auto& v = Country ? c->country : c->language;
  std::memcpy(v.data(), in, 2);
}
void copy(Configuration* dest, Configuration* src) {
  auto a = configs.get(dest), b = configs.get(src);
  if (!a || !b || a == b) return;
  std::scoped_lock lock(a->mutex, b->mutex);
  a->values = b->values;
  a->language = b->language;
  a->country = b->country;
}
int diff(Configuration* left, Configuration* right) {
  auto a = configs.get(left), b = configs.get(right);
  if (!a || !b || a == b) return 0;
  std::scoped_lock lock(a->mutex, b->mutex);
  constexpr int masks[] = {1,    2,    128,  8,   256, 16,   64,    32,    32,    1024, 2048,
                           2048, 4096, 4096, 512, 512, 8192, 16384, 32768, 65536, 65536};
  int result = (a->language != b->language || a->country != b->country) ? 4 : 0;
  for (std::size_t i = 0; i < a->values.size(); ++i)
    if (a->values[i] != b->values[i]) result |= masks[i];
  return result;
}
}  // namespace
void register_configuration(HleLibrary& lib) {
  add<&create>(lib, "AConfiguration_new");
  add<&destroy>(lib, "AConfiguration_delete");
  add<&from_assets>(lib, "AConfiguration_fromAssetManager");
  add<&copy>(lib, "AConfiguration_copy");
  add<&diff>(lib, "AConfiguration_diff");
  add<&get_locale<false>>(lib, "AConfiguration_getLanguage");
  add<&get_locale<true>>(lib, "AConfiguration_getCountry");
  add<&set_locale<false>>(lib, "AConfiguration_setLanguage");
  add<&set_locale<true>>(lib, "AConfiguration_setCountry");
  add<&get<0>>(lib, "AConfiguration_getMcc");
  add<&set<0>>(lib, "AConfiguration_setMcc");
  add<&get<1>>(lib, "AConfiguration_getMnc");
  add<&set<1>>(lib, "AConfiguration_setMnc");
  add<&get<2>>(lib, "AConfiguration_getOrientation");
  add<&set<2>>(lib, "AConfiguration_setOrientation");
  add<&get<3>>(lib, "AConfiguration_getTouchscreen");
  add<&set<3>>(lib, "AConfiguration_setTouchscreen");
  add<&get<4>>(lib, "AConfiguration_getDensity");
  add<&set<4>>(lib, "AConfiguration_setDensity");
  add<&get<5>>(lib, "AConfiguration_getKeyboard");
  add<&set<5>>(lib, "AConfiguration_setKeyboard");
  add<&get<6>>(lib, "AConfiguration_getNavigation");
  add<&set<6>>(lib, "AConfiguration_setNavigation");
  add<&get<7>>(lib, "AConfiguration_getKeysHidden");
  add<&set<7>>(lib, "AConfiguration_setKeysHidden");
  add<&get<8>>(lib, "AConfiguration_getNavHidden");
  add<&set<8>>(lib, "AConfiguration_setNavHidden");
  add<&get<9>>(lib, "AConfiguration_getSdkVersion");
  add<&set<9>>(lib, "AConfiguration_setSdkVersion");
  add<&get<10>>(lib, "AConfiguration_getScreenSize");
  add<&set<10>>(lib, "AConfiguration_setScreenSize");
  add<&get<11>>(lib, "AConfiguration_getScreenLong");
  add<&set<11>>(lib, "AConfiguration_setScreenLong");
  add<&get<12>>(lib, "AConfiguration_getUiModeType");
  add<&set<12>>(lib, "AConfiguration_setUiModeType");
  add<&get<13>>(lib, "AConfiguration_getUiModeNight");
  add<&set<13>>(lib, "AConfiguration_setUiModeNight");
  add<&get<14>>(lib, "AConfiguration_getScreenWidthDp");
  add<&set<14>>(lib, "AConfiguration_setScreenWidthDp");
  add<&get<15>>(lib, "AConfiguration_getScreenHeightDp");
  add<&set<15>>(lib, "AConfiguration_setScreenHeightDp");
  add<&get<16>>(lib, "AConfiguration_getSmallestScreenWidthDp");
  add<&set<16>>(lib, "AConfiguration_setSmallestScreenWidthDp");
  add<&get<17>>(lib, "AConfiguration_getLayoutDirection");
  add<&set<17>>(lib, "AConfiguration_setLayoutDirection");
  add<&get<18>>(lib, "AConfiguration_getScreenRound");
  add<&set<18>>(lib, "AConfiguration_setScreenRound");
  add<&get<19>>(lib, "AConfiguration_getWideColorGamut");
  add<&set<19>>(lib, "AConfiguration_setWideColorGamut");
  add<&get<20>>(lib, "AConfiguration_getHdr");
  add<&set<20>>(lib, "AConfiguration_setHdr");
}
}  // namespace venture::hle::android_hle
