// SPDX-License-Identifier: GPL-2.0-or-later
// VR hybrid stereo warp (vk_stereo_warp.h, host_shaders/stereo_warp.comp): a synthetic scene
// rendered for both eyes the way the vertex shift does it (clip.x - P00 * m), then eye R built
// from eye L and eye R's depth. Checks the disparity formula and its sign, the identity at zero
// eye distance, the sub-pixel (two-tap) sampling, the disocclusion fill from the background and
// its G-buffer-guided colour (eye R's albedo times the background's lighting ratio).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/vk_stereo_warp.h"

namespace {

using namespace Vulkan::StereoWarp;

constexpr int W = 480, H = 5;
constexpr float P00 = 1.42799f, Near = 0.0500679f, Far = 3000.0f; // the game's projection
constexpr float Zs = Far / (Far - Near), Zo = -Near * Far / (Far - Near);

struct Eye {
    std::vector<float> depth;   // hardware depth, W x H
    std::vector<int> surface;   // 0 background wall, 1 foreground post, 2 far (cleared)
    std::vector<float> world_x; // the point's world x (what its color would be)
};

/// The scene seen by the eye at m metres along the camera's right axis: a post 0.4 m wide
/// at 1.5 m in front of a wall at 10 m, the wall ending at x = 3 (cleared depth past it).
/// `jx`: the DLSS jitter, a viewport shift in pixels (both eyes get the same one: the G-buffer
/// pass is recorded once and executed twice), so pixel x samples the scene at x + 0.5 - jx.
Eye Render(float m, float jx = 0.0f) {
    Eye eye{std::vector<float>(W * H), std::vector<int>(W * H), std::vector<float>(W * H)};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // Pixel center -> NDC; the shifted clip x = P00 * (X - m) means the eye's ray
            // through the pixel reaches world x = ndc * z / P00 + m at depth z.
            const float ndc = (float(x) + 0.5f - jx) / W * 2.0f - 1.0f;
            int surface = 2;
            float z = Far, wx = 0.0f;
            const float post_x = ndc * 1.5f / P00 + m;
            const float wall_x = ndc * 10.0f / P00 + m;
            if (std::abs(post_x) <= 0.2f) {
                surface = 1, z = 1.5f, wx = post_x;
            } else if (wall_x <= 3.0f) {
                surface = 0, z = 10.0f, wx = wall_x;
            }
            const int i = y * W + x;
            eye.depth[i] = surface == 2 ? 1.0f : Zs + Zo / z;
            eye.surface[i] = surface;
            eye.world_x[i] = wx;
        }
    }
    return eye;
}

Params Make(float shift) {
    Params p{};
    p.width = W, p.height = H, p.depth_width = W, p.depth_height = H;
    p.shift = shift, p.zs = Zs, p.zo = Zo;
    p.tolerance_rel = TolRel, p.tolerance_abs = TolAbs, p.tolerance_px = TolPx;
    p.search = SearchRadius(shift);
    return p;
}

struct Result {
    int matched = 0, filled = 0, sign_ok = 0, shifted = 0, bilinear = 0;
    double subpixel_err = 0.0; // mean |world x error| / footprint over two-tap pixels
    double unjittered_err = 0.0; // the same against eye R rendered without the jitter
};

