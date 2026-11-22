#pragma once
// The launcher window: an ImGui view over launcher::Core (launcher/core.h).
#include "launcher/assets/controller_art.h"
#include "launcher/assets/texture_cache.h"
#include "launcher/core.h"
#include "launcher/drives.h"
#include "launcher/updater.h"
#include "launcher/ui/chrome.h"
#include <imgui.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <array>
#include "common/controls.h"
#include "common/openxr_layers.h"
#include "common/library/mods.h"
#include <map>
#include <memory>
namespace venture::launcher {
// Controls: which host controller input each Quest input reaches, for one controller
// (common/controls.h); saved in Settings::controls and Settings::game_controls.
struct ControlsEdit {
  int profile = 0;     // controls::kProfiles index
  int menu_mode = 0;   // controls::MenuMode
  int menu_input = 0;                 // one button / hold: hand-prefixed input (1..2n, left first)
  int menu_first = 0, menu_second = 0;  // two buttons: hand-prefixed inputs
  int menu_hold_ms = controls::kHoldMs;         // hold: how long
  int menu_chord_ms = controls::kChordHoldMs;   // two buttons: how long both are held (0: at once)
  // Per Quest input and hand: the host inputs (index + 1) in the player's order; empty: not assigned.
  std::array<std::array<std::vector<int>, controls::kQuestCount>, 2> host{};
  bool grip_toggle = false;  // each grip press flips the game's grip held / released
  std::array<controls::Offset, 2> offsets{};  // where the game sees each controller in the hand
  bool mirror = true;  // the right offset follows the left, mirrored
  std::array<controls::Tuning, 2> tuning{};  // sticks, press points and vibration per hand
  bool tuning_mirror = true;  // the right hand's tuning follows the left
  std::array<bool, controls::kQuestCount> toggles{};  // per Quest row (not the grip): presses latch
  bool custom = false;  // per game: overrides the defaults
  bool initialized = false;
};
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
  void on_art_changed(std::string_view id) override;
  void tick() override;
  SDL_Window *window_ = nullptr;
  SDL_Renderer *renderer_ = nullptr;
  uint32_t wake_event_ = 0;
  std::unique_ptr<TextureCache> textures_;
  std::set<std::string> addons_expanded_;      // games showing unowned add-ons
  bool advanced_confirmed_ = false;            // the troubleshooting dialog is past its warning
  std::optional<std::string> remove_account_;  // pending removal confirmation
  bool remove_saves_ = false;
  // The install dialog: where the game goes (free bytes there), Steam, and "don't ask again".
  std::vector<std::pair<launcher::InstallLocation, uint64_t>> install_choices_;
  int install_choice_ = 0;
  // The import dialog, as for installs: the scan it imports (true: the headset),
  // where new games go, Steam, and "don't ask again".
  std::optional<bool> import_ask_;
  std::vector<std::pair<launcher::InstallLocation, uint64_t>> import_choices_;
  int import_choice_ = 0;
  bool install_steam_ = true, install_remember_ = false;
  // The install dialog picks a version (or every one) to install beside the others.
  bool install_versions_ = false;
  int version_choice_ = 0;
  bool import_steam_ = true, import_remember_ = false;
  std::string direct_app_;
  bool direct_started_ = false;
  std::string backdrop_; // game whose ambient art tints the game page
  std::string search_, settings_games_, settings_adb_;
  // Settings > Runtime color key: the hex field's text, and Custom chosen
  // although the color matches a preset.
  std::string settings_key_hex_;
  bool key_custom_ = false;
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
  std::map<int, SDL_Texture *> hearts_; // by pixel size; negative: the outlined heart
  SDL_Texture *heart(int pixels, bool outline = false);
  std::map<const art::Image *, SDL_Texture *> controller_textures_;
  SDL_Texture *controller_texture(const art::Image *image);
  // Turned controllers: each picture's turn while the page stays open on one
  // controller, its newest drawing and texture.
  std::map<const art::Image *, art::Turn> controller_turns_;
  int controller_turns_frame_ = -1;  // the frame an editor last showed them
  int controller_turns_profile_ = -1;
  struct TurnedTexture {
    std::shared_ptr<const art::Shot> shot;
    SDL_Texture *texture = nullptr;
  };
  std::map<const art::Image *, TurnedTexture> turned_textures_;
  SDL_Texture *turned_texture(const art::Image *image, const std::shared_ptr<const art::Shot> &shot);
  bool quit_ = false;
  ImVec2 card_menu_at_{};  // where a library card's menu opens
  std::string smoke_hovered_;  // --smoke-view library-hover: the card drawn hovered
  // Display settings rows, for Settings (global) or a game's Settings tab;
  // a change is saved at once.
  void draw_display_rows(library::DisplaySettings& display, bool per_game, const char* id);
  bool display_restart_note_ = false;  // a Display setting changed while a game runs
  // A game's texture compression rows (its Settings tab, beside Settings > Runtime's).
  void draw_game_runtime_rows(const std::string& id);
  void draw_game_advanced(const std::string& id, float width);
  // A Compress textures change that turns compression down from Always (globally,
  // or for a game whose setting was Always), waiting on its warning: applied on
  // "Change anyway", dropped on Cancel.
  std::function<void()> texture_warning_;
  void draw_texture_warning();
  // Play on a 2D Android app while this copy lacks the Android 2D component
  // (android_2d_download): asks first, downloads it in the dialog, then plays.
  struct Android2dAsk {
    std::string id, key, name;
    std::uint64_t size = 0;
    struct Transfer {
      std::atomic<std::uint64_t> done{0}, total{0};
      std::atomic<bool> finished{false};
      std::mutex mutex;
      std::string error;  // under mutex, once finished
      std::jthread thread;
    };
    std::shared_ptr<Transfer> transfer;
  };
  std::optional<Android2dAsk> android_2d_ask_;
  // Play, or the Android 2D component's question first.
  void request_play(const meta::Game& game);
  void draw_android_2d();
  // Settings > Updates: choosing Experimental asks first; Stable switches at once.
  bool experimental_switch_ = false;
  void draw_experimental_switch();
  void set_update_channel(bool experimental);
  // Flatpak with SteamVR as the OpenXR runtime and its folder not in the
  // sandbox (checked at start): told once, with the flatpak override command
  // that grants the folder (copyable).
  bool flatpak_steamvr_warning_ = false;
  std::string flatpak_steamvr_folder_;
  void draw_flatpak_steamvr();
  // The host display of the last game launch (host-display.json), re-read when it changes.
  const library::HostDisplay* host_display();
  std::optional<library::HostDisplay> host_display_;
  fs::file_time_type host_display_time_{};
  double host_display_checked_ = -10;
  // Settings > VR runtime: the installed OpenXR runtimes, re-read every few seconds.
  std::vector<openxr_layers::Runtime> xr_runtimes_;
  double xr_runtimes_checked_ = -10;
  void draw_game_settings(const meta::Game& g, float width);
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
  bool smoke_debug_reported_ = false;  // --smoke-debug-report written
  bool smoke_dropped_ = false;  // --smoke-drop files taken, once the library is read
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
  // The headset check's text while its dialog is open (Android).
  std::optional<std::string> headset_check_;
  bool headset_check_copied_ = false;
  void draw_headset_check();
  // A newer published release than this build, offered once per version.
  struct Update {
    std::string version, url;
    // "Update now" downloads `file` (checked against `sums`) where the method allows it.
    update::Method method = update::Method::page;
    std::optional<update::Asset> file, sums;
    bool downloading = false;
    // An older release Android would refuse over this APK: offered by its page.
    bool android_downgrade = false;
    std::shared_ptr<std::atomic<uint64_t>> done = std::make_shared<std::atomic<uint64_t>>(0),
                                           total = std::make_shared<std::atomic<uint64_t>>(0);
    std::string error;
  };
  std::optional<Update> update_;
  std::unique_ptr<Worker> update_worker_;  // the update download, off the library worker
  void start_update();
  bool update_checked_ = false;
  bool os_sdk_checked_ = false;
  void check_update();
  void check_os_sdk();
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
  // Mods: a dropped file goes to its game's mods (add_mod_file), else to the
  // import page; "Add mods" on a game's Mods tab picks files for that game.
  void take_dropped(const fs::path &file);
  // Mods, or with `obb` the file to write over that expansion file.
  void choose_mod_file(const std::string &app_id, const std::string &obb = {});
  struct ModPick {
    std::string id, obb;
    fs::path file;
  };
  std::vector<ModPick> mod_picks_;  // under import_mutex_
  // The Mods tab: what it shows of the game's mods, read again after a change
  // (mods_generation_), for another game, or every few seconds.
  void draw_game_mods(const meta::Game &game, float width);
  struct ModsView {
    std::string game;
    std::uint64_t generation = ~0ull;
    double read_at = -10;
    std::vector<mods::Installed> mods;
    std::vector<mods::Item> items;
    std::optional<mods::Loader> apk_loader;
    std::vector<std::string> obbs;  // the install's expansion files
    bool loader_present = false, original_apk = false;
  };
  ModsView mods_view_;
  // Controls files: a dialog to read one into, or save the shown controller's
  // controls from, the Controls page or a game's Controls tab (`game`: "" for the page).
  void choose_controls_file(bool save, const std::string &game);
  void take_controls_files();
  void import_controls(const fs::path &file, const std::string &game);
  void export_controls(const fs::path &file, const std::string &game);
  struct ControlsFilePick {
    bool save = false;
    std::string game;
    fs::path file;
  };
  std::vector<ControlsFilePick> controls_file_picks_;  // under import_mutex_
  int controls_drawn_frame_ = -10;  // the last frame a Controls editor showed (drops import there)
  std::string controls_drawn_game_;
  void draw_save_import();
  // Export save data: confirm before sending to headset.
  std::string save_export_target_;
  void draw_save_export();
  // Link to store page...: the import whose dialog is open and the pasted link.
  std::string store_link_target_, store_link_text_;
  void draw_store_link();
  std::string save_game_;  // the game picked in the Import page's save data card
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
  ControlsEdit controls_, game_controls_;
  int controls_hover_ = -1;  // hand * kQuestCount + Quest input under the pointer
  // The extra rows (stick, button and trigger touches, stick directions) added with "Add input" while
  // unassigned, per hand; an assigned one always shows.
  std::array<std::array<bool, controls::kQuestCount>, 2> controls_extra_added_{};
  int controls_active_ = -1;  // the row being changed (its list open or focused), last frame
  // A row line whose list opens next frame (an input just added): hand, Quest input, line.
  std::optional<std::array<int, 3>> controls_open_line_;
  void draw_controls();
  void draw_controls_editor(ControlsEdit& edit, const ControlsEdit* base, float width, const meta::Game* game);
  void draw_controller_picker(ControlsEdit& edit, float width, const controls::Config& config);
  float controls_file_buttons_width() const;
  void draw_controls_file_buttons(const std::string& game);
  void draw_game_controls(const meta::Game& game, float width);
  void read_controller_report();
  const art::Image* controller_image(const controls::Profile& profile, int hand);
  std::string controls_search_;
  int controls_connected_ = -1, controls_last_used_ = -1;  // kProfiles index the runtime reports now / reported last
  int controls_reported_ = -1;
  std::int64_t controls_reported_at_ = 0;  // unix seconds of that report
  double controls_report_check_ = 0;
  fs::file_time_type controls_report_stamp_{};
  std::string controls_game_;  // the game game_controls_ edits
  int controls_menu_detail_ = 0, controls_menu_kind_ = 0;
  bool controls_menu_panel_ = false;  // the menu's hold / two-button popover is open
  void draw_language_row();
  void draw_account();
  void manual_sign_in();  // the account page's "Sign in manually" section
  void draw_uninstall();
  void draw_cancel_download();
  void draw_report();
  void draw_remove_account();
  void draw_install_ask();
  void draw_import_ask();
  void request_import(bool device);
  // The Install button: a new game asks where it goes and about Steam first,
  // unless installs do not ask or there is nothing to choose.
  void request_install(const meta::Game &game);
  void request_versions(const meta::Game &game);
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
