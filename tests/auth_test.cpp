// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/meta/auth.h"
#include <doctest/doctest.h>
using venture::meta::find_callback;

TEST_CASE("a pasted sign-in address is found in a console message") {
  CHECK(find_callback("oculus://login?token=abc&blob=def") == "oculus://login?token=abc&blob=def");
  CHECK(find_callback("  oculus://login?token=abc&blob=def\n") == "oculus://login?token=abc&blob=def");
  // Firefox: Prevented navigation to “oculus://…” due to an unknown protocol.
  CHECK(find_callback("Prevented navigation to \xE2\x80\x9C" "oculus://login?token=abc&blob=d%2Bf\xE2\x80\x9D due to an "
                      "unknown protocol.") == "oculus://login?token=abc&blob=d%2Bf");
  CHECK(find_callback("Prevented navigation to \"OCULUS://login?token=a\" due") == "OCULUS://login?token=a");
  CHECK(find_callback("oculus-client://x?y=1 trailing") == "oculus-client://x?y=1");
  CHECK(find_callback("<oculus://x?y=1>") == "oculus://x?y=1");
  CHECK(find_callback("https://auth.meta.com/native_sso/confirm").empty());
  CHECK(find_callback("").empty());
}

TEST_CASE("a sign-in only accepts its own address") {
  venture::meta::Auth auth;  // no request yet, so nothing answers it
  CHECK_FALSE(auth.accepts("oculus://login?token=abc&blob=def"));
  CHECK_FALSE(auth.accepts(""));
}