/// Eye R from eye L: every pixel whose surface eye L sees must get that surface's point (two
/// taps: interpolated to well within a pixel's footprint; one tap: within one footprint);
/// every other one the background behind the uncovered region.
Result Check(float eye_l, float eye_r, float jx = 0.0f) {
    const Eye left = Render(eye_l, jx), right = Render(eye_r, jx), right0 = Render(eye_r);
    const Params p = Make(Shift(P00, eye_r, eye_l, W));
    auto depth_l = [&](int x, int y) { return left.depth[y * W + x]; };
    auto depth_r = [&](int x, int y) { return right.depth[y * W + x]; };
    Result result;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            const Sample s = WarpPixel(x, y, p, depth_l, depth_r);
            assert(s.count >= 1 && s.count <= 3);
            float wsum = 0.0f, wx = 0.0f, src = 0.0f;
            for (int t = 0; t < s.count; ++t) {
                assert(s.column[t] >= 0 && s.column[t] < W);
                wsum += s.weight[t];
                wx += s.weight[t] * left.world_x[y * W + s.column[t]];
                src += s.weight[t] * float(s.column[t]);
            }
            assert(std::abs(wsum - 1.0f) < 1e-5f);
            if (!s.filled) {
                float z_r, u;
                Sample d;
                assert(DirectSample(x, y, p, depth_l, depth_r, z_r, u, d));
                const float footprint = 2.0f * z_r / P00 / W;
                for (int t = 0; t < s.count; ++t) {
                    assert(left.surface[y * W + s.column[t]] == right.surface[i]);
                }
                if (right.surface[i] != 2) {
                    const float err = std::abs(wx - right.world_x[i]) / footprint;
                    assert(err <= (s.count == 2 ? 0.1f : 1.01f));
                    if (s.count == 2) {
                        ++result.bilinear;
                        result.subpixel_err += err;
                        result.unjittered_err += std::abs(wx - right0.world_x[i]) / footprint;
                    }
                }
                ++result.matched;
                if (std::abs(src - float(x)) > 0.5f) {
                    ++result.shifted;
                    // x_L - x_R has the sign of (m_R - m_L).
                    result.sign_ok += (src > float(x)) == (eye_r > eye_l);
                }
            } else {
                // A disocclusion: eye R's surface is hidden in eye L (behind the post) or
                // outside eye L's image; filled from the farther side's background, so never
                // from the post that hides it.
                for (int t = 0; t < s.count; ++t) {
                    assert(left.surface[y * W + s.column[t]] != 1 || right.surface[i] == 1);
                }
                ++result.filled;
            }
        }
    }
    if (result.bilinear) {
        result.subpixel_err /= result.bilinear;
        result.unjittered_err /= result.bilinear;
    }
    return result;
}

/// The hole the post uncovers is not one column stretched: its fill takes more than one
/// distinct source (the background mirrored into it).
int FillSources(float eye_l, float eye_r) {
    const Eye left = Render(eye_l), right = Render(eye_r);
    const Params p = Make(Shift(P00, eye_r, eye_l, W));
    auto depth_l = [&](int x, int y) { return left.depth[y * W + x]; };
    auto depth_r = [&](int x, int y) { return right.depth[y * W + x]; };
    int distinct = 0, last = -1;
    for (int x = 0; x < W; ++x) {
        const Sample s = WarpPixel(x, 0, p, depth_l, depth_r);
        if (s.filled && right.surface[x] == 0 && s.column[0] != last) {
            ++distinct;
            last = s.column[0];
        }
    }
    return distinct;
}

// The guided fill's scene: the same geometry, shaded. Albedo is a texture of the point's world x
// (the wall's differs per channel, the post is red), lighting varies slowly along the wall;
// the display holds LinearToDisplay(albedo * lighting) in B, G, R order (the game's
// A8R8G8B8Srgb), eye R's G-buffer the albedo (R, G, B, linear) and the normal.
Rgb Albedo(int surface, float wx) {
    if (surface == 1) {
        return {0.8f, 0.1f, 0.1f};
    }
    if (surface == 2) {
        return {0.0f, 0.0f, 0.0f}; // the sky: nothing drawn into the G-buffer
    }
    return {0.35f + 0.25f * std::sin(7.0f * wx), 0.3f + 0.2f * std::sin(11.0f * wx + 1.0f),
            0.25f + 0.15f * std::sin(5.0f * wx + 2.0f)};
}
float light_slope = 0.05f; ///< the wall's lighting gradient along x (per metre)
bool bumps = false;        ///< the wall normal-mapped (its normal tilts along x, lit from +x)
Rgb Normal(int surface, float wx) {
    if (surface == 1) {
        return {0.6f, 0.0f, 0.8f};
    }
    if (!bumps) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float t = 0.6f * std::sin(13.0f * wx); // up to about 31 degrees
    return {t / std::sqrt(1.0f + t * t), 0.0f, 1.0f / std::sqrt(1.0f + t * t)};
}
float Light(int surface, float wx) {
    if (surface != 0) {
        return surface == 1 ? 1.2f : 0.0f;
    }
    const float l = 0.7f + light_slope * wx;
    return bumps ? l * (0.6f + 0.8f * Normal(0, wx).r) : l;
}
/// The displayed value of a point, B, G, R.
Rgb Shaded(int surface, float wx) {
    const Rgb a = Albedo(surface, wx);
    const float l = Light(surface, wx);
    return {LinearToDisplay(a.b * l, true), LinearToDisplay(a.g * l, true),
            LinearToDisplay(a.r * l, true)};
    // (the sky is shaded 0 here; it is not filled, so its value does not matter)
}

