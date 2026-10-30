#include "launcher/exit_report.h"
#include "launcher/frame_pacing.h"
#include "launcher/app.h"
#include "common/library/import.h"
#include "common/download/asset_files.h"
#include "common/file_ops.h"
#include "common/saves_migration.h"
#include "hle/ovrplatform/broker.h"
#include "common/log.h"
#include <chrono>
#include <csignal>
#include "launcher/ui/logo.h"
#include "launcher/ui/style.h"
#include "launcher/window_style.h"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>

namespace venture::launcher {
namespace {
volatile std::sig_atomic_t exit_signal = 0;
void request_signal_exit(int signal) { exit_signal = signal; }
// Developer aid for reviewing the UI without a visible desktop.
void save_screenshot(SDL_Renderer *renderer, const fs::path &file) {
  SDL_Surface *raw = SDL_RenderReadPixels(renderer, nullptr);
  SDL_Surface *rgba =
      raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32) : nullptr;
  SDL_DestroySurface(raw);
  if (!rgba)
    throw std::runtime_error("Could not read back the launcher frame.");
  bool ok = stbi_write_png(paths::utf8(file).c_str(), rgba->w, rgba->h, 4,
                           rgba->pixels, rgba->pitch) != 0;
  SDL_DestroySurface(rgba);
  if (!ok)
    throw std::runtime_error("Could not write the screenshot.");
}
} // namespace
// The UI's scale in window coordinates. Those are physical pixels on Windows but logical units
// on Wayland and macOS, where the renderer already applies the pixel density.
static float ui_scale(SDL_Window *window) {
  const auto density = SDL_GetWindowPixelDensity(window);
  return std::max(1.f, SDL_GetWindowDisplayScale(window) / (density > 0 ? density : 1.f));
}
void App::fit_window() {
  scale_ = ui_scale(window_) * float(settings_.interface_size) / 100;
  int width = int(1380 * scale_), height = int(880 * scale_);
  int min_width = int(900 * scale_), min_height = int(620 * scale_);
  // Never larger than the screen: a compositor squeezes an oversized window, which puts the UI
  // and the mouse out of step.
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable) && usable.w > 0 && usable.h > 0) {
    width = std::min(width, usable.w * 9 / 10);
    height = std::min(height, usable.h * 9 / 10);
    min_width = std::min(min_width, width);
    min_height = std::min(min_height, height);
  }
  SDL_SetWindowMinimumSize(window_, min_width, min_height);
  SDL_SetWindowSize(window_, width, height);
}
App::App(Options options) : Core(std::move(options)) {
  settings_games_ = paths::utf8(settings_.games);
  settings_runtime_ =
      settings_.runtime_override() ? paths::utf8(settings_.runtime) : "";
  settings_adb_ = paths::utf8(settings_.adb);
  prefer_native_video_driver();
  if (!SDL_Init(SDL_INIT_VIDEO))
    throw std::runtime_error(std::string(video_unavailable_message()) + " (" + SDL_GetError() + ")");
  window_ = SDL_CreateWindow(
      "Venture", 1380, 880,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
          (client_side_decorations() ? SDL_WINDOW_BORDERLESS : 0));
  if (!window_)
    throw std::runtime_error("Could not create the launcher window.");
  fit_window();
  if (client_side_decorations())
    SDL_SetWindowHitTest(
        window_,
        [](SDL_Window *, const SDL_Point *point, void *app) {
          return chrome::hit_test(static_cast<App *>(app)->chrome_, *point);
        },
        this);
  SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED,
                        SDL_WINDOWPOS_CENTERED);
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_)
    throw std::runtime_error("Could not create an SDL renderer.");
  const bool vsync = SDL_SetRenderVSync(renderer_, 1);
  log::info("launcher", "UI renderer={} vsync={}", SDL_GetRendererName(renderer_), vsync);
  style_native_window(window_, 26, 27, 30);
  if (SDL_Surface *icon = ui::icon_surface(256)) {
    SDL_SetWindowIcon(window_, icon);
    SDL_DestroySurface(icon);
  }
  wake_event_ = SDL_RegisterEvents(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ui::load_fonts();
  theme();
  ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer3_Init(renderer_);
  textures_ =
      std::make_unique<TextureCache>(renderer_, data_, [this] { wake(); });
  start();
  if (options_.smoke_install.empty() && !options_.launch_requested)
    SDL_ShowWindow(window_);
  if (options_.smoke_view == "downloads")
    page_ = 1;
  if (options_.smoke_view == "settings" || options_.smoke_view == "settings-end")
    page_ = 2;
  if (options_.smoke_view == "account")
    page_ = 3;
  if (options_.smoke_view == "import")
    page_ = 4;
  // The update dialog an installed copy shows (nothing downloads until Update now).
  if (options_.smoke_view == "update") {
    const std::string release = "https://github.com/Villagers654/Venture/releases/";
    update_checked_ = true;
    update_ = Update{"0.1.1", release + "tag/v0.1.1"};
    update_->method = update::Method::installer;
    update_->file = update::Asset{"Venture-windows64-setup.exe", release + "download/v0.1.1/Venture-windows64-setup.exe"};
    update_->sums = update::Asset{"SHA256SUMS", release + "download/v0.1.1/SHA256SUMS"};
  }
  if (!options_.smoke_import.empty())
    scan_local(options_.smoke_import);
}
App::~App() {
  shutdown();
  textures_.reset();
  for (auto &[pixels, texture] : logos_)
    SDL_DestroyTexture(texture);
  for (auto &[pixels, texture] : steam_logos_)
    SDL_DestroyTexture(texture);
  for (auto &[pixels, texture] : discord_logos_)
    SDL_DestroyTexture(texture);
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (renderer_)
    SDL_DestroyRenderer(renderer_);
  if (window_)
    SDL_DestroyWindow(window_);
  SDL_Quit();
  log::info("launcher", "Launcher closed.");
  log::set_file({});
}
void App::wake() {
  SDL_Event e{};
  e.type = wake_event_;
  SDL_PushEvent(&e);
}
void App::on_refresh() { textures_->retry(); }
void App::on_install_queued() {
  // A smoke that opens a game page keeps it so the page's progress is visible.
  if (options_.smoke_select.empty())
    page_ = 1;
}
void App::on_import_scan() {
  page_ = 4;
  import_choices_.clear();  // free space changes between scans
}
void App::smoke_install() {
  if (options_.smoke_install.empty() || smoke_exit_ || refreshing_ || scanning_)
    return;
  const auto app = options_.smoke_install;
  if (!smoke_install_home_)
    smoke_install_home_ = account_key_;
  auto finish = [&](std::string error) {
    smoke_install_error_ = std::move(error);
    smoke_exit_ = smoke_install_error_.empty() ? 0 : 1;
    if (auto job = download::base_job(downloads_->snapshot(), app))
      smoke_install_state_ = download::label(job->state);
    if (smoke_install_error_.empty())
      log::info("launcher", "Smoke install of {} completed.", app);
    else
      log::warn("launcher", "Smoke install of {} failed: {}", app,
                smoke_install_error_);
    // Finding the owner switches accounts as a user would; a smoke leaves the
    // saved active account as it found it.
    if (!smoke_install_home_->empty() && account_key_ != *smoke_install_home_) {
      try {
        switch_account(*smoke_install_home_);
      } catch (...) {
        log::warn("launcher", "Could not restore the active account: {}",
                  exception_text());
      }
    }
  };
  if (!smoke_install_queued_) {
    if (!session_ || options_.offline)
      return finish("Sign in to Meta before installing.");
    // A failed refresh says nothing about ownership.
    if (!refresh_error_.empty())
      return finish("Could not read the Meta library: " + refresh_error_);
    if (game_owners(app).empty()) {
      // An account's library is read while it is active, as when the user
      // switches accounts to find a game.
      smoke_install_tried_.insert(account_key_);
      std::string next;
      for (const auto &a : accounts_)
        if (a.signed_in && !a.needs_reauth &&
            !smoke_install_tried_.contains(a.key)) {
          next = a.key;
          break;
        }
      if (next.empty())
        return finish("No signed-in account owns this game.");
      try {
        switch_account(next);
      } catch (...) {
        smoke_install_tried_.insert(next);
      }
      return;
    }
    auto g = game(app);
    if (!g)
      return finish("Unknown game.");
    try {
      install_for_owner(*g);
    } catch (...) {
      return finish(exception_text());
    }
    smoke_install_queued_ = true;
    return;
  }
  auto job = download::base_job(downloads_->snapshot(), app);
  if (!job)
    return finish("The install job was removed.");
  if (job->state == download::State::failed ||
      job->state == download::State::paused)
    return finish(job->error.empty() ? "The install did not finish."
                                     : job->error);
  if (job->state != download::State::complete)
    return;
  // Judge the files from a scan that started after the job completed.
  if (smoke_install_scan_ == 0) {
    smoke_install_scan_ = 1;
    rescan();
    return;
  }
  auto install = installed(app, false);
  if (!install || !install->complete)
    return finish("The installed files did not pass the size check.");
  finish({});
}
// --smoke-import-start: imports what the --smoke-import scan found, as the
// Import button does, and exits once every queued import has finished.
void App::smoke_import() {
  if (!options_.smoke_import_start || smoke_exit_ || import_local_.busy) return;
  auto finish = [&](std::string error) {
    smoke_import_error_ = std::move(error);
    smoke_exit_ = smoke_import_error_.empty() ? 0 : 1;
    if (smoke_import_error_.empty()) log::info("launcher", "Smoke import completed.");
    else log::warn("launcher", "Smoke import failed: {}", smoke_import_error_);
  };
  if (!smoke_import_started_) {
    // Matching reads the library and the installed games.
    if (refreshing_ || scanning_) return;
    smoke_import_started_ = true;
    if (!import_local_.error.empty()) return finish(import_local_.error);
    for (const auto &item : import_local_.items)
      if (item.selected)
        if (auto match = import_match(item); match.importable)
          smoke_import_jobs_.push_back(match.game ? match.game->id : item.package);
    try {
      start_import(false);
    } catch (...) {
      finish(exception_text());
    }
    return;
  }
  const auto jobs = downloads_->snapshot();
  for (const auto &id : smoke_import_jobs_) {
    auto job = download::base_job(jobs, id);
    if (!job) return finish("The import job for " + id + " was removed.");
    if (job->state == download::State::failed || job->state == download::State::paused)
      return finish(job->error.empty() ? "The import of " + id + " did not finish." : job->error);
    if (job->state != download::State::complete) return;
  }
  finish({});
}
void App::save_settings(bool announce) {
  auto next = settings_;
  next.games = paths::from_utf8(settings_games_).lexically_normal();
  // An empty runtime field means the runtime beside the launcher.
  next.runtime = settings_runtime_.empty()
                     ? library::Settings::defaults(data_).runtime
                     : paths::from_utf8(settings_runtime_).lexically_normal();
  next.adb = settings_adb_.empty()
                 ? fs::path{}
                 : paths::from_utf8(settings_adb_).lexically_normal();
  apply_settings(std::move(next), announce);
}
SDL_Texture *App::logo(int pixels) {
  auto [it, inserted] = logos_.try_emplace(pixels, nullptr);
  if (inserted)
    if (SDL_Surface *surface = ui::logo_surface(pixels)) {
      it->second = SDL_CreateTextureFromSurface(renderer_, surface);
      SDL_DestroySurface(surface);
    }
  return it->second;
}
SDL_Texture *App::steam_logo(int pixels) {
  auto [it, inserted] = steam_logos_.try_emplace(pixels, nullptr);
  if (inserted)
    if (SDL_Surface *surface = ui::steam_logo_surface(pixels)) {
      it->second = SDL_CreateTextureFromSurface(renderer_, surface);
      SDL_DestroySurface(surface);
    }
  return it->second;
}
SDL_Texture *App::discord_logo(int pixels) {
  auto [it, inserted] = discord_logos_.try_emplace(pixels, nullptr);
  if (inserted)
    if (SDL_Surface *surface = ui::discord_logo_surface(pixels)) {
      it->second = SDL_CreateTextureFromSurface(renderer_, surface);
      SDL_DestroySurface(surface);
    }
  return it->second;
}
void App::tick() {
  Core::tick();
  take_import_picks();
  textures_->drain();
  for (auto &g : visible_games())
    textures_->request(g, !options_.offline);
}
void App::choose_import(bool folder) {
  // Portal (or zenity) dialogs answer on another thread; picks queue up for
  // the UI thread.
  auto picked = [](void *self, const char *const *files, int) {
    auto *app = static_cast<App *>(self);
    if (!files) {
      log::warn("import", "The file dialog failed: {}", SDL_GetError());
      return;
    }
    {
      std::lock_guard lock(app->import_mutex_);
      for (auto f = files; *f; ++f) app->import_picks_.push_back(paths::from_utf8(*f));
    }
    app->wake();
  };
  if (folder) {
    SDL_ShowOpenFolderDialog(picked, this, window_, nullptr, false);
  } else {
    static const SDL_DialogFileFilter filters[] = {{"Android apps and archives", "apk;zip"}};
    SDL_ShowOpenFileDialog(picked, this, window_, filters, 1, nullptr, false);
  }
}

