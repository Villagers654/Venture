#include "vtest.h"
#include <android/log.h>
#include <cstdarg>
static __attribute__((noinline)) void log_v(const char* format, ...) {
    va_list args; va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "VTEST", format, args);
    va_end(args);
}
// Engine diagnostics use variadic and va_list entry points. Log routing and log
// filtering are runtime policy; return values may vary and are not asserted.
extern "C" int vtest_main(int, char**) {
    Vtest q("log");
    __android_log_print(ANDROID_LOG_INFO, "VTEST", "guest log %d %.2f", 42, 1.25);
    __android_log_write(ANDROID_LOG_WARN, "VTEST", "guest log write");
    log_v("guest log va_list %s %lld %.2f", "forward", 1234567890123LL, 2.5);
    q.step("log: print write vprint OK"); return q.finish();
}
