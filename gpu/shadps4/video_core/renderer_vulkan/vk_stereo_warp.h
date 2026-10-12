// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR hybrid stereo (docs/vr/findings/31-true-stereo.md, "Hybrid stereo warp"): the math
// of host_shaders/stereo_warp.comp, header-only so tests/test_stereo_warp.cpp can check it on
// the CPU. The shader and WarpPixel below must stay the same algorithm.
//
// The eye at m metres along the camera's right axis sees clip.x - P00 * m (lower_stereo_remap,
// Stereo::ViewAt); clip w (= view z) and depth do not change. Pixel x = (ndc.x + 1) / 2 * W, so
// one point lands at
//     x_eye = x_center - P00 * m / z * W / 2
// and eye R's pixel x_R shows the point eye L sees at
//     x_L = x_R + P00 * (m_R - m_L) / z * W / 2 = x_R + shift / z.
// With m_L = -ipd/2, m_R = +ipd/2: x_L = x_R + P00 * ipd / z * W / 2 (the oracle's formula).
// z comes from the depth buffer through the projection's own constants (D3D style,
// depth = zs + zo / z; the game uses standard Z, cleared depth 1 = the far plane, which needs
// no special case: z = zo / (1 - zs) = far).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Vulkan::StereoWarp {

struct Params {
    float width = 0.0f;  ///< eye L color / eye R output, pixels
    float height = 0.0f;
    float depth_width = 0.0f; ///< both depth images, pixels
    float depth_height = 0.0f;
    float shift = 0.0f;  ///< P00 * (m_R - m_L) * width / 2: x_L = x_R + shift / z (pixels)
    float zs = 0.0f;     ///< depth = zs + zo / z
    float zo = 0.0f;
    float tolerance_rel = 0.0f; ///< same surface: |z_L - z_R| <= rel * z_R + abs
    float tolerance_abs = 0.0f;
    int32_t search = 0;  ///< disocclusion fill: pixels searched each way along the row
    float tolerance_px = 0.0f; ///< or same surface: disparities within this many pixels
    int32_t flags = 0;   ///< Flag* bits: the G-buffer-guided hole fill's inputs
};
static_assert(sizeof(Params) == 48, "push constant layout of stereo_warp.comp");

/// Params::flags. The guided fill (GuidedFill) needs eye R's albedo twin; normals refine it.
constexpr int32_t FlagAlbedo = 1; ///< eye R's albedo (base colour) twin is bound
constexpr int32_t FlagNormal = 2; ///< eye R's normal twin is bound
constexpr int32_t FlagSwapRB = 4; ///< the display bytes are B, G, R (the albedo's R, G, B)
constexpr int32_t FlagSrgb = 8;   ///< the display buffer is sRGB-encoded (the presenter's view)
constexpr int32_t FlagGuidedPass = 16; ///< the shader's second dispatch: the listed hole pixels

constexpr float TolRel = 0.02f; ///< as the oracle checks (tools/vr_stereo_poc.py dup)
constexpr float TolAbs = 0.01f;
constexpr int32_t MaxSearch = 96;
/// Two depths whose disparities differ by less than this are the same surface for the warp,
/// whatever their z: far away the 2% rule fails on depth precision and on eye L's sky (drawn
/// at depth 0.99997, about 1200 m, while eye R's twin keeps the cleared 1.0, 3000 m), which sent
/// every sky pixel to the disocclusion fill: building edges smeared up to 96 px into the sky.
constexpr float TolPx = 0.5f;

/// x_L = x_R + shift / z: shift in pixel metres.
inline float Shift(float p00, float eye_r, float eye_l, float width) {
    return p00 * (eye_r - eye_l) * width * 0.5f;
}

/// Disocclusion search radius: the widest disparity difference, foreground at 0.2 m.
inline int32_t SearchRadius(float shift) {
    return std::clamp(int32_t(std::ceil(std::abs(shift) / 0.2f)) + 2, 2, MaxSearch);
}

