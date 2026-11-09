#include "xr/stats_overlay.h"

#ifndef _WIN32
namespace venture::xr::stats_overlay {
bool active() { return false; }
const XrCompositionLayerBaseHeader* layer(XrSession, const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
                                          XrTime) {
  return nullptr;
}
void note_prepare(double) {}
void note_render_size(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
}  // namespace venture::xr::stats_overlay
#else
#include "common/environment.h"
#include "common/log.h"
#include "xr/queue.h"
#include "xr/session.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>


namespace venture::xr::stats_overlay {
namespace {
constexpr std::uint32_t kWidth = 1160, kHeight = 100;
// 0xAARRGGBB.
constexpr std::uint32_t kLabel = 0xe8ffffffu, kGrid = 0x70627482u, kGreen = 0xff58ee8au, kAmber = 0xffffbf3fu,
                        kRed = 0xffff5757u, kCyan = 0xff58d8ffu, kViolet = 0xffc07affu;

std::uint32_t rate_color(double value, double warning, double bad) {
  if (!std::isfinite(value) || value <= bad) return kRed;
  return value < warning ? kAmber : kGreen;
}
COLORREF color_ref(std::uint32_t argb) { return RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff); }
void put(std::vector<std::uint32_t>& p, int x, int y, std::uint32_t c) {
  if (x >= 0 && y >= 0 && x < int(kWidth) && y < int(kHeight)) p[std::size_t(y) * kWidth + x] = c;
}
void line(std::vector<std::uint32_t>& p, int x0, int y0, int x1, int y1, std::uint32_t c) {
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  for (int e = dx + dy;;) {
    put(p, x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * e;
    if (e2 >= dy) e += dy, x0 += sx;
    if (e2 <= dx) e += dx, y0 += sy;
  }
}
std::wstring number(double value, int decimals, const wchar_t* suffix = L"") {
  if (!std::isfinite(value) || value < 0) return L"\x2014";
  wchar_t text[64]{};
  std::swprintf(text, std::size(text), decimals == 0 ? L"%.0f" : L"%.1f", value);
  return std::wstring(text) + suffix;
}
// "Over" blend of a straight-alpha color onto a straight-alpha pixel.
std::uint32_t over(std::uint32_t dst, std::uint32_t src) {
  const float sa = float(src >> 24) / 255.f, da = float(dst >> 24) / 255.f;
  const float oa = sa + da * (1 - sa);
  if (oa <= 0) return 0;
  std::uint32_t out = std::uint32_t(std::lround(oa * 255)) << 24;
  for (int shift : {16, 8, 0}) {
    const float s = float((src >> shift) & 0xff), d = float((dst >> shift) & 0xff);
    out |= std::uint32_t(std::clamp(std::lround((s * sa + d * da * (1 - sa)) / oa), 0l, 255l)) << shift;
  }
  return out;
}

// The panel: a full-width dark band with blue rules at its
// top and bottom, thin grey separators between the sections, and a blue
// gradient "V" in the middle, on a transparent background.
constexpr int kBandTop = 20, kBandBottom = 80;
constexpr int kSeparators[] = {130, 239, 374, 519, 638, 796, 944, 1046};
std::uint32_t lerp_color(std::uint32_t a, std::uint32_t b, float t) {
  std::uint32_t out = 0xff000000u;
  for (int shift : {16, 8, 0}) {
    const float x = float((a >> shift) & 0xff), y = float((b >> shift) & 0xff);
    out |= std::uint32_t(std::lround(x + (y - x) * std::clamp(t, 0.f, 1.f))) << shift;
  }
  return out;
}
// A convex quad filled with a vertical gradient, 4x4 supersampled.
void fill_quad(std::vector<std::uint32_t>& p, const std::array<std::array<float, 2>, 4>& q, std::uint32_t top,
               std::uint32_t bottom) {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (const auto& v : q) x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), y0 = std::min(y0, v[1]), y1 = std::max(y1, v[1]);
  const auto inside = [&](float x, float y) {
    for (int i = 0; i < 4; ++i) {
      const auto& a = q[i];
      const auto& b = q[(i + 1) % 4];
      if ((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) < 0) return false;
    }
    return true;
  };
  for (int y = int(std::floor(y0)); y <= int(std::ceil(y1)); ++y)
    for (int x = int(std::floor(x0)); x <= int(std::ceil(x1)); ++x) {
      int hits = 0;
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) hits += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
      if (!hits || x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight)) continue;
      const auto color = lerp_color(top, bottom, (y + 0.5f - y0) / std::max(1.f, y1 - y0));
      auto& dst = p[std::size_t(y) * kWidth + x];
      dst = over(dst, (std::uint32_t(hits * 255 / 16) << 24) | (color & 0xffffff));
    }
}
std::vector<std::uint32_t> background() {
  std::vector<std::uint32_t> p(std::size_t(kWidth) * kHeight, 0);
  for (int y = kBandTop; y < kBandBottom; ++y)
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool rule = y < kBandTop + 2 || y >= kBandBottom - 2;
      p[std::size_t(y) * kWidth + x] = rule ? 0xff4a9effu : 0xf2333333u;
    }
  for (int x : kSeparators)
    for (int y = kBandTop + 10; y < kBandBottom - 10; ++y) p[std::size_t(y) * kWidth + x] = 0xff5c5c5cu;
  // The "V": a wide cyan arm and a blue arm meeting at the bottom.
  constexpr float cx = 578, top = 31, bottom = 69;
  fill_quad(p, {{{cx - 31, top}, {cx - 15, top}, {cx + 4, bottom}, {cx - 7, bottom}}}, 0xff27d6ffu, 0xff1f8fffu);
  fill_quad(p, {{{cx + 17, top}, {cx + 32, top}, {cx + 5, bottom}, {cx - 5, bottom}}}, 0xff1e6bffu, 0xff2a9dffu);
  return p;
}

