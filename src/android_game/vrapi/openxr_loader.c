// SPDX-License-Identifier: GPL-3.0-or-later
// libopenxr_loader.so for games Venture installs on Android headsets: the
// pinned Khronos loader, linked in whole, which finds the headset's own
// runtime through the Android loader interface. Its xr* entry points are the
// library's exports (openxr_loader.map); this file adds nothing.
int venture_openxr_loader(void);
int venture_openxr_loader(void) { return 0; }