/// View z of a depth value; a value at or past the projection's pole counts as very far.
inline float ViewZ(float depth, float zs, float zo) {
    const float denom = depth - zs;
    const float z = std::abs(denom) < 1e-12f ? 1e30f : zo / denom;
    return z > 0.0f ? z : 1e30f;
}

/// Eye L's column for eye R's column x at view z: round(x + shift / z).
inline int32_t SourceColumn(int32_t x, float z, float shift) {
    return int32_t(std::floor(float(x) + 0.5f + shift / z));
}

inline bool SameSurface(float z_l, float z_r, const Params& p) {
    return std::abs(z_l - z_r) <= p.tolerance_rel * z_r + p.tolerance_abs ||
           std::abs(p.shift) * std::abs(1.0f / z_l - 1.0f / z_r) <= p.tolerance_px;
}

/// Depth images may have another size than the color (the upscaler's render size).
inline void DepthPixel(int32_t x, int32_t y, const Params& p, int32_t& dx, int32_t& dy) {
    dx = std::clamp(int32_t((float(x) + 0.5f) * p.depth_width / p.width), 0,
                    int32_t(p.depth_width) - 1);
    dy = std::clamp(int32_t((float(y) + 0.5f) * p.depth_height / p.height), 0,
                    int32_t(p.depth_height) - 1);
}

/// Eye R's pixel as a weighted sum of eye L's pixels in the same row (weights sum to 1).
struct Sample {
    int32_t column[3] = {0, 0, 0};
    float weight[3] = {0.0f, 0.0f, 0.0f};
    int32_t count = 0;
    bool filled = false; ///< a disocclusion, filled from the background beside it
    // For a filled pixel (GuidedFill): eye R's view z there and the background pixel the fill
    // found, side (-1, +1; 0 none) and distance along the row.
    float z = 0.0f;
    int32_t fill_side = 0, fill_k = 0;
};

/// Eye L's column `xl` shows the surface at view z `z` (in the image, same surface)?
template <class DepthL>
bool TapValid(int32_t xl, int32_t y, float z, const Params& p, DepthL&& depth_l) {
    if (xl < 0 || xl >= int32_t(p.width)) {
        return false; // outside eye L's image
    }
    int32_t lx, ly;
    DepthPixel(xl, y, p, lx, ly);
    return SameSurface(ViewZ(depth_l(lx, ly), p.zs, p.zo), z, p);
}

/// Eye R's pixel (x, y) from eye L where eye L sees the same surface: the sub-pixel source
/// u = x + shift / z (pixel index space), sampled linearly between its two columns, each tap
/// depth-tested (one failing tap: the other alone). False for a disocclusion. `z_r`: eye R's
/// view z there; `u`: the source position.
template <class DepthL, class DepthR>
bool DirectSample(int32_t x, int32_t y, const Params& p, DepthL&& depth_l, DepthR&& depth_r,
                  float& z_r, float& u, Sample& s) {
    int32_t dx, dy;
    DepthPixel(x, y, p, dx, dy);
    z_r = ViewZ(depth_r(dx, dy), p.zs, p.zo);
    s = Sample{};
    if (p.shift == 0.0f) {
        u = float(x);
        s.column[0] = x; // eyes together (same=1): eye L's own pixel, bit for bit
        s.weight[0] = 1.0f;
        s.count = 1;
        return true;
    }
    u = float(x) + p.shift / z_r;
    const float base = std::floor(u);
    const float f = u - base;
    const int32_t x0 = int32_t(base);
    const bool v0 = TapValid(x0, y, z_r, p, depth_l);
    const bool v1 = f > 0.0f && TapValid(x0 + 1, y, z_r, p, depth_l);
    if (v0 && v1) {
        s.column[0] = x0, s.weight[0] = 1.0f - f;
        s.column[1] = x0 + 1, s.weight[1] = f;
        s.count = 2;
    } else if (v0 || v1) {
        s.column[0] = v0 ? x0 : x0 + 1, s.weight[0] = 1.0f;
        s.count = 1;
    }
    return s.count > 0;
}