// Rates and timings, written by the render thread and read by the painter.
struct Rates {
  std::mutex mutex;
  double prepare_ms = -1, game_fps = -1, headset_hz = -1;
  std::uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
} rates;

// Shown or hidden: the launch setting, then the event a tool sets or resets
// during play. A watcher thread follows the event, so the render thread only
// reads this flag (-1 until the first call).
std::atomic<int> shown{-1};
void start_watcher() {
  static std::once_flag once;
  std::call_once(once, [] {
    const char* initial = venture::environment::getenv("VENTURE_STATS_OVERLAY");
    const std::string_view start = initial ? initial : "";
    const auto name = L"Local\\VentureStatsOverlay-" + std::to_wstring(GetCurrentProcessId());
    const HANDLE event = CreateEventW(nullptr, TRUE, start == "on" || start == "1", name.c_str());
    shown.store(event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0 ? 1 : 0);
    if (!event) return;
    std::thread([event] {
      for (;;) {
        Sleep(250);
        shown.store(WaitForSingleObject(event, 0) == WAIT_OBJECT_0 ? 1 : 0, std::memory_order_relaxed);
      }
    }).detach();
  });
}

// PC load, sampled off the render thread twice a second:
// CPU from GetSystemTimes, RAM from GlobalMemoryStatusEx, GPU and VRAM from
// NVML on NVIDIA, else the busiest PDH GPU engine of the adapter. The same
// thread paints the panel (GDI, with fonts and a DIB made once) for the render
// thread to upload.
struct Load {
  double cpu = -1, gpu = -1, ram_percent = -1, ram_used = -1, ram_total = -1, vram_percent = -1, vram_used = -1,
         vram_total = -1;
};
class Sampler {
 public:
  Sampler(std::string gpu_name, std::wstring luid_tag)
      : name_(normalized(gpu_name)), luid_(std::move(luid_tag)), background_(background()),
        worker_([this] { run(); }) {}
  ~Sampler() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
  }
  // The newest painted panel, when it is newer than `seen`.
  bool take_panel(std::vector<std::uint32_t>& out, std::uint64_t& seen) {
    std::lock_guard lock(mutex_);
    if (!panel_sequence_ || panel_sequence_ == seen) return false;
    out = panel_;
    seen = panel_sequence_;
    return true;
  }

 private:
  static std::string normalized(std::string value) {
    std::erase_if(value, [](unsigned char c) { return !std::isalnum(c); });
    for (auto& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
  }
  static double smooth(double previous, double current) {
    if (!std::isfinite(current) || current < 0) return -1;
    if (!std::isfinite(previous) || previous < 0) return current;
    return previous * 0.75 + current * 0.25;
  }
  double pdh_gpu(PDH_HCOUNTER counter) const {
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_STATUS(PDH_MORE_DATA) || !bytes)
      return -1;
    std::vector<std::uint8_t> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    std::unordered_map<std::wstring, double> all, adapter;
    for (DWORD i = 0; i < count; ++i) {
      const auto& v = items[i].FmtValue;
      if ((v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) || !(v.doubleValue >= 0)) continue;
      std::wstring name = items[i].szName ? items[i].szName : L"";
      for (auto& c : name) c = wchar_t(std::towlower(c));
      const auto engine = name.find(L"engtype_");
      const auto key = engine == std::wstring::npos ? name : name.substr(engine);
      all[key] += v.doubleValue;
      if (!luid_.empty() && name.find(luid_) != std::wstring::npos) adapter[key] += v.doubleValue;
    }
    double busiest = -1;
    for (const auto& [key, value] : adapter.empty() ? all : adapter) busiest = std::max(busiest, value);
    return busiest < 0 ? -1 : std::clamp(busiest, 0.0, 100.0);
  }
  void run() {
    using Device = void*;
    struct Utilization { unsigned gpu, memory; };
    struct Memory { unsigned long long total, free, used; };
    using Init = int(__cdecl*)();
    using Count = int(__cdecl*)(unsigned*);
    using Handle = int(__cdecl*)(unsigned, Device*);
    using Name = int(__cdecl*)(Device, char*, unsigned);
    using Util = int(__cdecl*)(Device, Utilization*);
    using Mem = int(__cdecl*)(Device, Memory*);
    HMODULE nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Device device{};
    Util utilization{};
    Mem memory{};
    using Shutdown = int(__cdecl*)();
    Shutdown shutdown{};
    if (nvml) {
      const auto proc = [&](const char* a, const char* b = nullptr) {
        auto f = GetProcAddress(nvml, a);
        return f || !b ? f : GetProcAddress(nvml, b);
      };
      auto init = reinterpret_cast<Init>(proc("nvmlInit_v2"));
      auto count = reinterpret_cast<Count>(proc("nvmlDeviceGetCount_v2", "nvmlDeviceGetCount"));
      auto handle = reinterpret_cast<Handle>(proc("nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex"));
      auto name = reinterpret_cast<Name>(proc("nvmlDeviceGetName"));
      utilization = reinterpret_cast<Util>(proc("nvmlDeviceGetUtilizationRates"));
      memory = reinterpret_cast<Mem>(proc("nvmlDeviceGetMemoryInfo"));
      shutdown = reinterpret_cast<Shutdown>(proc("nvmlShutdown"));
      unsigned n = 0, matches = 0;
      if (init && count && handle && name && utilization && memory && init() == 0) {
        if (count(&n) == 0)
          for (unsigned i = 0; i < n; ++i) {
            Device candidate{};
            char text[128]{};
            if (handle(i, &candidate) == 0 && candidate && name(candidate, text, sizeof(text)) == 0 &&
                normalized(text) == name_)
              device = candidate, ++matches;
          }
        if (matches != 1) device = nullptr;
      } else {
        shutdown = nullptr;
      }
    }
    log::info("overlay", "PC load: CPU/RAM from Windows; GPU/VRAM from {}", device ? "NVML" : "PDH (no VRAM)");
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    if (!device && PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
      if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        query = nullptr;
      } else {
        PdhCollectQueryData(query);
      }
    }
    const auto value = [](const FILETIME& t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    std::uint64_t idle0 = 0, kernel0 = 0, user0 = 0;
    bool baseline = false;
    while (running_) {
      Load now;
      FILETIME idle{}, kernel{}, user{};
      if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto i = value(idle), k = value(kernel), u = value(user);
        if (baseline && (k - kernel0) + (u - user0))
          now.cpu = std::clamp(100.0 * (1 - double(i - idle0) / double((k - kernel0) + (u - user0))), 0.0, 100.0);
        idle0 = i, kernel0 = k, user0 = u, baseline = true;
      }
      MEMORYSTATUSEX ram{sizeof(ram)};
      constexpr double gb = 1024.0 * 1024.0 * 1024.0;
      if (GlobalMemoryStatusEx(&ram)) {
        now.ram_percent = ram.dwMemoryLoad;
        now.ram_total = double(ram.ullTotalPhys) / gb;
        now.ram_used = double(ram.ullTotalPhys - ram.ullAvailPhys) / gb;
      }
      if (device) {
        Utilization u{};
        Memory m{};
        if (utilization(device, &u) == 0) now.gpu = u.gpu;
        if (memory(device, &m) == 0 && m.total) {
          now.vram_used = double(m.used) / gb;
          now.vram_total = double(m.total) / gb;
          now.vram_percent = 100.0 * double(m.used) / double(m.total);
        }
      } else if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
        now.gpu = pdh_gpu(counter);
      }
      {
        std::lock_guard lock(mutex_);
        load_ = {smooth(load_.cpu, now.cpu),           smooth(load_.gpu, now.gpu),
                 smooth(load_.ram_percent, now.ram_percent), smooth(load_.ram_used, now.ram_used),
                 smooth(load_.ram_total, now.ram_total), smooth(load_.vram_percent, now.vram_percent),
                 smooth(load_.vram_used, now.vram_used), smooth(load_.vram_total, now.vram_total)};
      }
      Load current;
      {
        std::lock_guard lock(mutex_);
        current = load_;
      }
      paint(current);
      for (int i = 0; i < 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    release_gdi();
    if (query) PdhCloseQuery(query);
    if (shutdown) shutdown();
    if (nvml) FreeLibrary(nvml);
  }
  // GDI objects for painting, made once on this thread.
  bool prepare_gdi() {
    if (dc_) return bits_ != nullptr;
    dc_ = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(kWidth), -LONG(kHeight), 1, 32, BI_RGB};
    bitmap_ = dc_ ? CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
    if (!bitmap_ || !bits_) return false;
    SelectObject(dc_, bitmap_);
    SetBkMode(dc_, TRANSPARENT);
    const auto font = [](int size, int weight) {
      return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    label_font_ = font(11, FW_SEMIBOLD);
    value_font_ = font(20, FW_BOLD);
    compact_font_ = font(12, FW_BOLD);
    return true;
  }
  void release_gdi() {
    for (HFONT f : {label_font_, value_font_, compact_font_})
      if (f) DeleteObject(f);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
  }
  void paint(const Load& load) {
    double headset_hz = -1, game_fps = -1, prepare = -1;
    std::uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    {
      std::lock_guard lock(rates.mutex);
      headset_hz = rates.headset_hz, game_fps = rates.game_fps, prepare = rates.prepare_ms;
      rw = rates.render_width, rh = rates.render_height;
      ow = rates.output_width, oh = rates.output_height;
    }
    auto p = background_;
    // Rate graph: headset (green) and game (violet), 0-120 Hz.
    history_[0][cursor_] = float(std::clamp(headset_hz, 0.0, 120.0));
    history_[1][cursor_] = float(std::clamp(game_fps, 0.0, 120.0));
    cursor_ = (cursor_ + 1) % history_[0].size();
    filled_ = std::min(filled_ + 1, history_[0].size());
    constexpr int gl = 398, gr = 500, gt = 32, gb = 68;
    line(p, gl, gb, gr, gb, kGrid);
    line(p, gl, gb - int(90.0 / 120.0 * (gb - gt)), gr, gb - int(90.0 / 120.0 * (gb - gt)), 0x807b6632u);
    const auto n = history_[0].size();
    for (std::size_t series = 0; series < 2; ++series)
      for (std::size_t i = 1; i < filled_; ++i) {
        const auto a = history_[series][(cursor_ + n - filled_ + i - 1) % n], b = history_[series][(cursor_ + n - filled_ + i) % n];
        line(p, gl + int((i - 1) * (gr - gl) / (n - 1)), gb - int(std::clamp(a / 120.f, 0.f, 1.f) * (gb - gt)),
             gl + int(i * (gr - gl) / (n - 1)), gb - int(std::clamp(b / 120.f, 0.f, 1.f) * (gb - gt)),
             series ? kViolet : kGreen);
      }
    if (prepare_gdi()) {
      std::memcpy(bits_, p.data(), p.size() * 4);
      const auto text = [&](HFONT f, const std::wstring& s, int l, int t, int r, int b, std::uint32_t c) {
        SelectObject(dc_, f);
        SetTextColor(dc_, color_ref(c));
        RECT rect{l, t, r, b};
        DrawTextW(dc_, s.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
      };
      text(label_font_, L"HEADSET", 20, 26, 126, 42, kLabel);
      text(value_font_, number(headset_hz, 0, L" Hz"), 20, 40, 126, 74, rate_color(headset_hz, 70, 45));
      text(label_font_, L"GAME", 142, 26, 236, 42, kLabel);
      text(value_font_, number(game_fps, 1), 142, 40, 236, 74,
           rate_color(game_fps, headset_hz > 0 ? headset_hz * 0.95 : 70, 45));
      // The size sent to the headset for one eye (Display settings may change it)
      // and the size the game renders one eye at.
      const auto size = [](std::uint32_t w, std::uint32_t h) {
        return w ? std::to_wstring(w) + L"\x00d7" + std::to_wstring(h) : std::wstring(L"\x2014");
      };
      text(label_font_, L"HEADSET EYE", 254, 26, 370, 42, kLabel);
      text(compact_font_, size(ow, oh), 254, 46, 370, 70, kCyan);
      text(label_font_, L"SOURCE RENDER", 650, 26, 792, 42, kLabel);
      text(value_font_, size(rw, rh), 650, 40, 792, 74, kCyan);
      text(label_font_, L"PREPARE", 808, 26, 940, 42, kLabel);
      text(value_font_, number(prepare, 1, L" ms"), 808, 40, 940, 74,
           prepare <= 4 ? kGreen : prepare <= 8 ? kAmber : kRed);
      const auto percent = [](const wchar_t* name, double v) {
        if (!(v >= 0)) return std::wstring(name) + L" \x2014";
        wchar_t t[32]{};
        std::swprintf(t, std::size(t), L"%ls %.0f%%", name, v);
        return std::wstring(t);
      };
      const auto memory = [](const wchar_t* name, double used, double total) {
        if (!(used >= 0) || !(total > 0)) return std::wstring(name) + L" \x2014";
        wchar_t t[48]{};
        std::swprintf(t, std::size(t), used < 10 ? L"%ls %.1f/%.0fG" : L"%ls %.0f/%.0fG", name, used, total);
        return std::wstring(t);
      };
      const auto load_color = [](double v) { return !(v >= 0) ? kLabel : v < 75 ? kGreen : v < 90 ? kAmber : kRed; };
      text(compact_font_, percent(L"PC CPU", load.cpu), 958, 28, 1044, 48, load_color(load.cpu));
      text(compact_font_, percent(L"PC GPU", load.gpu), 1058, 28, 1156, 48, load_color(load.gpu));
      text(compact_font_, memory(L"RAM", load.ram_used, load.ram_total), 958, 52, 1044, 72,
           load_color(load.ram_percent));
      text(compact_font_, memory(L"VRAM", load.vram_used, load.vram_total), 1058, 52, 1156, 72,
           load_color(load.vram_percent));
      GdiFlush();
      std::memcpy(p.data(), bits_, p.size() * 4);
      // GDI leaves alpha at zero where it draws: text pixels become opaque.
      for (auto& px : p)
        if ((px & 0xffffff) && !(px >> 24)) px |= 0xff000000u;
    }
    std::lock_guard lock(mutex_);
    panel_ = std::move(p);
    ++panel_sequence_;
  }
  std::string name_;
  std::wstring luid_;
  std::atomic_bool running_{true};
  mutable std::mutex mutex_;
  Load load_;
  std::vector<std::uint32_t> background_, panel_;
  std::uint64_t panel_sequence_ = 0;
  std::array<std::array<float, 72>, 2> history_{};
  std::size_t cursor_ = 0, filled_ = 0;
  HDC dc_{};
  HBITMAP bitmap_{};
  void* bits_{};
  HFONT label_font_{}, value_font_{}, compact_font_{};
  std::thread worker_;
};

