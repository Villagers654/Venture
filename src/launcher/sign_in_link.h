#pragma once
// Manual Meta sign-in from the user's own browser: while it waits, Venture is
// the user's handler for oculus:// links, so Meta's Continue button hands the
// address back instead of the user copying it from a console. The browser
// starts `venture --sign-in-callback <data folder> <address>`, which leaves
// the address there for the waiting sign-in and exits.
#include "common/paths.h"
#include <string>
#include <string_view>

namespace venture::launcher::sign_in_link {
// Makes this program the oculus:// handler until restore(); false when the
// system has no place for one (the Flatpak sandbox, or no xdg-mime).
bool take_over(const fs::path &data);
// Puts back the handler there was before take_over().
void restore(const fs::path &data);
// The browser's launch: keeps the address for the waiting sign-in. Exit code.
int deliver(const fs::path &data, std::string_view address);
// The address a browser handed over since the last call; empty if none.
std::string receive(const fs::path &data);
#ifdef _WIN32
// Tests: the HKEY_CURRENT_USER subkey that stands for Software\Classes.
void set_classes_key_for_tests(std::wstring key);
#endif
} // namespace venture::launcher::sign_in_link