struct GuidedResult {
    int holes = 0, guided = 0, distinct = 0;
    double err_guided = 0.0, err_fallback = 0.0; // mean |eye R - truth| on wall hole pixels
};

/// Eye R's hole pixels on the wall: the guided fill against the truth (the wall shaded at the
/// point eye R really sees), and the mirrored fill alone (flags without the albedo).
GuidedResult CheckGuided(float eye_l, float eye_r, bool normals, const GuidedTuning& t = {}) {
    const Eye left = Render(eye_l), right = Render(eye_r);
    Params p = Make(Shift(P00, eye_r, eye_l, W));
    p.flags = FlagAlbedo | FlagSwapRB | FlagSrgb | (normals ? FlagNormal : 0);
    Params fallback = p;
    fallback.flags = FlagSwapRB | FlagSrgb;
    auto depth_l = [&](int x, int y) { return left.depth[y * W + x]; };
    auto depth_r = [&](int x, int y) { return right.depth[y * W + x]; };
    auto color_l = [&](int x, int y) {
        return Shaded(left.surface[y * W + x], left.world_x[y * W + x]);
    };
    auto albedo_r = [&](int x, int y) {
        return Albedo(right.surface[y * W + x], right.world_x[y * W + x]);
    };
    auto normal_r = [&](int x, int y) {
        return Normal(right.surface[y * W + x], right.world_x[y * W + x]);
    };
    GuidedResult r;
    for (int y = 0; y < H; ++y) {
        float last = -1.0f;
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            if (!WarpPixel(x, y, p, depth_l, depth_r).filled || right.surface[i] != 0) {
                continue;
            }
            bool guided = false;
            const Rgb g = WarpColor(x, y, p, depth_l, depth_r, color_l, albedo_r, normal_r,
                                    &guided, t);
            const Rgb f =
                WarpColor(x, y, fallback, depth_l, depth_r, color_l, albedo_r, normal_r);
            const Rgb truth = Shaded(0, right.world_x[i]);
            auto err = [&](const Rgb& c) {
                return (std::abs(c.r - truth.r) + std::abs(c.g - truth.g) +
                        std::abs(c.b - truth.b)) /
                       3.0;
            };
            ++r.holes;
            r.guided += guided;
            r.err_guided += err(g);
            r.err_fallback += err(f);
            // A streak repeats one value along the row; the guided fill follows the texture.
            r.distinct += std::abs(g.r - last) > 1e-4f;
            last = g.r;
        }
    }
    if (r.holes) {
        r.err_guided /= r.holes;
        r.err_fallback /= r.holes;
    }
    return r;
}

} // namespace