class Overlay {
 public:
  const XrCompositionLayerBaseHeader* layer(XrSession session, const XrGraphicsBindingVulkanKHR& binding,
                                            PFN_vkGetInstanceProcAddr get, XrTime display_time) {
    if (failed_) return nullptr;
    count_frame(display_time);
    if (session != session_ || binding.device != binding_.device) {
      if (!create(session, binding, get)) {
        failed_ = true;
        return nullptr;
      }
    }
    update();
    return valid_ ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_) : nullptr;
  }

 private:
  // Headset rate from the spacing of display times; game rate from calls.
  void count_frame(XrTime display_time) {
    const auto now = std::chrono::steady_clock::now();
    if (window_start_ == std::chrono::steady_clock::time_point{}) window_start_ = now;
    ++frames_;
    if (last_display_ && display_time > last_display_) {
      const double period = double(display_time - last_display_);
      min_period_ = min_period_ > 0 ? std::min(min_period_, period) : period;
    }
    last_display_ = display_time;
    const double seconds = std::chrono::duration<double>(now - window_start_).count();
    if (seconds >= 0.5) {
      std::lock_guard lock(rates.mutex);
      rates.game_fps = frames_ / seconds;
      rates.headset_hz = min_period_ > 0 ? 1e9 / min_period_ : -1;
      frames_ = 0;
      min_period_ = 0;
      window_start_ = now;
    }
  }
  template <class T> T fn(const char* name) const { return reinterpret_cast<T>(get_(binding_.instance, name)); }
  bool create(XrSession session, const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get) {
    session_ = session;
    binding_ = binding;
    get_ = get;
    valid_ = false;
    // Formats: an sRGB RGBA chain, else BGRA (swizzled on upload).
    std::uint32_t count = 0;
    ::xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    std::vector<std::int64_t> formats(count);
    ::xrEnumerateSwapchainFormats(session, count, &count, formats.data());
    format_ = 0;
    for (std::int64_t f : {std::int64_t(VK_FORMAT_R8G8B8A8_SRGB), std::int64_t(VK_FORMAT_B8G8R8A8_SRGB)})
      if (!format_ && std::ranges::find(formats, f) != formats.end()) format_ = f;
    if (!format_) {
      log::warn("overlay", "the host offers no 8-bit sRGB swapchain format; the overlay is off");
      return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    space.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(::xrCreateReferenceSpace(session, &space, &space_))) return false;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format_;
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = kWidth;
    info.height = kHeight;
    {
      HostQueueLock queue(session_shared_gpu());
      if (!queue || XR_FAILED(::xrCreateSwapchain(session, &info, &chain_))) return false;
    }
    ::xrEnumerateSwapchainImages(chain_, 0, &count, nullptr);
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XR_FAILED(::xrEnumerateSwapchainImages(chain_, count, &count,
                                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()))))
      return false;
    // Upload: a host-visible staging buffer with one region per slot, and a
    // command buffer and fence per slot. An upload only reuses a slot whose
    // fence has signalled; the render thread never waits for the GPU.
    const auto device = binding.device;
    fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, binding.queueFamilyIndex, binding.queueIndex, &queue_);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kSlotBytes * kSlots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &buffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer_, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice, &mem);
    std::uint32_t type = 0;
    constexpr auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < mem.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << type)) || (mem.memoryTypes[type].propertyFlags & wanted) != wanted))
      ++type;
    if (type == mem.memoryTypeCount) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    if (fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &memory_) != VK_SUCCESS ||
        fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer_, memory_, 0) != VK_SUCCESS ||
        fn<PFN_vkMapMemory>("vkMapMemory")(device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS)
      return false;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = binding.queueFamilyIndex;
    if (fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
    if (fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ci, commands_.data()) != VK_SUCCESS)
      return false;
    // Signalled at creation: every slot starts free.
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (auto& fence : fences_)
      if (fn<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &fence) != VK_SUCCESS) return false;
    // The PC load sampler matches NVML / PDH to this physical device.
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    std::wstring luid;
    std::string name;
    if (auto query = fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")) {
      query(binding.physicalDevice, &props);
      name = props.properties.deviceName;
      if (id.deviceLUIDValid) {
        LUID l{};
        std::memcpy(&l, id.deviceLUID, sizeof(l));
        wchar_t text[64]{};
        std::swprintf(text, std::size(text), L"luid_0x%08x_0x%08x", std::uint32_t(l.HighPart), l.LowPart);
        luid = text;
      }
    }
    if (!sampler_) sampler_ = std::make_unique<Sampler>(name, luid);
    quad_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    quad_.space = space_;
    quad_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain = chain_;
    quad_.subImage.imageRect.extent = {std::int32_t(kWidth), std::int32_t(kHeight)};
    quad_.pose.orientation.w = 1;
    quad_.pose.position = {0, 0.23f, -0.8f};
    quad_.size = {0.85f, 0.85f * float(kHeight) / float(kWidth)};
    log::info("overlay", "Performance overlay ready ({} images, format {})", images_.size(), format_);
    return true;
  }
  static bool session_shared_gpu();
  // Uploads the newest painted panel when the next slot is free: twice a
  // second, as the sampler paints. Never waits for the GPU.
  void update() {
    const auto device = binding_.device;
    const auto slot = next_slot_;
    if (fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device, fences_[slot]) != VK_SUCCESS) return;
    if (!sampler_ || !sampler_->take_panel(pixels_, panel_seen_)) return;
    // VENTURE_OVERLAY_DUMP=<file>: the first painted panel, raw BGRA 1160x100.
    if (const char* dump = venture::environment::getenv("VENTURE_OVERLAY_DUMP"); dump && *dump && !dumped_) {
      dumped_ = true;
      if (FILE* f = std::fopen(dump, "wb")) {
        std::fwrite(pixels_.data(), 4, pixels_.size(), f);
        std::fclose(f);
      }
    }
    std::uint32_t index = 0;
    {
      HostQueueLock queue(session_shared_gpu());
      if (!queue || XR_FAILED(::xrAcquireSwapchainImage(chain_, nullptr, &index))) return;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    if (XR_FAILED(::xrWaitSwapchainImage(chain_, &wait))) return;
    auto* out = static_cast<std::uint8_t*>(mapped_) + kSlotBytes * slot;
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i < pixels_.size(); ++i) {
      const auto px = pixels_[i];
      const std::uint8_t r = std::uint8_t(px >> 16), g = std::uint8_t(px >> 8), b = std::uint8_t(px), a = std::uint8_t(px >> 24);
      out[i * 4 + 0] = bgra ? b : r;
      out[i * 4 + 1] = g;
      out[i * 4 + 2] = bgra ? r : b;
      out[i * 4 + 3] = a;
    }
    const auto command = commands_[slot];
    const auto image = images_.at(index).image;
    fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin);
    const auto barrier = [&](VkImageLayout from, VkImageLayout to) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = from;
      b.newLayout = to;
      b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                                           1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = kSlotBytes * slot;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kWidth, kHeight, 1};
    fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(command, buffer_, image,
                                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    bool submitted = false;
    {
      // The copy is queued before the release on the session's queue, which
      // orders it ahead of the compositor's use of the image.
      HostQueueLock queue(session_shared_gpu());
      fn<PFN_vkResetFences>("vkResetFences")(device, 1, &fences_[slot]);
      submitted = queue && fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &submit, fences_[slot]) == VK_SUCCESS;
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      if (queue) ::xrReleaseSwapchainImage(chain_, &release);
    }
    if (!submitted) {
      // Nothing will signal the reset fence: make the slot free again.
      fn<PFN_vkDestroyFence>("vkDestroyFence")(device, fences_[slot], nullptr);
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
      fn<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &fences_[slot]);
      return;
    }
    next_slot_ = (slot + 1) % kSlots;
    valid_ = true;
  }

  static constexpr std::uint32_t kSlots = 3;
  static constexpr VkDeviceSize kSlotBytes = VkDeviceSize(kWidth) * kHeight * 4;
  bool failed_ = false, valid_ = false, dumped_ = false;
  XrSession session_{};
  XrGraphicsBindingVulkanKHR binding_{};
  PFN_vkGetInstanceProcAddr get_{};
  std::int64_t format_ = 0;
  XrSpace space_{};
  XrSwapchain chain_{};
  std::vector<XrSwapchainImageVulkanKHR> images_;
  VkQueue queue_{};
  VkBuffer buffer_{};
  VkDeviceMemory memory_{};
  void* mapped_{};
  VkCommandPool pool_{};
  std::array<VkCommandBuffer, kSlots> commands_{};
  std::array<VkFence, kSlots> fences_{};
  std::uint32_t next_slot_ = 0;
  std::vector<std::uint32_t> pixels_;
  std::uint64_t panel_seen_ = 0;
  XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::unique_ptr<Sampler> sampler_;
  std::chrono::steady_clock::time_point window_start_{};
  std::uint64_t frames_ = 0;
  XrTime last_display_ = 0;
  double min_period_ = 0;
};
bool Overlay::session_shared_gpu() { return session().uses_shared_gpu; }

Overlay& overlay() {
  static Overlay instance;
  return instance;
}
}  // namespace

bool active() {
  const int state = shown.load(std::memory_order_relaxed);
  if (state >= 0) return state != 0;
  start_watcher();
  return shown.load() > 0;
}
const XrCompositionLayerBaseHeader* layer(XrSession host_session, const XrGraphicsBindingVulkanKHR& binding,
                                          PFN_vkGetInstanceProcAddr get, XrTime display_time) {
  if (!active() || !host_session || !binding.device || !get) return nullptr;
  return overlay().layer(host_session, binding, get, display_time);
}
void note_prepare(double milliseconds) {
  std::lock_guard lock(rates.mutex);
  rates.prepare_ms = rates.prepare_ms < 0 ? milliseconds : rates.prepare_ms * 0.9 + milliseconds * 0.1;
}
void note_render_size(std::uint32_t width, std::uint32_t height, std::uint32_t output_width,
                      std::uint32_t output_height) {
  std::lock_guard lock(rates.mutex);
  rates.render_width = width;
  rates.render_height = height;
  rates.output_width = output_width;
  rates.output_height = output_height;
}
}  // namespace venture::xr::stats_overlay
#endif
