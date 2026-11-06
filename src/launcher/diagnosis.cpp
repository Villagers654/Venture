// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/diagnosis.h"
#include <array>
#include <cctype>

namespace venture::launcher {
namespace {
std::string lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

struct Rule {
  std::array<std::string_view, 6> signatures;  // any one matches; lower case
  std::string_view cause, advice;
};
constexpr std::string_view kSendReport = "Send this report with an issue: the game needs a fix in Venture.";
constexpr std::string_view kGpuAdvice =
    "Update the graphics driver and turn off overlays and capture tools (MangoHud, vkBasalt, OBS), then try again.";
// Most specific first: the first rule that matches names the cause. Each
// signature is a line Venture (or the system) writes for that failure.
constexpr Rule kRules[] = {
    // posix/window_style.cpp video_unavailable_message().
    {{"needs a wayland session, and no wayland display was reachable"},
     "Venture found no Wayland session to open its window in.",
     "Log in to a Wayland desktop session (Venture does not use X11), then start Venture again."},
    // hle/ovrplatform/account_store.cpp: the game's process could not read the account.
    {{"meta session load failed stage=vault-session", "meta session load failed stage=vault-load",
      "meta session load failed stage=vault-index", "meta session load failed stage=empty-token"},
     "The game could not read your Meta sign-in.",
     "Make sure your desktop keyring is running and unlocked (Linux), or choose Reconnect on the account in "
     "Accounts, then start the game again."},
    // ovrplatform: Meta refused the game's sign-in or entitlement check.
    {{"[ovrplatform] initialization failed error=-20001"},
     "The game needs a signed-in Meta account that owns it.",
     "Sign in with the account that owns the game, then start it again."},
    // xr/host_xr.cpp host_initialization_error().
    {{"your configured openxr runtime is unavailable", "xr_error_runtime_unavailable"},
     "No OpenXR runtime was running, or none is set as the active one.",
     "Start your VR runtime (SteamVR, WiVRn or Monado), make it the active OpenXR runtime, connect the headset, "
     "then press Play again."},
    {{"the openxr runtime failed to create an instance (xrresult -2)"},
     "The VR runtime could not start a session; it is often still shutting down from an earlier one.",
     "Close your VR runtime completely and start it again, then press Play again."},
    {{"does not support vulkan (xr_khr_vulkan_enable2)"},
     "The active OpenXR runtime does not support Vulkan, which Venture renders with.",
     "Update your VR runtime or switch to one with Vulkan support (SteamVR, WiVRn, Monado)."},
    {{"hmd result -35 (flat mode)", "xr_error_form_factor_unavailable"},
     "Your VR runtime had no headset connected.",
     "Connect the headset and make sure it is tracking, then start the game again."},
    // xr/session.cpp session_failed(): the game ran on without its session.
    {{"the vr session could not be created: "},
     "The VR runtime did not create the game's VR session.",
     "Update Venture and your graphics driver, close other VR tools and overlays (OpenXR Toolkit, other OpenXR "
     "layers), restart your VR runtime, then try again. If it keeps happening, send this report."},
    // library.cpp missing_expansion().
    {{"[vfs] missing expansion file: the game looked for "},
     "The game looked for an expansion file (OBB) that its install does not have.",
     "Install the game again, or import it again together with its OBB file."},
    // The kernel's own reports of a GPU reset (amdgpu, i915/xe, NVIDIA Xid).
    {{"gpu reset", "ring timeout", "nvrm: xid", "gpu hang",
      "stopped responding and has successfully recovered"},  // Windows: Display event 4101
     "The graphics driver reset the GPU during the game.", kGpuAdvice},
    // hle/vulkan/vulkan.cpp, and the host's own device loss.
    {{"native vulkan device lost", "vk_error_device_lost"},
     "The game lost the graphics device.", kGpuAdvice},
    // core/alloc_failure.cpp: a refused allocation and the limit it likely hit.
    {{"likely limit: system commit limit"},
     "The PC ran out of commit memory (RAM plus page file), so memory requests failed even though RAM and GPU "
     "memory looked free. Graphics drivers count GPU memory against this limit too.",
     "Let Windows manage the page file size, or set it to at least 16 GB (Settings > System > About > Advanced "
     "system settings > Performance > Advanced > Virtual memory), close other large programs, then try again. "
     "On Linux, add swap or use vm.overcommit_memory=0."},
    {{"likely limit: vulkan maxmemoryallocationcount"},
     "The game reached the graphics driver's limit on the number of GPU memory allocations.",
     "Update the graphics driver and send this report with an issue."},
    {{"likely limit: small cpu-visible vram heap"},
     "The small part of GPU memory the CPU can write to filled up (Resizable BAR is off).",
     "Turn on Resizable BAR (Re-Size BAR or Smart Access Memory) in the PC's firmware settings if it supports it, "
     "update the graphics driver, and send this report with an issue."},
    {{"likely limit: gpu memory budget"},
     "The GPU ran out of memory.",
     "Close other programs that use the GPU or lower the resolution (Settings > Display), then try again."},
    {{"likely limit: jit code space"},
     "Venture's store of translated game code could not grow.",
     "Close other large programs, then try again. If it keeps happening, send this report with an issue."},
    {{"likely limit: guest heap arena"},
     "The game used up the memory area Venture gives it.", kSendReport},
    {{"likely limit: none near"},
     "A memory request was refused although no memory limit was near.",
     "Update the graphics driver, close other programs, then try again. If it keeps happening, send this report "
     "with an issue."},
    {{"vk_error_out_of_device_memory", "gl_out_of_memory", "xr_error_out_of_memory", "out_of_device_memory"},
     "The GPU ran out of memory.",
     "Close other programs that use the GPU or lower the resolution (Settings > Display), then try again."},
    {{"out of memory: killed process", "oom-kill"},
     "The system ran out of memory and stopped a program.",
     "Close other programs, then try again."},
    // runtime_failure(): a trap names the import or ABI adapter that is missing.
    {{"[hle] unimplemented ", "[a32-abi] unsupported "},
     "The game called an Android function that Venture does not provide yet.", kSendReport},
    {{"[libc] fatal signal ", "e [signal] guest tid "},
     "The game crashed.", kSendReport},
};
} // namespace

bool error_line(std::string_view line) {
  static constexpr std::string_view words[] = {
      "error",     "errors",        "fail",      "failed",    "failure",   "fatal",       "panic",
      "crash",     "crashed",       "exception", "timed out", "timeout",   "unsupported", "not supported",
      "not found", "device lost",   "segfault",  "denied",    "gpu reset", "page fault",  "hung",
      "oom",       "xid",           "out of memory"};
  const std::string text = lower(line);
  for (auto word : words)
    for (size_t at = 0; (at = text.find(word, at)) != std::string::npos; at += word.size()) {
      const size_t end = at + word.size();
      if ((at == 0 || !word_char(text[at - 1])) && (end == text.size() || !word_char(text[end])))
        return true;
    }
  return false;
}

bool noisy_line(std::string_view line) {
  for (auto noise : {"Failed to parse bindings for ViveController",
                     "Failed to parse bindings for Unknown(\"holographic_controller\")",
                     "Unsupported application type: Utility", "Listening on systemd-oomd"})
    if (line.find(noise) != std::string_view::npos) return true;
  return line.find("Registered") != std::string_view::npos && line.find("drm panic") != std::string_view::npos;
}

std::optional<Diagnosis> diagnose(std::string_view evidence) {
  const std::string text = lower(evidence);
  for (const auto &rule : kRules)
    for (auto signature : rule.signatures)
      if (!signature.empty() && text.find(signature) != std::string::npos)
        return Diagnosis{std::string(rule.cause), std::string(rule.advice)};
  return std::nullopt;
}
} // namespace venture::launcher