void App::choose_save_folder(const std::string &app_id) {
  struct Pick {
    App *app;
    std::string id;
  };
  auto picked = [](void *data, const char *const *files, int) {
    std::unique_ptr<Pick> pick(static_cast<Pick *>(data));
    if (!files) {
      log::warn("import", "The folder dialog failed: {}", SDL_GetError());
      return;
    }
    if (!*files) return;  // cancelled
    {
      std::lock_guard lock(pick->app->import_mutex_);
      pick->app->save_picks_.emplace_back(pick->id, paths::from_utf8(*files));
    }
    pick->app->wake();
  };
  SDL_ShowOpenFolderDialog(picked, new Pick{this, app_id}, window_, nullptr, false);
}

void App::take_import_picks() {
  std::vector<fs::path> picks;
  std::vector<std::pair<std::string, fs::path>> save_picks;
  {
    std::lock_guard lock(import_mutex_);
    picks.swap(import_picks_);
    save_picks.swap(save_picks_);
  }
  for (auto &p : picks) scan_local(p);
  for (auto &[id, folder] : save_picks) action([&] { import_saves_later(id, folder); });
}

void App::report_smoke() {
  if (options_.smoke_report.empty() || smoke_reported_)
    return;
  smoke_reported_ = true;
  Json report{{"signedIn", session_.has_value()},
              {"accounts", accounts_.size()},
              {"offline", options_.offline},
              {"games", visible_games().size()},
              {"installed", std::ranges::count_if(
                                installs_, [](auto &i) { return i.complete; })},
              {"artworkTextures", textures_->size()},
              {"liveDetails", details_.size()},
              {"authPageLoaded", auth_page_loaded_},
              {"playAttempted", smoke_played_},
              {"playRequestedUnixMs", smoke_play_requested_unix_ms_},
              {"renderer", SDL_GetRendererName(renderer_)},
              {"renderedFrames", ImGui::GetFrameCount()},
              {"dpiScale", scale_}};
  Json exits = Json::array();
  for (auto &[id, r] : running_)
    if (r.exit)
      exits.push_back({{"appId", id},
                       {"exitCode", *r.exit},
                       {"logBytes", r.tail.size()},
                       {"error", r.failure}});
  report["processes"] = exits;
  std::vector<std::optional<int>> codes;
  for (auto &[id, r] : running_) codes.push_back(r.exit);
  report["exit"] = exit_report(exit_reason_, smoke_exit_.value_or(0), int(exit_signal), codes);
  if (!options_.smoke_install.empty()) {
    const auto &app = options_.smoke_install;
    Json install{{"appId", app},
                 {"ok", smoke_exit_ == 0},
                 {"error", smoke_install_error_}};
    if (!smoke_install_state_.empty())
      install["state"] = smoke_install_state_;
    else if (auto job = download::base_job(downloads_->snapshot(), app))
      install["state"] = download::label(job->state);
    if (auto i = installed(app, false)) {
      size_t base = 0, assets = 0;
      for (auto &f : i->plan.files) {
        base += f.base();
        assets += f.kind == "asset";
      }
      install.update({{"directory", paths::utf8(i->directory)},
                      {"versionCode", i->plan.version_code},
                      {"complete", i->complete},
                      {"bytes", i->bytes},
                      {"launcherPlan", i->plan.launcher_format},
                      {"assetCatalog", i->plan.assets_complete},
                      {"catalogFiles", i->plan.files.size()},
                      {"baseFiles", base},
                      {"assetFiles", assets}});
    }
    report["install"] = install;
  }
  paths::atomic_write(options_.smoke_report, report.dump(2));
  auto screenshot = options_.smoke_report;
  screenshot.replace_extension("bmp");
  auto surface = SDL_RenderReadPixels(renderer_, nullptr);
  if (surface) {
    SDL_SaveBMP(surface, paths::utf8(screenshot).c_str());
    SDL_DestroySurface(surface);
  }
}
int App::run() {
  exit_signal = 0;
  const auto previous_int = std::signal(SIGINT, request_signal_exit);
  const auto previous_term = std::signal(SIGTERM, request_signal_exit);
  auto started = SDL_GetTicks();
  FramePacing pacing(started);
  auto last_tick = started;
  while (!quit_) {
    if (exit_signal) {
      exit_reason_ = "signal";
      smoke_exit_ = 128 + int(exit_signal);
      break;
    }
    auto now = SDL_GetTicks();
    bool work_ready = false;
    SDL_Event event{};
    auto process = [&](const SDL_Event &e) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT ||
          (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
           e.window.windowID == SDL_GetWindowID(window_))) {
        if (exit_reason_.empty())
          exit_reason_ = e.type == SDL_EVENT_QUIT ? "quit_event" : "window_close";
        quit_ = true;
      }
      if ((e.type >= SDL_EVENT_KEY_DOWN && e.type <= SDL_EVENT_MOUSE_WHEEL) ||
          (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) ||
          e.type == SDL_EVENT_DROP_FILE || e.type == wake_event_)
        pacing.wake(SDL_GetTicks());
      if (e.type == wake_event_) work_ready = true;
      // A dropped APK, folder or archive opens the import page with it.
      if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
        scan_local(paths::from_utf8(e.drop.data));
      if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        scale_ = ui_scale(window_) * float(settings_.interface_size) / 100;
        theme();
      }
    };
    auto hidden = [&] { return SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN); };
    const int wait = pacing.wait(now, hidden());
    if (wait && SDL_WaitEventTimeout(&event, wait))
      process(event);
    while (SDL_PollEvent(&event))
      process(event);
    if (quit_) break;
    now = SDL_GetTicks();
    // A held drag needs continuous frames; mere text-field focus does not.
    if (SDL_GetMouseFocus() == window_ && SDL_GetMouseState(nullptr, nullptr))
      pacing.wake(now);
    if (!options_.launch_requested && !work_ready && !pacing.ready(now, hidden()) && now - last_tick < 250)
      continue;
    tick();
    last_tick = SDL_GetTicks();
    if (options_.launch_requested) {
      if (!direct_started_ && !scanning_) {
        direct_started_ = true;
        auto found = std::ranges::find_if(installs_, [&](const auto& i) {
          return i.complete && i.plan.package == options_.launch_package;
        });
        if (found == installs_.end()) {
          error("The requested package is not installed.");
          smoke_exit_ = 1;
        } else {
          direct_app_ = found->plan.app_id;
          try { launch_game(direct_app_, play_key(direct_app_), {}); }
          catch (...) { error(exception_text()); smoke_exit_ = 1; }
        }
      }
      if (direct_started_ && !scanning_ && !launch_pending_.contains(direct_app_)) {
        auto r = running_.find(direct_app_);
        if (r == running_.end()) smoke_exit_ = 1;
        else if (r->second.exit) smoke_exit_ = *r->second.exit;
      }
      if (smoke_exit_) { exit_reason_ = "direct_launch_exit"; break; }
      continue;
    }
    if (options_.smoke_details && !smoke_detail_started_ && !refreshing_ &&
        !scanning_) {
      auto visible = visible_games();
      if (!visible.empty()) {
        smoke_detail_started_ = true;
        select(visible.front().id);
      }
    }
    if (!options_.smoke_select.empty() && !smoke_detail_started_ &&
        !scanning_ && game(options_.smoke_select)) {
      smoke_detail_started_ = true;
      select(options_.smoke_select);
      // The install dialog of a game that is not installed (nothing downloads offline).
      if (options_.smoke_view == "install")
        action([&] { request_install(*game(options_.smoke_select)); });
      if (options_.smoke_view == "saves") save_import_target_ = options_.smoke_select;
    }
    // Ownership comes from the library refresh; without a cached library a
    // play before it finishes is refused as not owned.
    if (options_.smoke_play && !smoke_played_ && !scanning_ && !refreshing_) {
      for (auto &g : visible_games())
        if (installed(g.id) &&
            (options_.smoke_select.empty() || options_.smoke_select == g.id)) {
          smoke_played_ = true;
          smoke_play_requested_unix_ms_ = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count());
          action([&] {
            play(g);
            save_settings();
          });
          break;
        }
    }
    smoke_install();
    smoke_import();
    if (options_.smoke_auth && !smoke_auth_opened_ && !refreshing_) {
      smoke_auth_opened_ = true;
      sign_in();
    }
    const bool timed_out =
        options_.smoke_seconds &&
        SDL_GetTicks() - started >= uint64_t(options_.smoke_seconds) * 1000;
    // A timeout/completed smoke still draws its final screenshot. Ordinary
    // minimized/hidden windows keep workers and game polling without drawing.
    if (!timed_out && !smoke_exit_ && !pacing.ready(SDL_GetTicks(), hidden()))
      continue;
    const auto frame = SDL_GetTicks();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 26, 27, 30, 255);
    SDL_RenderClear(renderer_);
    // The backend scales clip rects but not vertices: map window coordinates
    // to the HiDPI framebuffer (Wayland, macOS) through the render scale.
    const auto framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
    SDL_SetRenderScale(renderer_, framebuffer_scale.x, framebuffer_scale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (timed_out && !options_.smoke_install.empty() && !smoke_exit_) {
      smoke_install_error_ = "Timed out before the install finished.";
      smoke_exit_ = 1;
    }
    if (timed_out && options_.smoke_import_start && !smoke_exit_) {
      smoke_import_error_ = "Timed out before the import finished.";
      smoke_exit_ = 1;
    }
    if (timed_out || smoke_exit_) {
      if (exit_reason_.empty())
        exit_reason_ = timed_out ? "timeout"
                       : options_.smoke_import_start ? (*smoke_exit_ == 0 ? "import_complete" : "import_failed")
                       : (*smoke_exit_ == 0 ? "install_complete" : "install_failed");
      if (!options_.screenshot.empty())
        action([&] { save_screenshot(renderer_, options_.screenshot); });
      quit_ = true;
    }
    SDL_RenderPresent(renderer_);
    pacing.presented(frame);
  }
  if (exit_reason_.empty()) exit_reason_ = "launcher_quit";
  std::vector<std::optional<int>> codes;
  for (auto &[id, r] : running_) {
    r.exit = r.process.poll();
    codes.push_back(r.exit);
  }
  log::info("launcher", "Exit {}", exit_report(exit_reason_, smoke_exit_.value_or(0),
                                              int(exit_signal), codes).dump());
  action([&] { report_smoke(); });
  std::signal(SIGINT, previous_int);
  std::signal(SIGTERM, previous_term);
  return smoke_exit_.value_or(0);
}
int run(Options options) {
  if (options.invalid_launch) {
    log::error("launcher", "Usage: venture --launch <installed.package>");
    return 2;
  }
  const auto report_path = options.smoke_report;
  const bool direct = options.launch_requested;
  try {
    App app(std::move(options));
    return app.run();
  } catch (const std::exception &e) {
    const auto status = exit_report("launcher_exception", 1, 0, {});
    log::error("launcher", "Startup failed: {}", e.what());
    log::error("launcher", "Exit {}", status.dump());
    if (!report_path.empty()) {
      try { paths::atomic_write(report_path, Json{{"exit", status}}.dump(2)); }
      catch (...) {} // Preserve the original failure when the report directory is inaccessible.
    }
    log::error("launcher", "Launcher initialization failed. See system "
                           "graphics configuration and folder permissions.");
    if (!direct) SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "Venture",
        "Venture could not start. Check graphics drivers and access to the "
        "application data folder.",
        nullptr);
    return 1;
  }
}
} // namespace venture::launcher
