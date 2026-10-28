#include "vtest.h"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>

// Config files and JSON numeric paths depend on end pointers and overflow errno.
extern "C" int vtest_main(int, char**) {
    Vtest q("parse");
    int integer = 0, consumed = 0; unsigned hex = 0; long long large = 0;
    double real = 0; char word[16] = {};
    CHECK(sscanf("-17 ff 1234567890123 1.25 quest!", "%d %x %lld %lf %5[a-z]%n",
                 &integer, &hex, &large, &real, word, &consumed) == 5);
    CHECK(integer == -17 && hex == 255 && large == 1234567890123LL && real == 1.25);
    CHECK(strcmp(word, "quest") == 0 && consumed == 31);
    CHECK(sscanf("bad", "%d", &integer) == 0);
    q.step("sscanf: widths scansets mixed values OK");
    char* end = nullptr;
    errno = 0; CHECK(strtol("  -0x2a tail", &end, 0) == -42 && strcmp(end, " tail") == 0 && errno == 0);
    const char invalid[] = "no-number";
    errno = 0; CHECK(strtol(invalid, &end, 10) == 0 && end == invalid && errno == 0);
    errno = 0; CHECK(strtol("999999999999999999999999", &end, 10) == LONG_MAX && errno == ERANGE);
    errno = 0; CHECK(strtoull("18446744073709551615!", &end, 10) == ULLONG_MAX && *end == '!' && errno == 0);
    errno = 0; CHECK(strtoull("18446744073709551616", &end, 10) == ULLONG_MAX && errno == ERANGE);
    errno = 0; CHECK(strtoull("-1", &end, 10) == ULLONG_MAX && errno == 0);
    q.step("integer parsing: bases limits errno OK");
    CHECK(strtod("0x1.8p+1tail", &end) == 3.0 && strcmp(end, "tail") == 0);
    CHECK(strtod("  -1.25e2!", &end) == -125.0 && *end == '!');
    CHECK(std::isnan(strtod("nan", &end)) && *end == 0);
    errno = 0; CHECK(std::isinf(strtod("1e9999", &end)) && errno == ERANGE);
    errno = 0; CHECK(strtod("1e-9999", &end) == 0.0 && errno == ERANGE);
    CHECK(strtof("2.5", &end) == 2.5f && *end == 0);
    q.step("floating parsing: hex special overflow underflow OK");
    return q.finish();
}
