// SPDX-License-Identifier: GPL-3.0-or-later
// The Platform loader under another name (docs/design/native-headsets.md 5.3).
// Some SDKs ship Meta's loader renamed; the adapter writes this library under
// each such name, its soname set to that name (game_adapter.h, named_alias).
// It has no code of its own: it needs libovrplatformloader.so, so a game that
// loads it, links against it or looks up symbols through it reaches the one
// Platform instance Venture's loader holds.
int venture_platform_alias(void);
int venture_platform_alias(void) { return 0; }