/// Eye R's pixel (x, y): the direct sample, or for a disocclusion (eye L sees another surface
/// there) the background beside it: the nearest direct pixel along the row on the farther
/// side, its source mirrored into the hole (the background's texture continued, not one
/// column stretched) and averaged over three depth-tested columns of that background.
template <class DepthL, class DepthR>
Sample WarpPixel(int32_t x, int32_t y, const Params& p, DepthL&& depth_l, DepthR&& depth_r) {
    float z_r, u;
    Sample s;
    if (DirectSample(x, y, p, depth_l, depth_r, z_r, u, s)) {
        return s;
    }
    Sample bg;
    float bg_z = -1.0f, bg_u = 0.0f;
    int32_t bg_side = 0, bg_k = 0;
    for (int32_t side = -1; side <= 1; side += 2) {
        for (int32_t k = 1; k <= p.search; ++k) {
            const int32_t nx = x + side * k;
            if (nx < 0 || nx >= int32_t(p.width)) {
                break;
            }
            float nz, nu;
            Sample n;
            if (DirectSample(nx, y, p, depth_l, depth_r, nz, nu, n)) {
                if (nz > bg_z) {
                    bg = n, bg_z = nz, bg_u = nu, bg_side = side, bg_k = k;
                }
                break;
            }
        }
    }
    s = Sample{};
    s.filled = true;
    s.z = z_r;
    s.fill_side = bg_side;
    s.fill_k = bg_k;
    if (bg_side == 0) {
        s.column[0] = std::clamp(SourceColumn(x, z_r, p.shift), 0, int32_t(p.width) - 1);
        s.weight[0] = 1.0f;
        s.count = 1;
        return s;
    }
    // The hole pixel k columns from the background pixel mirrors to k columns past it.
    const int32_t centre = int32_t(std::floor(bg_u + 0.5f)) + bg_side * bg_k;
    for (int32_t j = -1; j <= 1; ++j) {
        if (TapValid(centre + j, y, bg_z, p, depth_l)) {
            s.column[s.count++] = centre + j;
        }
    }
    if (s.count == 0) {
        bg.filled = true;
        bg.z = z_r;
        bg.fill_side = bg_side;
        bg.fill_k = bg_k;
        return bg; // no background past it: the background pixel's own color
    }
    for (int32_t i = 0; i < s.count; ++i) {
        s.weight[i] = 1.0f / float(s.count);
    }
    return s;
}

// G-buffer-guided hole fill (findings/31 section 9.10). A disoccluded pixel shows background
// that eye L's final image never saw, but eye R's own G-buffer drew it: its albedo and normal
// twins hold the right surface there. Its final colour is that albedo times the lighting the
// same background receives where eye R does see it through eye L: direct pixels beside the hole
// on the background side (depth close to the hole pixel's, facing its way), in linear light:
//     colour_R(p) = A_p * r * s(n_p),   r = sum(w A C) / sum(w A^2)   (per channel)
// r is the lighting's colour and level (the least-squares ratio of C_q = A_q * r), s its shape
// in the normal (the shading a normal map adds): the luminance lighting fitted as
// L_Y(n) = l0 + l . n (diffuse irradiance to first order) by weighted least squares on
// Y(C_q) = Y(A_q) * L_Y(n_q), the normal terms pulled towards 0 (ridge), divided by its plain
// ratio, so s = 1 for alike normals.
// The mirrored fill above stays the fallback (no albedo, sky, no such neighbour).

struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

/// The game's display pass applies a brightness curve (about x^0.85 per channel, findings/31
/// section 9.7) to the sRGB-encoded scene; the fill undoes both for the fit.
constexpr float DisplayCurve = 0.85f;

