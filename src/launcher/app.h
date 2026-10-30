#pragma once
// The launcher window: an ImGui view over launcher::Core (launcher/core.h).
#include "launcher/assets/texture_cache.h"
#include "launcher/core.h"
#include "launcher/drives.h"
#include "launcher/updater.h"
#include "launcher/ui/chrome.h"
#include <imgui.h>
#include <atomic>
#include <map>
#include <memory>
namespace venture::launcher {
class App : public Core {
public:
  explicit App(Options options);
  ~App() override;
  int run();

private:
  void wake() override;
  void fit_window();  // size from the display scale, within the screen
  void on_refresh() override;
  void on_install_queued() override;
  void on_import_scan() override;
  void tick() override;
  SDL_Window *window_ = nullptr;
  SDL_Renderer *renderer_ = nullptr;
  uint32_t wake_event_ = 0;
  std::unique_ptr<TextureCache> textures_;
  std::set<std::string> addons_expanded_;      // games showing unowned add-ons
  std::optional<std::string> remove_account_;  // pending removal confirmation
  bool remove_saves_ = false;
  // The install dialog: where the game goes (free bytes there), Steam, and "don't ask again".
  std::vector<std::pair<launcher::InstallLocation, uint64_t>> install_choices_;
  int install_choice_ = 0;
  // Where imported games go, listed when the Import page opens or rescans.
  std::vector<std::pair<launcher::InstallLocation, uint64_t>> import_choices_;
  int import_choice_ = 0;
  bool install_steam_ = true, install_remember_ = false;
  std::string direct_app_;
  bool direct_started_ = false;
  std::string backdrop_; // game whose ambient art tints the game page
  std::string search_, settings_games_, settings_runtime_, settings_adb_;
  std::string toast_;         // notice_ as currently shown
  double toast_since_ = 0;    // ImGui time it appeared
  std::set<std::string> log_open_; // games whose runtime log is expanded
  int page_ = 0, sort_ = 1, game_tab_ = 0;
  std::map<int, SDL_Texture *> logos_; // rasterized logo by pixel size
  SDL_Texture *logo(int pixels);
  std::map<int, SDL_Texture *> steam_logos_;
  SDL_Texture *steam_logo(int pixels);
  std::map<int, SDL_Texture *> discord_logos_;
  SDL_Texture *discord_logo(int pixels);
  bool installed_only_ = false, quit_ = false;
  bool smoke_played_ = false, smoke_auth_opened_ = false, smoke_reported_ = false;
  bool smoke_detail_started_ = false;
  uint64_t smoke_play_requested_unix_ms_ = 0;
  // --smoke-install: accounts whose library was read, the queued job, the
  // post-download rescan step, the outcome and the process exit code.
  std::set<std::string> smoke_install_tried_;
  bool smoke_install_queued_ = false;
  int smoke_install_scan_ = 0;
  std::string smoke_install_error_;
  std::string smoke_install_state_;                // the job's last state
  std::optional<std::string> smoke_install_home_;  // active account at start
  std::optional<int> smoke_exit_;
  bool smoke_import_started_ = false;
  std::vector<std::string> smoke_import_jobs_;  // the queued imports' job ids
  std::string smoke_import_error_;
  std::string exit_reason_;
  float scale_ = 1;
  chrome::Frame chrome_{};  // last frame's window chrome, for the hit test
  uint64_t last_input_ = 0;
  std::optional<library::Install> uninstall_target_;
  std::optional<download::Job> cancel_target_;
  struct SharedReport {
    fs::path file;
    std::string url, error;  // the upload's link or why it failed; neither while uploading
    bool copied = false;
  };
  std::optional<SharedReport> report_;
  // A newer published release than this build, offered once per version.
  struct Update {
    std::string version, url;
    // "Update now" downloads `file` (checked against `sums`) where the method allows it.
    update::Method method = update::Method::page;
    std::optional<update::Asset> file, sums;
    bool downloading = false;
    std::shared_ptr<std::atomic<uint64_t>> done = std::make_shared<std::atomic<uint64_t>>(0),
                                           total = std::make_shared<std::atomic<uint64_t>>(0);
    std::string error;
  };
  std::optional<Update> update_;
  std::unique_ptr<Worker> update_worker_;  // the update download, off the library worker
  void start_update();
  bool update_checked_ = false;
  void check_update();
  void draw_update();
  // Saves a debug report about the game's last launch and uploads it.
  void share_report(std::string_view app_id);
  void upload_report();  // (again) for the report in report_
  std::mutex import_mutex_;            // picks from dialogs and drops
  std::vector<fs::path> import_picks_;
  void choose_import(bool folder);
  // Import save data: the game whose dialog is open, and folder picks for it.
  std::string save_import_target_;
  std::vector<std::pair<std::string, fs::path>> save_picks_;
  void choose_save_folder(const std::string &app_id);
  void draw_save_import();
  std::string save_game_;  // the game picked in the Import page's save data card
  // A game's mods folder as its page summarizes it, re-read every few seconds:
  // files replacing an installed file, and files matching none (another version).
  struct ModsSummary {
    double checked = -1;
    int active = 0, unused = 0;
    uint64_t bytes = 0;
    std::string name;
  };
  std::map<std::string, ModsSummary> mods_;
  const ModsSummary &mods_summary(const std::string &package, const fs::path &install);
  void draw_mods(const fs::path &install, const std::string &package, float width);
  void take_import_picks();
  void draw_import();
  void draw_import_items(ImportScan &scan, bool device, float width);
  void theme();
  void draw();
  void draw_library();
  void draw_details();
  void download_progress(const download::Job& job, float width);
  void draw_downloads();
  void draw_settings();
  void draw_account();
  void draw_uninstall();
  void draw_cancel_download();
  void draw_report();
  void draw_remove_account();
  void draw_install_ask();
  // The Install button: a new game asks where it goes and about Steam first,
  // unless installs do not ask or there is nothing to choose.
  void request_install(const meta::Game &game);
  void draw_addons(const meta::Game &game, float width);
  void draw_owner(const meta::Game &game, bool installed, float width);
  void smoke_install();
  void smoke_import();
  // Settings' text fields applied (launcher::Core::apply_settings).
  void save_settings(bool announce = true);
  void report_smoke();
  // Cover art (or a placeholder) filling `size` at the cursor; `zoom` > 0 crops
  // in.
  void art(std::string_view id, ImVec2 size, float rounding, float zoom = 0,
           ImDrawFlags corners = 0);
  void draw_sidebar();
  void draw_game_page();
  void draw_toast();
  void draw_chrome();
  void draw_about(const meta::Game &game, float width);
  void draw_backdrop(std::string_view id, ImVec2 a, ImVec2 b, float strength);
  float s(float value) const { return value * scale_; }
};
int run(Options options);
} // namespace venture::launcher