int main() {
    // Depth -> view z through the projection's own constants; cleared depth is the far plane.
    assert(std::abs(ViewZ(Zs + Zo / 1.5f, Zs, Zo) - 1.5f) < 1e-3f);
    assert(std::abs(ViewZ(1.0f, Zs, Zo) - Far) / Far < 1e-3f);
    // The oracle's formula: x_L = x_R + P00 * ipd / z * W / 2 for eyes at -/+ ipd/2.
    assert(std::abs(Shift(P00, 0.0315f, -0.0315f, 1920) - P00 * 0.063f * 960) < 1e-3f);
    assert(SourceColumn(100, 10.0f, 0.0f) == 100);
    assert(SourceColumn(100, 10.0f, 86.4f) == 109);  // 8.64 px at 10 m, rounded
    assert(SourceColumn(100, 10.0f, -86.4f) == 91);

    // Zero eye distance (same=1): every pixel is eye L's own, bit for bit (one tap, weight 1).
    {
        const Eye left = Render(-0.0315f);
        const Params p = Make(0.0f);
        auto depth = [&](int x, int y) { return left.depth[y * W + x]; };
        for (int x = 0; x < W; ++x) {
            const Sample s = WarpPixel(x, 0, p, depth, depth);
            assert(!s.filled && s.count == 1 && s.column[0] == x && s.weight[0] == 1.0f);
        }
    }

    // Eyes at -/+ 31.5 mm: matched pixels are the same point, disparity has the right sign,
    // the region the post uncovers is filled from the wall.
    const Result stereo = Check(-0.0315f, 0.0315f);
    assert(stereo.filled > 0 && stereo.matched > W * H * 9 / 10);
    assert(stereo.shifted > 0 && stereo.sign_ok == stereo.shifted);
    assert(stereo.bilinear > W * H / 2 && stereo.subpixel_err < 0.02);
    assert(FillSources(-0.0315f, 0.0315f) > 1);

    // Inverted eye distance: the disparity flips sign, the rest still holds.
    const Result inverted = Check(0.0315f, -0.0315f);
    assert(inverted.filled > 0 && inverted.shifted > 0 && inverted.sign_ok == inverted.shifted);

    // A wider baseline (more disocclusion) still finds the background within the search.
    const Result wide = Check(-0.1f, 0.1f);
    assert(wide.filled > stereo.filled && wide.sign_ok == wide.shifted);

    // DLSS jitter (findings/31 section 9.8): both eyes' G-buffers carry the same viewport shift;
    // the warp's two taps interpolate eye L's jittered samples at x_R + shift / z, so eye R's
    // pixel keeps the jittered sample position (it matches eye R rendered WITH the jitter, not
    // without it) for content linear across a pixel. y is untouched (no disparity); what x loses
    // is the detail between the two taps (a linear filter of weight frac(u)).
    float worst_jitter_err = 0.0f;
    for (const float jx : {0.25f, -0.4f, 0.4375f}) {
        const Result j = Check(-0.0315f, 0.0315f, jx);
        assert(j.bilinear > W * H / 2 && j.subpixel_err < 0.02);
        assert(j.unjittered_err > 0.8 * std::abs(jx)); // the jitter is really in eye R
        worst_jitter_err = std::max(worst_jitter_err, float(j.subpixel_err));
    }

    // Eye L's sky drawn at depth 0.99997 (about 1600 m, as the game's sky dome) while eye R's
    // twin keeps the cleared 1.0: both are far (disparity well under a pixel), so the sky is
    // sampled directly, never filled from the wall beside it (the streaks into the sky).
    int sky = 0;
    {
        Eye left = Render(-0.0315f);
        const Eye right = Render(0.0315f);
        for (int i = 0; i < W * H; ++i) {
            if (left.surface[i] == 2) {
                left.depth[i] = 0.99997f;
            }
        }
        const Params p = Make(Shift(P00, 0.0315f, -0.0315f, W));
        auto depth_l = [&](int x, int y) { return left.depth[y * W + x]; };
        auto depth_r = [&](int x, int y) { return right.depth[y * W + x]; };
        for (int x = 0; x < W; ++x) {
            // Sky eye L sees there too (not the wall's end, a real disocclusion).
            if (right.surface[x] == 2 && left.surface[x] == 2 && x + 1 < W &&
                left.surface[x + 1] == 2) {
                const Sample s = WarpPixel(x, 0, p, depth_l, depth_r);
                assert(!s.filled);
                for (int t = 0; t < s.count; ++t) {
                    assert(left.surface[s.column[t]] == 2);
                }
                ++sky;
            }
        }
        assert(sky > W / 10);
        // The 2% rule alone filled all of them.
        Params strict = p;
        strict.tolerance_px = 0.0f;
        int strict_filled = 0;
        for (int x = 0; x < W; ++x) {
            strict_filled += right.surface[x] == 2 && left.surface[x] == 2 && x + 1 < W &&
                             left.surface[x + 1] == 2 && WarpPixel(x, 0, strict, depth_l, depth_r).filled;
        }
        assert(strict_filled == sky);
    }

    // G-buffer-guided hole fill (findings/31 section 9.10): every wall pixel the post uncovers
    // gets eye R's albedo times the wall's lighting beside it: the truth to the lighting's
    // variation over the window, far closer than the mirrored fill, and not a streak.
    GuidedResult guided[2];
    for (int n = 0; n < 2; ++n) {
        for (const float b : {0.0315f, 0.1f}) {
            const GuidedResult g = CheckGuided(-b, b, n == 1);
            assert(g.holes > 0 && g.guided == g.holes);
            assert(g.err_guided < 0.02 && g.err_guided < 0.15 * g.err_fallback);
            assert(g.distinct > g.holes * 3 / 4);
            if (b < 0.05f) {
                guided[n] = g;
            }
        }
    }
    // Uniform lighting on the wall: the ratio is exact, the fill the truth.
    light_slope = 0.0f;
    const GuidedResult flat = CheckGuided(-0.0315f, 0.0315f, true);
    // (Left: eye L's two taps blended in display encoding, about 0.2/255.)
    assert(flat.guided == flat.holes && flat.err_guided < 0.002);
    light_slope = 0.05f;
    // A normal-mapped wall lit from the side: the shading follows the normal, which the fit of
    // the lighting's shape in the normal (Y = l0 + l . n) follows and the plain ratio cannot.
    bumps = true;
    GuidedTuning ratio_only;
    ratio_only.normal_fit = false;
    const GuidedResult bump_fit = CheckGuided(-0.0315f, 0.0315f, true);
    const GuidedResult bump_ratio = CheckGuided(-0.0315f, 0.0315f, true, ratio_only);
    assert(bump_fit.guided == bump_fit.holes && bump_ratio.guided == bump_ratio.holes);
    assert(bump_fit.err_guided < 0.7 * bump_ratio.err_guided);
    assert(bump_fit.err_guided < 0.3 * bump_fit.err_fallback);
    bumps = false;
    // Zero eye distance with the guided fill on: still eye L's own value, bit for bit.
    {
        const Eye left = Render(-0.0315f);
        Params p = Make(0.0f);
        p.flags = FlagAlbedo | FlagNormal | FlagSwapRB | FlagSrgb;
        auto depth = [&](int x, int y) { return left.depth[y * W + x]; };
        auto color = [&](int x, int y) {
            return Shaded(left.surface[y * W + x], left.world_x[y * W + x]);
        };
        auto albedo = [&](int x, int y) {
            return Albedo(left.surface[y * W + x], left.world_x[y * W + x]);
        };
        auto normal = [&](int x, int y) {
            return Normal(left.surface[y * W + x], left.world_x[y * W + x]);
        };
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                bool g = true;
                const Rgb c = WarpColor(x, y, p, depth, depth, color, albedo, normal, &g);
                const Rgb e = color(x, y);
                assert(!g && c.r == e.r && c.g == e.g && c.b == e.b);
            }
        }
    }
    // The display encoding round-trips (the ratio is taken in linear light).
    for (int v = 0; v <= 255; ++v) {
        const float d = float(v) / 255.0f;
        assert(std::abs(LinearToDisplay(DisplayToLinear(d, true), true) - d) < 1e-4f);
    }

    std::printf("stereo warp: guided fill PASS (%d wall hole pixels at 63 mm, all guided: mean "
                "error %.4f vs mirrored fill %.4f (display units 0..1), with normals %.4f, "
                "uniform lighting %.4f; normal-mapped wall: lighting fit in the normal %.4f, "
                "plain ratio %.4f, mirrored %.4f; identity exact)\n",
                guided[0].holes, guided[0].err_guided, guided[0].err_fallback,
                guided[1].err_guided, flat.err_guided, bump_fit.err_guided,
                bump_ratio.err_guided, bump_fit.err_fallback);
    std::printf("stereo warp: PASS (sky at a different far depth per eye: %d pixels direct; "
                "identity, sign, inverted IPD, sub-pixel taps: %d pixels, "
                "mean error %.4f footprints; disocclusion fill: %d of %d pixels filled at 63 mm "
                "from %d sources, %d at 200 mm; jittered eyes: eye R keeps the jitter, mean "
                "error <= %.4f footprints)\n",
                sky, stereo.bilinear, stereo.subpixel_err, stereo.filled, W * H,
                FillSources(-0.0315f, 0.0315f), wide.filled, worst_jitter_err);
}