/// The guided fill's settings (the shader's constants are the defaults).
struct GuidedTuning {
    int32_t reach = 48;       ///< columns used past the hole, on its background side
    int32_t step = 3;         ///< every third column (16): 80 neighbours
    int32_t rows = 2;         ///< rows y - rows * row_step .. y + rows * row_step (5)
    int32_t row_step = 4;
    float depth_rel = 0.08f;  ///< neighbours within 8% of the hole pixel's z
    float min_normal = 0.7f;  ///< and facing within about 45 degrees of it (with normals)
    float min_weight = 2.0f;  ///< at least about two good neighbours
    float far_z = 1000.0f;    ///< sky and beyond: no albedo there (fallback)
    float ridge = 0.05f;      ///< normal terms' penalty, relative to sum(w A^2)
    float max_light = 16.0f;  ///< ratio clamp (linear)
    float max_shape = 4.0f;   ///< normal shading factor clamp
    bool normal_fit = true;   ///< false: the plain ratio (no normal terms)
};

/// A display value (0..1, as stored) to linear light, and back.
inline float DisplayToLinear(float v, bool srgb) {
    const float e = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / DisplayCurve);
    if (!srgb) {
        return e;
    }
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}
inline float LinearToDisplay(float l, bool srgb) {
    l = std::clamp(l, 0.0f, 1.0f);
    const float e =
        !srgb ? l : (l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
    return std::pow(e, DisplayCurve);
}

/// Solves the symmetric 4x4 system m x = v (Gaussian elimination, m positive definite by the
/// ridge); false when it is singular.
inline bool Solve4(float m[4][4], float v[4], float x[4]) {
    for (int32_t i = 0; i < 4; ++i) {
        if (!(std::abs(m[i][i]) > 1e-12f)) {
            return false;
        }
        for (int32_t j = i + 1; j < 4; ++j) {
            const float f = m[j][i] / m[i][i];
            for (int32_t k = i; k < 4; ++k) {
                m[j][k] -= f * m[i][k];
            }
            v[j] -= f * v[i];
        }
    }
    for (int32_t i = 3; i >= 0; --i) {
        float s = v[i];
        for (int32_t k = i + 1; k < 4; ++k) {
            s -= m[i][k] * x[k];
        }
        x[i] = s / m[i][i];
    }
    return true;
}

/// The lighting of the background beside a hole pixel (GuidedSolve).
struct GuidedLight {
    bool valid = false;
    float ratio[3] = {0.0f, 0.0f, 0.0f}; ///< per channel, display order (linear)
    bool shaped = false;                 ///< the shape in the normal was fitted
    float l[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< luminance lighting l0 + l . n
    float plain = 1.0f;                  ///< the luminance's plain ratio
};

/// One neighbour's contribution to a fit (the shader sums them across a subgroup).
struct GuidedSums {
    float rn[3] = {}, rd[3] = {}; ///< per channel: sum(w A C), sum(w A^2)
    float m[4][4] = {}, v[4] = {}; ///< luminance: sum(w Ya^2 phi phi^T), sum(w Ya Yc phi)
    float w = 0.0f;
};

/// Neighbour j (0 .. GuidedCount(t) - 1) of the hole pixel (x, y) whose fill found the
/// background `k0` px away on `side`: rows y - rows * row_step .. y + rows * row_step, columns
/// x + side * (k0, k0 + step, ...) (reach columns). A direct pixel near the hole pixel's view z
/// `z_ref` and facing its way (`n_ref`) adds its terms to `s`.
template <class DepthL, class DepthR, class ColorL, class AlbedoR, class NormalR>
void GuidedNeighbour(int32_t j, int32_t x, int32_t y, int32_t side, int32_t k0, float z_ref,
                     const Rgb& n_ref, const Params& p, DepthL&& depth_l, DepthR&& depth_r,
                     ColorL&& color_l, AlbedoR&& albedo_r, NormalR&& normal_r, GuidedSums& s,
                     const GuidedTuning& t = {}) {
    const int32_t cols = (t.reach + t.step - 1) / t.step;
    const int32_t qx = x + side * (k0 + (j % cols) * t.step);
    const int32_t qy = y + (j / cols - t.rows) * t.row_step;
    if (qy < 0 || qy >= int32_t(p.height) || qx < 0 || qx >= int32_t(p.width)) {
        return;
    }
    // Cheapest tests first: eye R's depth, its normal, then eye L's two taps as DirectSample (a
    // neighbour eye L does not see there is a hole pixel too: no lighting known).
    int32_t qdx, qdy;
    DepthPixel(qx, qy, p, qdx, qdy);
    const float z_q = ViewZ(depth_r(qdx, qdy), p.zs, p.zo);
    const float rel = std::abs(z_q - z_ref) / z_ref;
    if (rel > t.depth_rel) {
        return; // another surface (the foreground, a step in the background)
    }
    const bool normals = (p.flags & FlagNormal) != 0;
    const Rgb n_q = normals ? normal_r(qdx, qdy) : Rgb{};
    if (normals && n_ref.r * n_q.r + n_ref.g * n_q.g + n_ref.b * n_q.b < t.min_normal) {
        return; // another surface at a similar depth (a wall beside a floor)
    }
    const float u = float(qx) + p.shift / z_q;
    const float base = std::floor(u);
    const float f = u - base;
    const int32_t x0 = int32_t(base);
    const bool v0 = TapValid(x0, qy, z_q, p, depth_l);
    const bool v1 = f > 0.0f && TapValid(x0 + 1, qy, z_q, p, depth_l);
    if (!v0 && !v1) {
        return;
    }
    Rgb c;
    if (v0 && v1) {
        const Rgb c0 = color_l(x0, qy), c1 = color_l(x0 + 1, qy);
        c = Rgb{c0.r * (1.0f - f) + c1.r * f, c0.g * (1.0f - f) + c1.g * f,
                c0.b * (1.0f - f) + c1.b * f};
    } else {
        c = color_l(v0 ? x0 : x0 + 1, qy);
    }
    const float sigma = 0.25f * t.depth_rel;
    const float w = 1.0f / (1.0f + (rel / sigma) * (rel / sigma));
    const bool srgb = (p.flags & FlagSrgb) != 0;
    Rgb a_q = albedo_r(qdx, qdy);
    if (p.flags & FlagSwapRB) {
        a_q = Rgb{a_q.b, a_q.g, a_q.r}; // the albedo in the display's channel order
    }
    const float aq[3] = {a_q.r, a_q.g, a_q.b};
    const float cq[3] = {DisplayToLinear(c.r, srgb), DisplayToLinear(c.g, srgb),
                         DisplayToLinear(c.b, srgb)};
    for (int32_t ch = 0; ch < 3; ++ch) {
        s.rn[ch] += w * aq[ch] * cq[ch];
        s.rd[ch] += w * aq[ch] * aq[ch];
    }
    const float ya = (aq[0] + aq[1] + aq[2]) / 3.0f;
    const float yc = (cq[0] + cq[1] + cq[2]) / 3.0f;
    const float phi[4] = {1.0f, n_q.r, n_q.g, n_q.b};
    for (int32_t i = 0; i < 4; ++i) {
        s.v[i] += w * ya * yc * phi[i];
        for (int32_t k = 0; k < 4; ++k) {
            s.m[i][k] += w * ya * ya * phi[i] * phi[k];
        }
    }
    s.w += w;
}

inline int32_t GuidedCount(const GuidedTuning& t = {}) {
    return (2 * t.rows + 1) * ((t.reach + t.step - 1) / t.step);
}

/// The fit from the summed neighbours.
inline GuidedLight GuidedSolve(GuidedSums s, const Params& p, const GuidedTuning& t = {}) {
    GuidedLight light;
    if (s.w < t.min_weight) {
        return light;
    }
    light.valid = true;
    // An albedo floor keeps a channel the neighbours lack (pure red stone: no blue) finite.
    const float floor_m = 1e-4f * s.w;
    for (int32_t ch = 0; ch < 3; ++ch) {
        light.ratio[ch] = std::min(s.rn[ch] / std::max(s.rd[ch], floor_m), t.max_light);
    }
    if ((p.flags & FlagNormal) && t.normal_fit && s.m[0][0] > floor_m) {
        light.plain = s.v[0] / s.m[0][0];
        const float ridge = t.ridge * s.m[0][0];
        for (int32_t i = 1; i < 4; ++i) {
            s.m[i][i] += ridge;
        }
        light.shaped = light.plain > 1e-6f && Solve4(s.m, s.v, light.l);
    }
    return light;
}

/// A hole pixel's colour from the fit: its albedo times the ratio times the shape at its normal.
inline Rgb GuidedShade(const GuidedLight& light, Rgb a_p, const Rgb& n_p, const Params& p,
                       const GuidedTuning& t = {}) {
    if (p.flags & FlagSwapRB) {
        a_p = Rgb{a_p.b, a_p.g, a_p.r};
    }
    float shape = 1.0f;
    if (light.shaped) {
        shape = std::clamp((light.l[0] + light.l[1] * n_p.r + light.l[2] * n_p.g +
                            light.l[3] * n_p.b) /
                               light.plain,
                           0.0f, t.max_shape);
    }
    const bool srgb = (p.flags & FlagSrgb) != 0;
    return Rgb{LinearToDisplay(a_p.r * light.ratio[0] * shape, srgb),
               LinearToDisplay(a_p.g * light.ratio[1] * shape, srgb),
               LinearToDisplay(a_p.b * light.ratio[2] * shape, srgb)};
}

/// Eye R's final colour at a filled pixel from its albedo, its normal and the lighting of the
/// background beside it, display-encoded in the display buffer's channel order; false: keep the
/// fallback. `color_l(x, y)`: eye L's display value (Rgb, as stored); `albedo_r(dx, dy)`: eye
/// R's albedo, linear R, G, B; `normal_r(dx, dy)`: eye R's normal (unit, any consistent space).
template <class DepthL, class DepthR, class ColorL, class AlbedoR, class NormalR>
bool GuidedFill(int32_t x, int32_t y, const Sample& hole, const Params& p, DepthL&& depth_l,
                DepthR&& depth_r, ColorL&& color_l, AlbedoR&& albedo_r, NormalR&& normal_r,
                Rgb& out, const GuidedTuning& t = {}) {
    if (!(p.flags & FlagAlbedo) || !hole.filled || hole.fill_side == 0 || hole.z >= t.far_z) {
        return false;
    }
    int32_t dx, dy;
    DepthPixel(x, y, p, dx, dy);
    const Rgb n_p = (p.flags & FlagNormal) ? normal_r(dx, dy) : Rgb{};
    GuidedSums s;
    for (int32_t j = 0; j < GuidedCount(t); ++j) {
        GuidedNeighbour(j, x, y, hole.fill_side, hole.fill_k, hole.z, n_p, p, depth_l, depth_r,
                        color_l, albedo_r, normal_r, s, t);
    }
    const GuidedLight light = GuidedSolve(s, p, t);
    if (!light.valid) {
        return false;
    }
    out = GuidedShade(light, albedo_r(dx, dy), n_p, p, t);
    return true;
}

/// Eye R's final colour at (x, y), display-encoded in the display's order, as the shader
/// writes it: the warp's sample of eye L; a filled pixel from GuidedFill when it can.
template <class DepthL, class DepthR, class ColorL, class AlbedoR, class NormalR>
Rgb WarpColor(int32_t x, int32_t y, const Params& p, DepthL&& depth_l, DepthR&& depth_r,
              ColorL&& color_l, AlbedoR&& albedo_r, NormalR&& normal_r,
              bool* guided = nullptr, const GuidedTuning& t = {}) {
    const Sample s = WarpPixel(x, y, p, depth_l, depth_r);
    Rgb out;
    const bool g =
        s.filled &&
        GuidedFill(x, y, s, p, depth_l, depth_r, color_l, albedo_r, normal_r, out, t);
    if (guided) {
        *guided = g;
    }
    if (!g) {
        out = Rgb{};
        for (int32_t t = 0; t < s.count; ++t) {
            const Rgb v = color_l(s.column[t], y);
            out.r += v.r * s.weight[t], out.g += v.g * s.weight[t], out.b += v.b * s.weight[t];
        }
    }
    return out;
}

} // namespace Vulkan::StereoWarp
