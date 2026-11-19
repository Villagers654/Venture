// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for Meta's libvrapi.so and libovrplatformloader.so in the test
// game's APK (tests/android/test_game/game.c). Venture replaces both when it
// adapts the game. If this copy is loaded, the game's log shows the missing
// symbols.
int venture_test_game_stub;
