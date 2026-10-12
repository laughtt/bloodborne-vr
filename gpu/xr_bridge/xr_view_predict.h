// Head and eye poses for a display time the runtime was not asked about (BbXrApi::GetViewsAt).
//
// Asking the runtime (xrLocateViews + xrLocateSpace) from the game thread took ~13 ms per call in
// the headset logs (every call over 1 ms, 40 calls/s): the runtime serialises those calls with the
// frame loop's xrWaitFrame / xrEndFrame. The frame loop already locates the views once per frame
// for the frame's own display time; two consecutive samples give the pose's velocity, and the pose
// at any later time is their extrapolation. No runtime call, no lock beyond the copy of the samples.
#pragma once
#include <cmath>
#include <cstdint>

namespace xrpredict {

struct Pose {
    float p[3];
    float q[4]; // x y z w
};

// The most the extrapolation goes beyond the second sample, in units of the sample spacing: the
// samples are a display period apart (14 ms), the game asks for up to ~60 ms ahead; a longer reach
// would amplify noise into a swing.
constexpr double kMaxFactor = 6.0;

inline float Dot4(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// Pose at time t from the poses a (at ta) and b (at tb > ta): position linear, orientation along the
// shortest arc (slerp with a factor past 1 extrapolates). t before ta or tb <= ta gives b.
inline Pose Extrapolate(const Pose& a, int64_t ta, const Pose& b, int64_t tb, int64_t t) {
    if (tb <= ta || t <= tb) {
        // between or before the samples: interpolate when inside, else the later sample
        if (tb <= ta || t <= ta) {
            return b;
        }
    }
    double r = double(t - ta) / double(tb - ta);
    if (r > 1.0 + kMaxFactor) {
        r = 1.0 + kMaxFactor;
    }
    Pose out;
    for (int i = 0; i < 3; ++i) {
        out.p[i] = float(double(a.p[i]) + (double(b.p[i]) - double(a.p[i])) * r);
    }
    float qb[4] = {b.q[0], b.q[1], b.q[2], b.q[3]};
    double d = 0.0;
    for (int i = 0; i < 4; ++i) d += double(a.q[i]) * double(qb[i]);
    if (d < 0.0f) { // the other hemisphere: same rotation, shorter way round
        for (float& c : qb) {
            c = -c;
        }
        d = -d;
    }
    double wa, wb;
    if (d > 0.9999999) { // the same rotation to ~0.03 deg: nlerp (extrapolating needs the arc, not the chord)
        wa = 1.0 - r;
        wb = r;
    } else {
        const double theta = std::acos(d);
        const double s = std::sin(theta);
        wa = std::sin((1.0 - r) * theta) / s;
        wb = std::sin(r * theta) / s;
    }
    double n = 0.0;
    for (int i = 0; i < 4; ++i) {
        out.q[i] = float(wa * double(a.q[i]) + wb * double(qb[i]));
        n += double(out.q[i]) * double(out.q[i]);
    }
    n = n > 0.0 ? 1.0 / std::sqrt(n) : 1.0;
    for (float& c : out.q) {
        c = float(double(c) * n);
    }
    return out;
}

} // namespace xrpredict
