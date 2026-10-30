// Headless integration probe: actual launcher, focused search, hide/restore,
// background ticks and final smoke screenshots. No game or account is needed.
#include "launcher/app.h"
#include <imgui_internal.h>
#include <cstring>
#include <iostream>

using namespace venture;
class Probe : public launcher::App {
public:
  Probe(launcher::Options options, std::string mode)
      : App(std::move(options)), mode_(std::move(mode)), started_(SDL_GetTicks()) {}
  bool passed() const {
    if (mode_ == "focus") return focused_ && typed_ && idle_end_ > idle_start_ && idle_end_ - idle_start_ <= 8;
    return hidden_ticks_ >= 5 && hidden_frames_ == 0 && resumed_;
  }
  Json report() const {
    return {{"mode", mode_}, {"focused", focused_}, {"typed", typed_}, {"idleFrames", idle_end_ - idle_start_},
            {"hiddenTicks", hidden_ticks_}, {"hiddenFrames", hidden_frames_}, {"resumed", resumed_}};
  }
protected:
  void tick() override {
    App::tick();
    const auto elapsed = SDL_GetTicks() - started_;
    const auto frames = ImGui::GetFrameCount();
    if (mode_ == "focus") {
      if (!activated_ && frames >= 3) {
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
          if (!std::strstr(window->Name, "/Content_")) continue;
          const auto search = ImHashStr("search", 0, window->ID);
          const auto input = ImHashStr("##input", 0, search);
          input_ = input;
          ImGui::GetIO().AddFocusEvent(true);
          ImGui::FocusWindow(window);
          ImGui::SetFocusID(input, window);
          ImGui::ActivateItemByID(input);
          activated_ = true;
          break;
        }
      }
      if (elapsed >= 2500 && !idle_start_) idle_start_ = frames;
      if (elapsed >= 5000 && !idle_end_) {
        idle_end_ = frames;
        focused_ = ImGui::IsAnyItemActive();
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = SDL_WindowID(reinterpret_cast<std::uintptr_t>(ImGui::GetMainViewport()->PlatformHandle));
        text.text.text = "probe";
        SDL_PushEvent(&text);
      }
      if (idle_end_)
        if (auto* input = ImGui::GetInputTextState(input_))
          typed_ = std::string_view(input->GetText()) == "probe";
    } else {
      int count = 0;
      auto** windows = SDL_GetWindows(&count);
      auto* window = count ? windows[0] : nullptr;
      SDL_free(windows);
      if (!window) return;
      if (elapsed >= 1500 && !hidden_at_) {
        SDL_HideWindow(window);
        hidden_at_ = frames;
      } else if (hidden_at_ && !restored_) {
        ++hidden_ticks_;
        hidden_frames_ = frames - hidden_at_;
        if (elapsed >= 3500) {
          SDL_ShowWindow(window);
          restored_ = true;
        }
      } else if (restored_ && frames > hidden_at_) {
        resumed_ = true;
      }
    }
  }
private:
  std::string mode_;
  std::uint64_t started_;
  ImGuiID input_ = 0;
  bool activated_ = false, focused_ = false, typed_ = false, restored_ = false, resumed_ = false;
  int idle_start_ = 0, idle_end_ = 0, hidden_at_ = 0, hidden_ticks_ = 0, hidden_frames_ = 0;
};
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  Probe probe(launcher::parse_options({argv, argv + argc}), argv[1]);
  const int result = probe.run();
  std::cout << probe.report().dump() << '\n';
  return result ? result : probe.passed() ? 0 : 1;
}
