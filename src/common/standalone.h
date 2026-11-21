#pragma once
// Whether Venture runs on a standalone headset: the headset's own panels are
// the only displays this machine drives, so there is no separate monitor the
// player looks at (the Steam Frame and other headsets running Linux). CPU
// architecture says nothing about it (Arm laptops and desktops exist), so the
// decision comes from the displays: every connected display connector is a
// DSI panel, the internal link headsets and phones use. A laptop's own panel
// is eDP or LVDS, so a laptop (Arm or x86) is never taken for a headset, and a
// machine with any external monitor never is. With no display connector
// visible at all the answer is no.
//
// VENTURE_STANDALONE=1 or 0 overrides the detection. Windows is never
// standalone; the Android build always runs on the headset itself.
#include <string>
#include <vector>

namespace venture::standalone {
struct Connector {
  std::string name;  // DRM connector name without the card prefix: "DSI-1", "eDP-1", "HDMI-A-1"
  bool connected = false;
};
struct Decision {
  bool standalone = false;
  std::string reason;  // one line for logs and the debug report
};
// The rule above, on connectors already read (testable without a device).
Decision classify(const std::vector<Connector>& connectors);
// The rule applied to this machine (the override first), worked out once.
const Decision& detect();
}  // namespace venture::standalone
