#include "vtest.h"
#include <cmath>
#include <cfenv>
#include <limits>

// Physics and animation require numerical accuracy, including fused operations.
// All reference inputs are positive; bit distance then equals ULP distance.
static bool ulp(double actual, double expected, uint64_t tolerance = 4) {
    uint64_t a, b; memcpy(&a, &actual, 8); memcpy(&b, &expected, 8);
    return std::isfinite(actual) && (a > b ? a - b : b - a) <= tolerance;
}
static bool ulpf(float actual, float expected, uint32_t tolerance = 4) {
    uint32_t a, b; memcpy(&a, &actual, 4); memcpy(&b, &expected, 4);
    return std::isfinite(actual) && (a > b ? a - b : b - a) <= tolerance;
}
extern "C" int vtest_main(int, char**) {
    Vtest q("libm");
    CHECK(ulp(sin(0.5), 0.479425538604203));
    CHECK(ulp(cos(0.5), 0.8775825618903728));
    CHECK(ulp(tan(0.5), 0.5463024898437905));
    CHECK(ulp(exp(0.5), 1.6487212707001282));
    CHECK(ulp(log(2.0), 0.6931471805599453));
    CHECK(ulp(pow(2.0, 0.5), 1.4142135623730951));
    CHECK(ulp(sqrt(2.0), 1.4142135623730951));
    CHECK(fma(0x1.0000000000001p0, 0x1.ffffffffffffep-1, -1.0) == -0x1p-104);
    CHECK(fmod(5.5, 2.0) == 1.5 && remainder(5.5, 2.0) == -0.5);
    q.step("double: transcendental fused remainder ULP checks OK");
    int exponent = 0; double integral = 0;
    CHECK(frexp(12.0, &exponent) == 0.75 && exponent == 4);
    CHECK(ldexp(0.75, 4) == 12.0 && modf(3.25, &integral) == 0.25 && integral == 3.0);
    int original = fegetround(); CHECK(fesetround(FE_TONEAREST) == 0);
    CHECK(round(2.5) == 3.0 && round(-2.5) == -3.0);
    CHECK(lrint(2.5) == 2 && lrint(3.5) == 4 && lrint(-2.5) == -2);
    CHECK(fesetround(original) == 0);
    CHECK(std::signbit(sqrt(-0.0)) && std::isnan(sqrt(-1.0)));
    q.step("double: decomposition rounding signed zero OK");
    CHECK(ulpf(sinf(0.5f), 0.4794255386f)); CHECK(ulpf(cosf(0.5f), 0.8775825619f));
    CHECK(ulpf(tanf(0.5f), 0.5463024898f)); CHECK(ulpf(expf(0.5f), 1.6487212707f));
    CHECK(ulpf(logf(2.0f), 0.6931471806f)); CHECK(ulpf(powf(2.0f, 0.5f), 1.4142135624f));
    CHECK(ulpf(sqrtf(2.0f), 1.4142135624f));
    CHECK(fmaf(0x1.000002p0f, 0x1.fffffcp-1f, -1.f) == -0x1p-46f);
    CHECK(fmodf(5.5f, 2.f) == 1.5f && remainderf(5.5f, 2.f) == -0.5f);
    float intf = 0; CHECK(modff(3.25f, &intf) == 0.25f && intf == 3.f);
    CHECK(frexpf(12.f, &exponent) == 0.75f && exponent == 4 && ldexpf(0.75f, 4) == 12.f);
    CHECK(roundf(-2.5f) == -3.f && lrintf(3.f) == 3);
    q.step("float: transcendental fused decomposition ULP checks OK");
    return q.finish();
}
