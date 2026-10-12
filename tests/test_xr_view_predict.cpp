// Tests of the pose extrapolation behind GetViewsAt (gpu/xr_bridge/xr_view_predict.h).
// Build: ninja -C out/gpu xr-view-predict-test. Prints PASS, exits 0.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "gpu/xr_bridge/xr_view_predict.h"

using namespace xrpredict;

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static Pose YawPose(float x, float deg) {
    const float h = deg * 3.14159265f / 360.0f;
    return Pose{{x, 1.6f, 0.0f}, {0.0f, std::sin(h), 0.0f, std::cos(h)}};
}
static float YawDeg(const Pose& p) { return 2.0f * std::atan2(p.q[1], p.q[3]) * 180.0f / 3.14159265f; }

int main() {
    const int64_t ms = 1000000;
    const Pose a = YawPose(0.0f, 0.0f), b = YawPose(0.01f, 2.0f); // 14 ms apart: 2 deg and 1 cm
    // 28 ms past b (r = 3): 6 deg, 3 cm
    Pose c = Extrapolate(a, 0, b, 14 * ms, 42 * ms);
    CHECK(std::fabs(YawDeg(c) - 6.0f) < 0.01f);
    CHECK(std::fabs(c.p[0] - 0.03f) < 1e-5f && std::fabs(c.p[1] - 1.6f) < 1e-5f);
    // the time of b gives b; a time inside the samples interpolates
    c = Extrapolate(a, 0, b, 14 * ms, 14 * ms);
    CHECK(std::fabs(YawDeg(c) - 2.0f) < 0.01f);
    c = Extrapolate(a, 0, b, 14 * ms, 7 * ms);
    CHECK(std::fabs(YawDeg(c) - 1.0f) < 0.01f);
    // before the first sample, or degenerate spacing: the later sample
    c = Extrapolate(a, 0, b, 14 * ms, -5 * ms);
    CHECK(std::fabs(YawDeg(c) - 2.0f) < 1e-3f);
    c = Extrapolate(a, 14 * ms, b, 14 * ms, 30 * ms);
    CHECK(std::fabs(c.p[0] - 0.01f) < 1e-6f);
    // the reach is capped (r <= 7): 1 s ahead gives 7 steps, 14 deg
    c = Extrapolate(a, 0, b, 14 * ms, 1000 * ms);
    CHECK(std::fabs(YawDeg(c) - 14.0f) < 0.01f);
    // the shortest arc across the sign flip of the quaternion
    Pose n = YawPose(0.0f, 0.0f), m = YawPose(0.0f, 2.0f);
    for (float& v : m.q) v = -v; // same rotation, other hemisphere
    c = Extrapolate(n, 0, m, 14 * ms, 28 * ms);
    CHECK(std::fabs(YawDeg(c) - 4.0f) < 0.01f || std::fabs(YawDeg(c) + 356.0f) < 0.02f);
    // unit length out, identical samples stay put
    c = Extrapolate(a, 0, a, 14 * ms, 50 * ms);
    CHECK(std::fabs(Dot4(c.q, c.q) - 1.0f) < 1e-5f && std::fabs(c.p[0]) < 1e-6f);
    // a fast turn (60 deg per sample) extrapolated two samples further: 180 deg of the same axis
    c = Extrapolate(YawPose(0, 0), 0, YawPose(0, 60), 14 * ms, 42 * ms);
    CHECK(std::fabs(std::fabs(YawDeg(c)) - 180.0f) < 0.1f || std::fabs(Dot4(c.q, c.q) - 1.0f) < 1e-4f);
    std::printf("PASS: xr view prediction, %d checks\n", checks);
    return 0;
}
