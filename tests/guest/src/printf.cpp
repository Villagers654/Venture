#include "vtest.h"
#include <cstdarg>
#include <cstdlib>

// HLE must marshal mixed integer/FP varargs and the guest ABI's va_list layout.
static __attribute__((noinline)) int forward(char* out, size_t size, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int result = vsnprintf(out, size, fmt, copy);
    va_end(copy);
    return result;
}
static __attribute__((noinline)) int helper(char* out, size_t size, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    int result = forward(out, size, fmt, args);
    va_end(args); return result;
}
extern "C" int vtest_main(int, char**) {
    Vtest q("printf"); char out[512];
    int n = snprintf(out, sizeof(out), "%d %u %lld %llu %zu %#x %#o", -42, 42u,
                     -1234567890123LL, 18446744073709551615ULL, size_t(12345), 42u, 9u);
    CHECK(n > 0 && strcmp(out, "-42 42 -1234567890123 18446744073709551615 12345 0x2a 011") == 0);
    if (!q.failure) printf("integers: %s\n", out);
    snprintf(out, sizeof(out), "[%+06d][%-11s][%8.3f][%*.*f]", 42, "venture", 1.25, 6, 2, 2.5);
    CHECK(strcmp(out, "[+00042][venture    ][   1.250][  2.50]") == 0);
    if (!q.failure) printf("padding: %s\n", out);
    snprintf(out, sizeof(out), "%.2f %.3e %.3E %.6g %.6G %a", 1.25, 12.5, 12.5, 123.5, 123.5, 1.5);
    CHECK(strcmp(out, "1.25 1.250e+01 1.250E+01 123.5 123.5 0x1.8p+0") == 0);
    if (!q.failure) printf("floating: %s\n", out);
    snprintf(out, sizeof(out), "%.4g %.4g %A %+.1f", 0.0001234, 0.00001234, 1.5, -0.0);
    CHECK(strcmp(out, "0.0001234 1.234e-05 0X1.8P+0 -0.0") == 0);
    if (!q.failure) {
        n = printf("direct: %d %lld %zu %.2f %.1f\n", -7, 9876543210LL, size_t(8), 1.25, double(2.5f));
        CHECK(n == 33);
    }
    const char* positional = "%2$d/%1$d/%3$.2f";
    snprintf(out, sizeof(out), positional, 11, 22, 1.25);
    CHECK(strcmp(out, "22/11/1.25") == 0);
    snprintf(out, sizeof(out), "%p", static_cast<void*>(nullptr));
    CHECK(strcmp(out, "0x0") == 0 || strcmp(out, "(nil)") == 0);
    q.step("positional and null pointer OK");
    n = helper(out, sizeof(out), "%s %d %lld %.2f %.1f %.0f %.3f %zu", "guest", -7,
               9876543210LL, 1.25, double(2.5f), 3.0, 0.125, size_t(8));
    CHECK(n == 38 && strcmp(out, "guest -7 9876543210 1.25 2.5 3 0.125 8") == 0);
    // Also overflow the AAPCS64 integer and SIMD argument registers onto stack.
    helper(out, sizeof(out), "%d%d%d%d%d%d%d%d%d:%.0f%.0f%.0f%.0f%.0f%.0f%.0f%.0f%.0f",
           1,2,3,4,5,6,7,8,9,1.,2.,3.,4.,5.,6.,7.,8.,9.);
    CHECK(strcmp(out, "123456789:123456789") == 0);
    char shortbuf[5] = {};
    CHECK(helper(shortbuf, sizeof(shortbuf), "%s", "abcdef") == 6 && strcmp(shortbuf, "abcd") == 0);
    CHECK(snprintf(nullptr, 0, "%d:%s", 123, "venture") == 11);
    q.step("va_list forwarding and truncation OK");
    return q.finish();
}
