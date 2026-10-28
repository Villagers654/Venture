#include "vtest.h"
#include <cerrno>
#include <cstdlib>
#include <sys/time.h>
#include <unistd.h>

// Engine clocks must advance and convert correctly; wall time is never printed.
extern "C" int vtest_main(int, char**) {
    Vtest q("time");
    for (clockid_t clock : {CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_BOOTTIME, CLOCK_THREAD_CPUTIME_ID}) {
        timespec a{}, b{}, resolution{};
        CHECK(clock_gettime(clock, &a) == 0 && clock_gettime(clock, &b) == 0);
        CHECK(vtest_timespec_valid(a) && vtest_timespec_valid(b));
        if (clock != CLOCK_REALTIME) CHECK(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
        CHECK(clock_getres(clock, &resolution) == 0 && vtest_timespec_valid(resolution));
    }
    timeval tv{}; CHECK(gettimeofday(&tv, nullptr) == 0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    time_t output = 0; time_t now = time(&output); CHECK(now != time_t(-1) && now == output);
    q.step("time: realtime monotonic boottime thread CPU OK");
    timespec before{}, after{}; CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    timespec interval{0, 1000000}, remaining{}; int result;
    do { result = nanosleep(&interval, &remaining); if (result < 0) interval = remaining; } while (result < 0 && errno == EINTR);
    CHECK(result == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    CHECK((int64_t(after.tv_sec) - before.tv_sec) * 1000000000 + after.tv_nsec - before.tv_nsec >= 1000000);
    do { result = usleep(1000); } while (result < 0 && errno == EINTR);
    CHECK(result == 0);
    q.step("time: nanosleep usleep OK");
    const char* old = getenv("TZ"); char* saved = old ? strdup(old) : nullptr;
    REQUIRE(!old || saved);
    REQUIRE(setenv("TZ", "UTC0", 1) == 0); tzset();
    time_t sample = 946684800; tm utc{}, local{};
    REQUIRE(gmtime_r(&sample, &utc) && localtime_r(&sample, &local));
    CHECK(utc.tm_year == 100 && utc.tm_mon == 0 && utc.tm_mday == 1 && utc.tm_wday == 6);
    CHECK(local.tm_year == 100 && local.tm_hour == 0 && local.tm_isdst == 0);
    char text[64];
    CHECK(strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 20);
    CHECK(strcmp(text, "2000-01-01T00:00:00Z") == 0);
    CHECK(mktime(&local) == sample);
    local.tm_mday = 2; CHECK(mktime(&local) == sample + 86400);
    if (!q.failure) printf("time: UTC %s conversions OK\n", text);
    if (saved) { CHECK(setenv("TZ", saved, 1) == 0); free(saved); } else CHECK(unsetenv("TZ") == 0);
    tzset(); return q.finish();
}
