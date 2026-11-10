#pragma once
// Controls: which host controller input each Quest Touch input reaches, per host
// interaction profile. The launcher shows and edits it (Controls page, a game's
// Controls tab) and passes the player's choices to the runtime in
// VENTURE_CONTROLS; the runtime applies them where it suggests the game's Touch
// bindings (xr/bindings.cpp) and opens the game's menu (xr/menu_chord.cpp).
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::controls {

// The Quest inputs a player can move, per hand. Touch sensors follow the button
// they belong to (X touch follows X), so they are not listed separately.
enum class Quest : int { primary, secondary, menu, stick, stick_click, trigger, grip, thumbrest, count };
inline constexpr int kQuestCount = int(Quest::count);

struct QuestInput {
  Quest id;
  std::string_view left, right;            // label per hand ("" = none on that hand)
  std::string_view left_path, right_path;  // Touch component
};
inline constexpr std::array<QuestInput, kQuestCount> kQuestInputs{{
    {Quest::primary, "X", "A", "/input/x/click", "/input/a/click"},
    {Quest::secondary, "Y", "B", "/input/y/click", "/input/b/click"},
    {Quest::menu, "Menu", "", "/input/menu/click", ""},
    {Quest::stick, "Thumbstick", "Thumbstick", "/input/thumbstick", "/input/thumbstick"},
    {Quest::stick_click, "Stick click", "Stick click", "/input/thumbstick/click", "/input/thumbstick/click"},
    {Quest::trigger, "Trigger", "Trigger", "/input/trigger/value", "/input/trigger/value"},
    {Quest::grip, "Grip", "Grip", "/input/squeeze/value", "/input/squeeze/value"},
    {Quest::thumbrest, "Thumbrest", "Thumbrest", "/input/thumbrest/touch", "/input/thumbrest/touch"},
}};

// One physical input of a host controller.
struct HostInput {
  std::string_view path, label;
  bool reserved = false;  // the runtime keeps it (system / dashboard)
  int hands = 3;          // 1 left only, 2 right only, 3 both
};

// How the game's menu button is reached.
enum class MenuMode : int { automatic, button, hold, chord, off };

struct Profile {
  std::string_view path, name;
  std::string_view aliases;      // extra search words
  std::string_view art;          // bundled fallback picture (launcher/assets/controllers/<art>-<hand>.png), or ""
  std::string_view render_model; // installed SteamVR render model: "<name>_left/_right", or "<name>" for both hands
  std::span<const HostInput> inputs;
  // Default host input per Quest input and hand ("" = not assigned).
  std::array<std::array<std::string_view, kQuestCount>, 2> defaults;
  // Automatic menu: the controller's left menu input and its label, or "" where it
  // has none. Automatic also opens the menu with both stick clicks pressed together,
  // on every controller: some runtimes keep the menu button for themselves (SteamVR's
  // dashboard on Touch-shaped controllers), and one default must work on all of them.
  std::string_view menu_path, menu_label;
  // Identifiers with a /touch sensor ("a b trigger"): a Quest touch follows its
  // button only where the host has one.
  std::string_view touches = "";
  // The host extension that defines the profile on an OpenXR 1.0 instance ("" = core).
  std::string_view extension = "";
};

namespace detail {
using Row = std::array<std::string_view, kQuestCount>;
// The common Touch-shaped layout: face buttons, stick, trigger, grip, thumbrest. The
// menu is the profile's menu_path (the left menu button) plus both stick clicks.
inline constexpr Row touch_left{"/input/x/click", "/input/y/click", "", "/input/thumbstick",
                                "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value",
                                "/input/thumbrest/touch"};
inline constexpr Row touch_right{"/input/a/click", "/input/b/click", "", "/input/thumbstick",
                                 "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value",
                                 "/input/thumbrest/touch"};
inline constexpr Row face_left{"/input/x/click", "/input/y/click", "", "/input/thumbstick",
                               "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value", ""};
// HP Reverb G2: the face layout with its left menu button, which runtimes leave to games.
inline constexpr Row hp_left{"/input/x/click", "/input/y/click", "/input/menu/click", "/input/thumbstick",
                             "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value", ""};
inline constexpr Row face_right{"/input/a/click", "/input/b/click", "", "/input/thumbstick",
                                "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value", ""};
// Cosmos: the face layout with a grip that only clicks.
inline constexpr Row cosmos_left{"/input/x/click", "/input/y/click", "", "/input/thumbstick",
                                 "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/click", ""};
inline constexpr Row cosmos_right{"/input/a/click", "/input/b/click", "", "/input/thumbstick",
                                  "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/click", ""};
}  // namespace detail

inline constexpr HostInput kIndexInputs[]{
    {"/input/a/click", "A"},
    {"/input/b/click", "B"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trackpad", "Trackpad (swipe)"},
    {"/input/trackpad/force", "Trackpad press"},
    {"/input/trackpad/touch", "Trackpad touch"},
    {"/input/trigger/value", "Trigger"},
    {"/input/trigger/click", "Trigger click"},
    {"/input/squeeze/value", "Grip"},
    {"/input/squeeze/force", "Grip squeeze (hard)"},
    {"/input/system/click", "System", true},
};
inline constexpr HostInput kTouchInputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu", false, 1},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
    {"/input/thumbrest/touch", "Thumbrest"},
    {"/input/system/click", "Meta button", true, 2},
};
// Trackpad halves and center are XR_EXT_dpad_binding paths (SteamVR, Monado);
// without the extension they fall back to the whole trackpad click.
inline constexpr HostInput kViveInputs[]{
    {"/input/menu/click", "Menu"},
    {"/input/trackpad", "Trackpad"},
    {"/input/trackpad/click", "Trackpad click"},
    {"/input/trackpad/dpad_up", "Trackpad top"},
    {"/input/trackpad/dpad_down", "Trackpad bottom"},
    {"/input/trackpad/dpad_center", "Trackpad center"},
    {"/input/trackpad/touch", "Trackpad touch"},
    {"/input/trigger/value", "Trigger"},
    {"/input/trigger/click", "Trigger click"},
    {"/input/squeeze/click", "Grip"},
    {"/input/system/click", "System", true},
};
// Pico 4 (XR_BD_controller_interaction): the menu button is on the left only
// (the right controller has the Pico home button, the reserved system input).
inline constexpr HostInput kPico4Inputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu", false, 1},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
    {"/input/system/click", "System", true},
};
// Pico Neo 3 (XR_BD_controller_interaction): a menu button on each controller.
inline constexpr HostInput kPicoNeo3Inputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
    {"/input/system/click", "System", true},
};
inline constexpr HostInput kHpInputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
};
inline constexpr HostInput kFocus3Inputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu", false, 1},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
    {"/input/thumbrest/touch", "Thumbrest"},
    {"/input/system/click", "System", true, 2},
};
inline constexpr HostInput kCosmosInputs[]{
    {"/input/x/click", "X", false, 1},
    {"/input/y/click", "Y", false, 1},
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/menu/click", "Menu", false, 1},
    {"/input/shoulder/click", "Bumper"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/click", "Grip"},
    {"/input/system/click", "System", true, 2},
};
inline constexpr HostInput kWmrInputs[]{
    {"/input/menu/click", "Menu"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trackpad", "Trackpad"},
    {"/input/trackpad/click", "Trackpad click"},
    {"/input/trackpad/dpad_up", "Trackpad top"},
    {"/input/trackpad/dpad_down", "Trackpad bottom"},
    {"/input/trackpad/touch", "Trackpad touch"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/click", "Grip"},
    {"/input/system/click", "Windows button", true},
};
inline constexpr HostInput kSenseInputs[]{
    {"/input/square_mndx/click", "Square", false, 1},
    {"/input/triangle_mndx/click", "Triangle", false, 1},
    {"/input/cross_mndx/click", "Cross", false, 2},
    {"/input/circle_mndx/click", "Circle", false, 2},
    {"/input/share_mndx/click", "Create", false, 1},
    {"/input/options_mndx/click", "Options", false, 2},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/click", "Grip"},
    {"/input/system/click", "PS button", true},
};
inline constexpr HostInput kFrameInputs[]{
    {"/input/a/click", "A", false, 2},
    {"/input/b/click", "B", false, 2},
    {"/input/x/click", "X", false, 2},
    {"/input/y/click", "Y", false, 2},
    {"/input/dpad_up/click", "D-pad up", false, 1},
    {"/input/dpad_down/click", "D-pad down", false, 1},
    {"/input/dpad_left/click", "D-pad left", false, 1},
    {"/input/dpad_right/click", "D-pad right", false, 1},
    {"/input/view/click", "View", false, 1},
    {"/input/menu/click", "Menu", false, 2},
    {"/input/shoulder/click", "Bumper"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
    {"/input/system/click", "Steam button", true},
};
inline constexpr HostInput kGenericInputs[]{
    {"/input/primary/click", "Primary"},
    {"/input/secondary/click", "Secondary"},
    {"/input/thumbstick", "Thumbstick"},
    {"/input/thumbstick/click", "Thumbstick click"},
    {"/input/trigger/value", "Trigger"},
    {"/input/squeeze/value", "Grip"},
};

inline constexpr Profile kProfiles[]{
    {"/interaction_profiles/valve/index_controller", "Valve Index", "knuckles bigscreen beyond", "index", "valve_controller_knu_1_0",
     kIndexInputs,
     {{{"/input/a/click", "/input/b/click", "", "/input/thumbstick", "/input/thumbstick/click", "/input/trigger/value",
        "/input/squeeze/value", "/input/trackpad/touch"},
       {"/input/a/click", "/input/b/click", "", "/input/thumbstick", "/input/thumbstick/click", "/input/trigger/value",
        "/input/squeeze/value", "/input/trackpad/touch"}}},
     "/input/trackpad/force", "Left trackpad press", "a b trigger thumbstick trackpad"},
    {"/interaction_profiles/oculus/touch_controller", "Meta Quest Touch", "quest 2 oculus rift", "touch", "oculus_quest2_controller",
     kTouchInputs, {{detail::touch_left, detail::touch_right}}, "/input/menu/click", "Left menu button", "x y a b trigger thumbstick thumbrest", ""},
    {"/interaction_profiles/meta/touch_controller_plus", "Meta Quest Touch Plus", "quest 3 3s oculus", "touch", "oculus_quest_plus_controller",
     kTouchInputs, {{detail::touch_left, detail::touch_right}}, "/input/menu/click", "Left menu button", "x y a b trigger thumbstick thumbrest", "XR_META_touch_controller_plus"},
    {"/interaction_profiles/facebook/touch_controller_pro", "Meta Quest Touch Pro", "quest pro oculus", "touch", "oculus_quest_pro_controller",
     kTouchInputs, {{detail::touch_left, detail::touch_right}}, "/input/menu/click", "Left menu button", "x y a b trigger thumbstick thumbrest", "XR_FB_touch_controller_pro"},
    {"/interaction_profiles/htc/vive_controller", "HTC Vive wand", "vive pro wands", "vive", "vr_controller_vive_1_5", kViveInputs,
     {{{"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "/input/menu/click", "/input/trackpad",
        "/input/trackpad/dpad_center", "/input/trigger/value", "/input/squeeze/click", ""},
       {"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "", "/input/trackpad", "/input/trackpad/dpad_center",
        "/input/trigger/value", "/input/squeeze/click", ""}}},
     "/input/menu/click", "Left menu button", "trackpad"},
    {"/interaction_profiles/htc/vive_cosmos_controller", "HTC Vive Cosmos", "cosmos elite", "cosmos", "",
     kCosmosInputs, {{detail::cosmos_left, detail::cosmos_right}}, "/input/menu/click", "Left menu button", "thumbstick", "XR_HTC_vive_cosmos_controller_interaction"},
    {"/interaction_profiles/htc/vive_focus3_controller", "HTC Vive Focus 3", "focus vision xr elite", "focus3", "vive_focus3_controller",
     kFocus3Inputs, {{detail::touch_left, detail::touch_right}}, "/input/menu/click", "Left menu button", "trigger thumbstick thumbrest squeeze", "XR_HTC_vive_focus3_controller_interaction"},
    {"/interaction_profiles/hp/mixed_reality_controller", "HP Reverb G2", "wmr windows mixed reality", "wmr", "",
     kHpInputs, {{detail::hp_left, detail::face_right}}, "/input/menu/click", "Left menu button", "", "XR_EXT_hp_mixed_reality_controller"},
    {"/interaction_profiles/microsoft/motion_controller", "Windows Mixed Reality", "wmr samsung odyssey acer dell lenovo",
     "", "", kWmrInputs,
     {{{"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "/input/menu/click", "/input/thumbstick",
        "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/click", ""},
       {"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "", "/input/thumbstick", "/input/thumbstick/click",
        "/input/trigger/value", "/input/squeeze/click", ""}}},
     "/input/menu/click", "Left menu button", "trackpad"},
    {"/interaction_profiles/bytedance/pico4_controller", "Pico 4", "pico 4 ultra", "pico4", "pico_4_controller", kPico4Inputs,
     {{detail::face_left, detail::face_right}}, "/input/menu/click", "Left menu button",
     "x y a b trigger thumbstick", "XR_BD_controller_interaction"},
    {"/interaction_profiles/bytedance/pico_neo3_controller", "Pico Neo 3", "pico neo 3 link pro eye", "", "", kPicoNeo3Inputs,
     {{detail::face_left, detail::face_right}}, "/input/menu/click", "Left menu button",
     "x y a b trigger thumbstick", "XR_BD_controller_interaction"},
    {"/interaction_profiles/mndx/pssense_controller_mndx", "PlayStation VR2 Sense", "psvr2 ps5 sony", "",
     "playstation_vr2_sense", kSenseInputs,
     {{{"/input/square_mndx/click", "/input/triangle_mndx/click", "/input/share_mndx/click", "/input/thumbstick",
        "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/click", ""},
       {"/input/cross_mndx/click", "/input/circle_mndx/click", "", "/input/thumbstick", "/input/thumbstick/click",
        "/input/trigger/value", "/input/squeeze/click", ""}}},
     "/input/share_mndx/click", "Left Create button",
     "square_mndx triangle_mndx cross_mndx circle_mndx trigger thumbstick squeeze", "XR_MNDX_psvr2_interaction"},
    {"/interaction_profiles/valve/frame_controller_valve", "Steam Frame", "valve frame steam link", "",
     "frame_controller", kFrameInputs,
     {{{"/input/dpad_down/click", "/input/dpad_up/click", "/input/view/click", "/input/thumbstick",
        "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value", ""},
       {"/input/a/click", "/input/b/click", "", "/input/thumbstick", "/input/thumbstick/click",
        "/input/trigger/value", "/input/squeeze/value", ""}}},
     "/input/view/click", "Left View button", "thumbstick trigger", "XR_VALVE_frame_controller_interaction"},
    {"/interaction_profiles/khr/generic_controller", "Generic controller", "khr other unknown", "", "", kGenericInputs,
     {{{"/input/primary/click", "/input/secondary/click", "", "/input/thumbstick", "/input/thumbstick/click",
        "/input/trigger/value", "/input/squeeze/value", ""},
       {"/input/primary/click", "/input/secondary/click", "", "/input/thumbstick", "/input/thumbstick/click",
        "/input/trigger/value", "/input/squeeze/value", ""}}},
     "", "Both stick clicks", "", "XR_KHR_generic_controller"},
};

// ---- The player's choices -------------------------------------------------
// One controller's choices. An unset row keeps the default; an empty list leaves
// that Quest input without a host input. A row may list several host inputs: any
// of them presses the Quest input (Stick click on the stick and on a bumper).
// The default hold for one held button, and for two buttons together: a pair
// (both stick clicks by default) is deliberate enough to open the menu at once.
inline constexpr int kHoldMs = 500, kChordHoldMs = 0;
struct MenuChoice {
  MenuMode mode = MenuMode::automatic;
  // Hand-qualified host inputs ("/user/hand/right/input/b/click"): the button
  // (one button, hold), or the pair (two buttons).
  std::string first, second;
  // How long the button (hold) or the pair (two buttons) is held first; 0
  // presses the menu as soon as it is down. A saved value is kept as saved;
  // a saved choice without one gets its mode's default.
  int hold_ms = kHoldMs;
  bool operator==(const MenuChoice&) const = default;
};
// A row's host inputs (component paths), in the player's order.
using Inputs = std::vector<std::string>;
struct Choice {
  std::array<std::array<std::optional<Inputs>, kQuestCount>, 2> rows{};
  MenuChoice menu;
  bool operator==(const Choice&) const = default;
};
// Choices by interaction profile path; a profile without an entry uses the defaults.
using Config = std::map<std::string, Choice, std::less<>>;

const Profile* find_profile(std::string_view path);
// A host input of `profile` by component path ("/input/a/click"), or null.
const HostInput* find_input(const Profile& profile, std::string_view path);
inline bool on_hand(const HostInput& input, int hand) { return (input.hands >> hand) & 1; }
// A two-axis input: the stick row takes only these, the other rows never.
inline bool is_axes(std::string_view path) { return path.find('/', 7) == std::string_view::npos; }
std::string_view hand_root(int hand);

// How the game's menu is reached on a controller.
struct Menu {
  enum class Kind { off, direct, synthesized } kind = Kind::off;
  // direct: the left-hand host input the game's menu action is bound to.
  std::string direct;
  // One or two sources held for hold_ns (both together for two), merged into the
  // game's menu: a synthesized menu's inputs, and with automatic also beside a
  // direct one (both stick clicks). A source is pressed by any of its
  // hand-qualified host inputs: automatic's are the Stick click rows, as many
  // inputs as the player gave each; a chosen button is one.
  std::vector<Inputs> sources;
  std::int64_t hold_ns = 0;
  bool automatic = false;  // the controller's default: its menu input (if any) and both stick clicks
};
struct Resolved {
  // Component paths, none or several per row; duplicates never.
  std::array<std::array<Inputs, kQuestCount>, 2> rows{};
  Menu menu;
};
// The choices in effect: the defaults with `choice` (may be null) applied.
// Inputs the controller cannot take (another hand's button, a reserved input,
// a stick on a button) and repeats are dropped from a row; a row left with none
// of the player's inputs falls back to the default.
Resolved resolve(const Profile& profile, const Choice* choice);
// The host paths a game's Touch path reaches ("/user/hand/left/input/x/click" ->
// "/user/hand/left/input/trackpad/dpad_down"), one per host input of its row, or
// none. Touch sensors follow their button where the host has a sensor there.
// `dpad`: the host binds trackpad and stick halves (XR_EXT_dpad_binding); without
// it the first half on a pad, in row order (and within a row, in its order),
// gets the whole pad's press and the others stay unbound.
std::vector<std::string> host_bindings(std::string_view guest_path, const Profile& profile,
                                       const Resolved& resolved, bool dpad);
// A menu source or direct menu input as the host can bind it (the dpad fallback), or "".
std::string bindable(std::string_view hand_qualified, const Profile& profile, bool dpad);
// Two Quest inputs of one hand on one host input. A held menu shares on purpose
// (a short press still reaches the game) and is not listed.
struct Conflict {
  int hand;
  Quest a, b;
  bool operator==(const Conflict&) const = default;
};
std::vector<Conflict> conflicts(const Resolved& resolved, MenuMode mode);

// VENTURE_CONTROLS and settings.json: {"<profile>": {"left": {"primary": "/input/a/click", ...},
// "right": {...}, "menu": {"mode": "hold", "first": "...", "second": "...", "holdMs": 1000}}}.
// A row is a string ("" for none), or a list of strings when it has several
// inputs ("stickClick": ["/input/thumbstick/click", "/input/shoulder/click"]).
std::string encode(const Config& config);
// Malformed text or entries are ignored, never fatal.
Config decode(std::string_view text);
std::string_view quest_key(Quest q);
// Settings > Runtime's former "Menu from both stick clicks": 1 (always) puts the
// menu on both stick clicks pressed together on every controller; 2 (off) keeps each
// controller's own menu button with no stick-click fallback.
Config migrate_menu_button(int menu_button);

}  // namespace venture::controls
