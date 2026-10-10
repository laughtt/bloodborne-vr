// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "dash_preview.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

#include "image_out.h"
#include "json.h"
#include "locomotion.h"
#include "model.h"
#include "vignette.h"
#include "vr_dash.h"

namespace sb {

using namespace m3;

namespace {

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kBack{250, 250, 247}, kInk{40, 40, 46}, kGridLine{226, 226, 220}, kAxis{120, 120, 125};
constexpr Rgb kRaw{228, 118, 28}, kDash{28, 150, 82}, kFlat{120, 196, 150}, kVignette{60, 110, 222};
constexpr Rgb kMark{150, 150, 155};

// ---- a 5x7 bitmap font (upper case; lower case is drawn as upper case) --------------------------

struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {';', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b00100, 0b01000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
    {'/', {0b00000, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b00000}},
    {'%', {0b11000, 0b11001, 0b00010, 0b00100, 0b01000, 0b10011, 0b00011}},
    {'=', {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'_', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111}},
    {'*', {0b00000, 0b00100, 0b10101, 0b01110, 0b10101, 0b00100, 0b00000}},
    {'|', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'\'', {0b01100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'"', {0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'^', {0b01100, 0b10010, 0b10010, 0b01100, 0b00000, 0b00000, 0b00000}}, // degree sign
};

class Canvas {
public:
    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 255) {
        Rect(0, 0, w, h, kBack);
    }
    void Blend(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) {
            return;
        }
        a = std::min(a, 1.0f);
        uint8_t* p = &px[(size_t(y) * w + x) * 4];
        p[0] = uint8_t(std::lround(p[0] + (c.r - p[0]) * a));
        p[1] = uint8_t(std::lround(p[1] + (c.g - p[1]) * a));
        p[2] = uint8_t(std::lround(p[2] + (c.b - p[2]) * a));
    }
    void Rect(int x0, int y0, int x1, int y1, Rgb c) {
        for (int y = std::max(y0, 0); y < std::min(y1, h); ++y) {
            for (int x = std::max(x0, 0); x < std::min(x1, w); ++x) {
                Blend(x, y, c, 1.0f);
            }
        }
    }
    void Disc(float cx, float cy, float r, Rgb c) {
        for (int y = int(std::floor(cy - r - 1)); y <= int(std::ceil(cy + r + 1)); ++y) {
            for (int x = int(std::floor(cx - r - 1)); x <= int(std::ceil(cx + r + 1)); ++x) {
                const float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
                Blend(x, y, c, std::clamp(r + 0.5f - d, 0.0f, 1.0f));
            }
        }
    }
    void Line(float x0, float y0, float x1, float y1, Rgb c, float width) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int steps = std::max(1, int(len * 2.0f));
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i) / float(steps);
            Disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, width * 0.5f, c);
        }
    }
    void Text(int x, int y, const std::string& s, Rgb c, int scale = 2) {
        for (char ch : s) {
            const char u = char(std::toupper(static_cast<unsigned char>(ch)));
            for (const Glyph& g : kFont) {
                if (g.c != u) {
                    continue;
                }
                for (int row = 0; row < 7; ++row) {
                    for (int col = 0; col < 5; ++col) {
                        if (g.rows[row] & (0x10 >> col)) {
                            for (int dy = 0; dy < scale; ++dy) {
                                for (int dx = 0; dx < scale; ++dx) {
                                    Blend(x + col * scale + dx, y + row * scale + dy, c, 1.0f);
                                }
                            }
                        }
                    }
                }
                break;
            }
            x += 6 * scale;
        }
    }
    static int TextWidth(const std::string& s, int scale = 2) { return int(s.size()) * 6 * scale; }
    bool Save(const std::filesystem::path& path) const { return WritePng(path.string(), uint32_t(w), uint32_t(h), px); }

    int w, h;
    std::vector<uint8_t> px;
};

std::string Fmt(const char* format, double v) {
    char text[64];
    std::snprintf(text, sizeof(text), format, v);
    return text;
}

struct Series {
    std::vector<float> x, y;
    Rgb color{kInk};
    float width = 2.5f;
    bool dots = false;  // a dot per sample (the game's frames)
    bool steps = false; // held between samples (the game at its frame rate)
    std::string label;
};

/// One plot: axes with ticks, series, a legend.
struct Panel {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the whole cell in pixels
    std::string title, xlabel, ylabel;
    bool equal = false;                 // same scale on both axes (top views)
    float xmin = 0, xmax = 1, ymin = 0, ymax = 1;
    bool fixed = false;
    std::vector<Series> series;
    std::vector<std::pair<float, std::string>> marks; // vertical lines at x

    void Fit() {
        if (fixed) {
            return;
        }
        float a = 1e9f, b = -1e9f, c = 1e9f, d = -1e9f;
        for (const Series& s : series) {
            for (size_t i = 0; i < s.x.size(); ++i) {
                a = std::min(a, s.x[i]);
                b = std::max(b, s.x[i]);
                c = std::min(c, s.y[i]);
                d = std::max(d, s.y[i]);
            }
        }
        if (a > b) {
            a = 0, b = 1, c = 0, d = 1;
        }
        const float px = std::max((b - a) * 0.06f, 0.05f), py = std::max((d - c) * 0.08f, 0.05f);
        xmin = a - px, xmax = b + px, ymin = c - py, ymax = d + py;
        if (equal) {
            const float pw = float(PlotX1() - PlotX0()), ph = float(PlotY1() - PlotY0());
            const float sx = (xmax - xmin) / pw, sy = (ymax - ymin) / ph;
            if (sx > sy) {
                const float mid = 0.5f * (ymin + ymax), half = 0.5f * sx * ph;
                ymin = mid - half, ymax = mid + half;
            } else {
                const float mid = 0.5f * (xmin + xmax), half = 0.5f * sy * pw;
                xmin = mid - half, xmax = mid + half;
            }
        }
    }
    int PlotX0() const { return x0 + 78; }
    int PlotX1() const { return x1 - 16; }
    int PlotY0() const { return y0 + 40; }
    int PlotY1() const { return y1 - 54; }
    float X(float v) const { return float(PlotX0()) + (v - xmin) / (xmax - xmin) * float(PlotX1() - PlotX0()); }
    float Y(float v) const { return float(PlotY1()) - (v - ymin) / (ymax - ymin) * float(PlotY1() - PlotY0()); }

    static float Nice(float range) {
        const float raw = range / 5.0f;
        const float p = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-6f))));
        const float m = raw / p;
        return (m < 1.5f ? 1.0f : m < 3.5f ? 2.0f : m < 7.5f ? 5.0f : 10.0f) * p;
    }

    void Draw(Canvas& cv) const {
        cv.Text(x0 + 10, y0 + 10, title, kInk, 2);
        // Grid and ticks.
        const float sx = Nice(xmax - xmin), sy = Nice(ymax - ymin);
        for (float v = std::ceil(xmin / sx) * sx; v <= xmax + 1e-6f; v += sx) {
            cv.Line(X(v), float(PlotY0()), X(v), float(PlotY1()), kGridLine, 1.0f);
            const std::string t = Fmt(sx < 0.1f ? "%.2f" : sx < 1.0f ? "%.1f" : "%.0f", std::fabs(v) < 1e-6f ? 0.0 : v);
            cv.Text(int(X(v)) - Canvas::TextWidth(t, 1) / 2, PlotY1() + 6, t, kAxis, 1);
        }
        for (float v = std::ceil(ymin / sy) * sy; v <= ymax + 1e-6f; v += sy) {
            cv.Line(float(PlotX0()), Y(v), float(PlotX1()), Y(v), kGridLine, 1.0f);
            const std::string t = Fmt(sy < 0.1f ? "%.2f" : sy < 1.0f ? "%.1f" : "%.0f", std::fabs(v) < 1e-6f ? 0.0 : v);
            cv.Text(PlotX0() - 8 - Canvas::TextWidth(t, 1), int(Y(v)) - 3, t, kAxis, 1);
        }
        cv.Line(float(PlotX0()), float(PlotY1()), float(PlotX1()), float(PlotY1()), kAxis, 1.5f);
        cv.Line(float(PlotX0()), float(PlotY0()), float(PlotX0()), float(PlotY1()), kAxis, 1.5f);
        cv.Text((PlotX0() + PlotX1()) / 2 - Canvas::TextWidth(xlabel, 1) / 2, PlotY1() + 20, xlabel, kInk, 1);
        cv.Text(x0 + 10, y0 + 28, ylabel, kInk, 1);
        int mark_row = 0;
        for (const auto& [x, label] : marks) {
            if (x < xmin || x > xmax) {
                continue;
            }
            for (int y = PlotY0(); y < PlotY1(); y += 8) {
                cv.Line(X(x), float(y), X(x), float(std::min(y + 4, PlotY1())), kMark, 1.0f);
            }
            cv.Text(int(X(x)) + 4, PlotY0() + 4 + 12 * mark_row++, label, kMark, 1);
        }
        // Series, clipped to the plot.
        const auto inside = [&](float x, float y) {
            return x >= float(PlotX0()) - 1 && x <= float(PlotX1()) + 1 && y >= float(PlotY0()) - 1 &&
                   y <= float(PlotY1()) + 1;
        };
        for (const Series& s : series) {
            for (size_t i = 1; i < s.x.size(); ++i) {
                float ax = X(s.x[i - 1]), ay = Y(s.y[i - 1]), bx = X(s.x[i]), by = Y(s.y[i]);
                if (s.steps) {
                    if (inside(ax, ay) && inside(bx, ay)) cv.Line(ax, ay, bx, ay, s.color, s.width);
                    if (inside(bx, ay) && inside(bx, by)) cv.Line(bx, ay, bx, by, s.color, s.width);
                } else if (inside(ax, ay) && inside(bx, by)) {
                    cv.Line(ax, ay, bx, by, s.color, s.width);
                }
            }
            if (s.dots) {
                for (size_t i = 0; i < s.x.size(); ++i) {
                    if (inside(X(s.x[i]), Y(s.y[i]))) cv.Disc(X(s.x[i]), Y(s.y[i]), s.width + 0.8f, s.color);
                }
            }
        }
        // Legend.
        int ly = PlotY0() + 6;
        for (const Series& s : series) {
            if (s.label.empty()) {
                continue;
            }
            const int lx = PlotX1() - Canvas::TextWidth(s.label, 1) - 34;
            cv.Line(float(lx), float(ly + 3), float(lx + 22), float(ly + 3), s.color, 3.0f);
            cv.Text(lx + 28, ly, s.label, kInk, 1);
            ly += 14;
        }
    }
};

// ---- simulation: the game (the move's eye path at its frame rate) and the dash at 72 Hz ----------

struct SimMove {
    const MovePath* path = nullptr;
    float yaw = 0.0f;       // direction (radians about +Y, 0 = -Z)
    double t0 = 0.0;
    float stop_at = 1.0f;   // fraction of the path where the character stops (blocked)
    float cancel_at = 1.0f; // fraction of its length when the move ends early
    int kind = VR_DASH_DODGE;
    float duration = 0.0f;  // the dash's (0: params)
};
struct SimSample {
    float t;
    Vec3 raw, cam;
    float vignette;
    int mode;
};

std::vector<SimSample> Simulate(const std::vector<SimMove>& moves, const VrDashParams& params, double t_end,
                                float game_fps, double warp_at = -1.0, Vec3 warp = {}) {
    std::vector<SimSample> out;
    VrDash dash;
    vr_dash_reset(&dash);
    Vec3 base{0.0f, 1.65f, 0.0f}; // the eye: where the previous move left it
    size_t current = 0;
    Vec3 c0 = base;
    for (double t = 0.0; t < t_end; t += 1.0 / 72.0) {
        while (current + 1 <= moves.size() && t >= moves[current].t0) {
            // A new move starts from where the character is now.
            if (current > 0) {
                const SimMove& prev = moves[current - 1];
                const float tau = float(moves[current].t0 - prev.t0);
                const float lim = std::min(prev.stop_at * prev.path->length, prev.cancel_at * prev.path->length);
                Vec3 e = prev.path->Eye(std::min(tau, lim));
                c0 = c0 + Rotate(AxisAngle({0, 1, 0}, prev.yaw), Vec3{e.x, 0.0f, e.z});
            }
            ++current;
        }
        Vec3 raw = c0;
        int id = 0;
        VrDashHint hint{};
        if (current > 0) {
            const SimMove& m = moves[current - 1];
            const float len = m.path->length;
            float tau = float(t - m.t0);
            if (game_fps > 0.0f) {
                tau = std::floor(tau * game_fps + 1e-4f) / game_fps;
            }
            const float lim = std::min(m.stop_at, m.cancel_at) * len;
            raw = c0 + Rotate(AxisAngle({0, 1, 0}, m.yaw), m.path->Eye(std::min(tau, lim)));
            if (float(t - m.t0) < m.cancel_at * len) {
                id = int(current);
                const Vec3 end = Rotate(AxisAngle({0, 1, 0}, m.yaw), m.path->End());
                hint.kind = m.kind;
                hint.direction = {end.x, 0.0f, end.z};
                hint.distance = std::sqrt(end.x * end.x + end.z * end.z);
                hint.duration = m.duration;
                hint.height = -1.0f;
            }
        }
        if (warp_at >= 0.0 && t >= warp_at) {
            raw = raw + warp;
        }
        const VrDashV3 o = vr_dash_update(&dash, &params, {raw.x, raw.y, raw.z}, id, id ? &hint : nullptr, t);
        out.push_back({float(t), raw, {o.x, o.y, o.z}, dash.vignette, dash.mode});
    }
    return out;
}

float Along(Vec3 p, Vec3 origin, float yaw) {
    const Vec3 dir = Rotate(AxisAngle({0, 1, 0}, yaw), {0, 0, -1});
    return (p.x - origin.x) * dir.x + (p.z - origin.z) * dir.z;
}

/// The four standard panels of one move: top view, side view, progress over time, speed and
/// vignette over time.
void MovePanels(Canvas& cv, const std::vector<SimSample>& s, const std::vector<SimSample>* flat, float yaw,
                float move_t0, float move_length, float settle, const std::string& name) {
    const int W = cv.w / 2, H = (cv.h - 60) / 2;
    Panel top, side, progress, speed;
    top = {0, 60, W, 60 + H, "TOP VIEW: " + name, "X (M)", "Z (M)", true};
    side = {W, 60, cv.w, 60 + H, "SIDE VIEW: HEIGHT ALONG THE MOVE", "DISTANCE ALONG THE MOVE (M)", "EYE HEIGHT (M)"};
    progress = {0, 60 + H, W, 60 + 2 * H, "PROGRESS OVER TIME (GAME 30 FPS, CAMERA 72 HZ)", "TIME (S)",
                "DISTANCE ALONG (M)"};
    speed = {W, 60 + H, cv.w, 60 + 2 * H, "CAMERA SPEED AND VIGNETTE", "TIME (S)", "SPEED (M/S); VIGNETTE x 10"};
    const Vec3 origin = s.front().raw;
    Series raw_top{{}, {}, kRaw, 2.0f, true, false, "ANIMATION (RAW)"};
    Series dash_top{{}, {}, kDash, 3.0f, false, false, "COMFORT DASH"};
    Series raw_side = raw_top, dash_side = dash_top, raw_prog = raw_top, dash_prog = dash_top;
    raw_prog.steps = true;
    raw_prog.dots = false;
    Series raw_speed{{}, {}, kRaw, 1.5f, false, false, "RAW (72 HZ)"};
    Series dash_speed{{}, {}, kDash, 2.5f, false, false, "DASH"};
    Series vig{{}, {}, kVignette, 2.0f, false, false, "VIGNETTE x 10"};
    Vec3 last_raw = s.front().raw;
    for (size_t i = 0; i < s.size(); ++i) {
        const SimSample& x = s[i];
        const bool new_sample = i == 0 || Length(x.raw - last_raw) > 1e-6f;
        if (new_sample) {
            raw_top.x.push_back(x.raw.x), raw_top.y.push_back(-x.raw.z);
            raw_side.x.push_back(Along(x.raw, origin, yaw)), raw_side.y.push_back(x.raw.y);
        }
        last_raw = x.raw;
        dash_top.x.push_back(x.cam.x), dash_top.y.push_back(-x.cam.z);
        dash_side.x.push_back(Along(x.cam, origin, yaw)), dash_side.y.push_back(x.cam.y);
        raw_prog.x.push_back(x.t), raw_prog.y.push_back(Along(x.raw, origin, yaw));
        dash_prog.x.push_back(x.t), dash_prog.y.push_back(Along(x.cam, origin, yaw));
        if (i > 0) {
            raw_speed.x.push_back(x.t), raw_speed.y.push_back(Length(x.raw - s[i - 1].raw) * 72.0f);
            dash_speed.x.push_back(x.t), dash_speed.y.push_back(Length(x.cam - s[i - 1].cam) * 72.0f);
        }
        vig.x.push_back(x.t), vig.y.push_back(x.vignette * 10.0f);
    }
    top.ylabel = "-Z FORWARD (M)";
    top.series = {raw_top, dash_top};
    side.series = {raw_side, dash_side};
    progress.series = {raw_prog, dash_prog};
    speed.series = {raw_speed, dash_speed, vig};
    if (flat) {
        Series flat_side{{}, {}, kFlat, 2.0f, false, false, "FLAT (JUMP_FLAT)"};
        for (const SimSample& x : *flat) {
            flat_side.x.push_back(Along(x.cam, origin, yaw)), flat_side.y.push_back(x.cam.y);
        }
        side.series.push_back(flat_side);
    }
    progress.marks = {{move_t0 + move_length, "MOVE ENDS"}, {move_t0 + move_length + settle, "SETTLED"}};
    speed.marks = progress.marks;
    for (Panel* p : {&top, &side, &progress, &speed}) {
        p->Fit();
        p->Draw(cv);
    }
}

void Header(Canvas& cv, const std::string& title, const std::string& note) {
    cv.Rect(0, 0, cv.w, 56, {236, 236, 230});
    cv.Text(16, 10, title, kInk, 3);
    cv.Text(16, 38, note, {90, 90, 95}, 1);
}

std::string Settings(const LocoConfig& c, const VrDashParams& p) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "DASH %.2f S, EASE %s (POWER %.1f), SETTLE %.2f S, JUMP HEIGHT %.2f M OVER %.2f S, MAX LEAD %.2f M, "
                  "GAME %.0f FPS",
                  p.duration, c.ease.c_str(), p.power, p.settle, p.jump_height, p.jump_duration, p.max_lead,
                  c.game_fps);
    return text;
}

} // namespace

bool WriteDashPreviews(const std::filesystem::path& out_dir, const std::filesystem::path& assets,
                       const std::filesystem::path& config_path, std::string& log) {
    LocoConfig config;
    {
        std::string error;
        const json::Value doc = json::Parse(ReadText(config_path), &error);
        if (error.empty()) {
            config.Load(doc["locomotion"]);
        }
    }
    const VrDashParams params = DashParams(config);
    MoveSet moves;
    moves.Load(assets, config);
    log += moves.log + "\n";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const float fps = config.game_fps > 0.0f ? config.game_fps : 30.0f;
    const std::string settings = Settings(config, params);
    bool ok = true;
    const auto save = [&](const Canvas& cv, const char* name, const std::string& what) {
        const std::filesystem::path png = out_dir / name;
        const bool saved = cv.Save(png);
        ok = ok && saved;
        log += (saved ? "dash preview: " : "cannot write ") + png.string() + (saved ? ": " + what : "") + "\n";
    };
    // Roll, backstep: one move each, raw against the dash.
    struct One {
        const char* file;
        const char* title;
        const MovePath* path;
        float yaw;
        int kind;
    } ones[3] = {{"dash_roll.png", "ROLL (DODGE)", &moves.roll, -0.5f, VR_DASH_DODGE},
                 {"dash_backstep.png", "BACKSTEP", &moves.backstep, -0.5f, VR_DASH_DODGE},
                 {"dash_jump.png", "JUMP", &moves.jump, -0.5f, VR_DASH_JUMP}};
    for (const One& one : ones) {
        SimMove m;
        m.path = one.path;
        m.yaw = one.yaw;
        m.t0 = 0.2;
        m.kind = one.kind;
        m.duration = one.kind == VR_DASH_JUMP ? config.jump_duration : 0.0f;
        const auto s = Simulate({m}, params, 0.2 + one.path->length + params.settle + 0.6, fps);
        std::vector<SimSample> flat;
        if (one.kind == VR_DASH_JUMP) {
            VrDashParams p = params;
            p.jump_height = 0.0f;
            flat = Simulate({m}, p, 0.2 + one.path->length + params.settle + 0.6, fps);
        }
        Canvas cv(1600, 1060);
        float raw_dip = 0.0f, cam_dy = 0.0f, peak = 0.0f;
        for (size_t i = 1; i < s.size(); ++i) {
            raw_dip = std::max(raw_dip, std::fabs(s[i].raw.y - 1.65f));
            cam_dy = std::max(cam_dy, std::fabs(s[i].cam.y - 1.65f));
            peak = std::max(peak, Length(s[i].cam - s[i - 1].cam) * 72.0f);
        }
        char note[256];
        std::snprintf(note, sizeof(note), "%s %s, %.2f M IN %.2f S. EYE HEIGHT CHANGE: RAW %.2f M, CAMERA %.2f M. %s",
                      one.path->source.c_str(), one.path->name.c_str(), one.path->Distance(), one.path->length,
                      raw_dip, cam_dy, settings.c_str());
        Header(cv, std::string("COMFORT DASH: ") + one.title, note);
        MovePanels(cv, std::vector<SimSample>(s.begin() + 10, s.end()), one.kind == VR_DASH_JUMP ? &flat : nullptr,
                   one.yaw, float(m.t0), one.path->length, params.settle, one.title);
        char what[160];
        std::snprintf(what, sizeof(what), "%s, raw height change %.2f m -> camera %.2f m, peak camera speed %.1f m/s",
                      one.path->name.c_str(), raw_dip, cam_dy, peak);
        save(cv, one.file, what);
    }

    // The hard cases: cancelled, chained, blocked, teleport.
    {
        Canvas cv(1600, 1060);
        Header(cv, "COMFORT DASH: EDGE CASES", "CANCELLED AT 40%, THREE MOVES BACK TO BACK, BLOCKED BY A WALL AFTER 1 M "
                                               "(MAX LEAD 0.5 M), A TELEPORT DURING THE MOVE. " + settings);
        const int W = cv.w / 2, H = (cv.h - 60) / 2;
        const MovePath& roll = moves.roll;
        // Cancelled: 40% into the root motion.
        {
            SimMove m{&roll, 0.0f, 0.2};
            m.stop_at = m.cancel_at = 0.4f * roll.motion_end / roll.length;
            const auto s = Simulate({m}, params, 1.6, fps);
            Panel p{0, 60, W, 60 + H, "CANCELLED 40% INTO THE ROOT MOTION (HIT OUT OF THE ROLL)", "TIME (S)",
                    "DISTANCE ALONG (M)"};
            Series a{{}, {}, kRaw, 2.0f, false, true, "ANIMATION (RAW)"}, b{{}, {}, kDash, 3.0f, false, false, "DASH"};
            for (const SimSample& x : s) {
                a.x.push_back(x.t), a.y.push_back(Along(x.raw, s.front().raw, 0.0f));
                b.x.push_back(x.t), b.y.push_back(Along(x.cam, s.front().raw, 0.0f));
            }
            p.series = {a, b};
            p.marks = {{0.2f + 0.4f * roll.motion_end, "CANCEL"}};
            p.Fit();
            p.Draw(cv);
        }
        // Chained.
        {
            const float M = roll.motion_end; // the next roll 75% into this one's root motion
            std::vector<SimMove> chain = {{&roll, 0.0f, 0.2}, {&roll, -1.5708f, 0.2 + 0.75 * M},
                                          {&roll, -3.1416f, 0.2 + 1.5 * M}};
            const auto s = Simulate(chain, params, 0.2 + 1.5 * M + roll.length + params.settle + 0.4, fps);
            Panel p{W, 60, cv.w, 60 + H, "THREE ROLLS BACK TO BACK (FORWARD, RIGHT, BACK)", "X (M)", "-Z FORWARD (M)", true};
            Series a{{}, {}, kRaw, 2.0f, true, false, "ANIMATION (RAW)"}, b{{}, {}, kDash, 3.0f, false, false, "DASH"};
            Vec3 last{1e9f, 0, 0};
            for (const SimSample& x : s) {
                if (Length(x.raw - last) > 1e-6f) {
                    a.x.push_back(x.raw.x), a.y.push_back(-x.raw.z);
                }
                last = x.raw;
                b.x.push_back(x.cam.x), b.y.push_back(-x.cam.z);
            }
            p.series = {a, b};
            p.Fit();
            p.Draw(cv);
        }
        // Blocked.
        {
            VrDashParams q = params;
            q.max_lead = 0.5f;
            SimMove m{&roll, 0.0f, 0.2};
            for (float tau = 0.0f; tau < roll.length; tau += 0.005f) { // where the path has gone 1 m
                const Vec3 e = roll.Eye(tau);
                m.stop_at = tau / roll.length;
                if (std::sqrt(e.x * e.x + e.z * e.z) >= 1.0f) {
                    break;
                }
            }
            const auto s = Simulate({m}, q, 0.2 + roll.length + 0.6, fps);
            Panel p{0, 60 + H, W, 60 + 2 * H, "BLOCKED: A WALL AFTER ABOUT 1 M (MAX_LEAD 0.5 M)", "TIME (S)", "DISTANCE ALONG (M)"};
            p.marks = {{0.2f + roll.length, "CLIP ENDS"}};
            Series a{{}, {}, kRaw, 2.0f, false, true, "ANIMATION (RAW)"}, b{{}, {}, kDash, 3.0f, false, false, "DASH"};
            for (const SimSample& x : s) {
                a.x.push_back(x.t), a.y.push_back(Along(x.raw, s.front().raw, 0.0f));
                b.x.push_back(x.t), b.y.push_back(Along(x.cam, s.front().raw, 0.0f));
            }
            p.series = {a, b};
            p.Fit();
            p.Draw(cv);
        }
        // Teleport.
        {
            SimMove m{&roll, 0.0f, 0.2};
            const auto s = Simulate({m}, params, 1.6, fps, 0.45, {6.0f, 0.0f, 0.0f});
            Panel p{W, 60 + H, cv.w, 60 + 2 * H, "TELEPORT 6 M SIDEWAYS DURING THE ROLL: SNAPS", "TIME (S)", "X (M)"};
            Series a{{}, {}, kRaw, 2.0f, false, true, "ANIMATION (RAW)"}, b{{}, {}, kDash, 3.0f, false, false, "DASH"};
            for (const SimSample& x : s) {
                a.x.push_back(x.t), a.y.push_back(x.raw.x);
                b.x.push_back(x.t), b.y.push_back(x.cam.x);
            }
            p.series = {a, b};
            p.marks = {{0.45f, "WARP"}};
            p.Fit();
            p.Draw(cv);
        }
        save(cv, "dash_cases.png", "cancelled, chained, blocked, teleport");
    }

    // The curves: the ease options, and the vignette across the view.
    {
        Canvas cv(1600, 640);
        Header(cv, "EASE CURVES AND THE VIGNETTE", "LEFT: PROGRESS OVER THE DASH FOR EACH \"EASE\" (THE CONFIGURED ONE IS THICK). "
                                                  "RIGHT: LIGHT THROUGH THE VIGNETTE BY ANGLE FROM THE HEAD'S AXIS.");
        const int W = cv.w / 2;
        Panel ease{0, 60, W, cv.h, "EASE: PROGRESS OVER THE DASH", "TIME / DASH DURATION", "SHARE OF THE DISTANCE"};
        ease.fixed = true, ease.xmin = -0.03f, ease.xmax = 1.03f, ease.ymin = -0.03f, ease.ymax = 1.08f;
        const struct {
            const char* name;
            int curve;
            float power;
            Rgb color;
        } curves[] = {{"linear", VR_DASH_EASE_POWER, 1, {150, 150, 150}},
                      {"quad", VR_DASH_EASE_POWER, 2, {120, 170, 220}},
                      {"cubic", VR_DASH_EASE_POWER, 3, kDash},
                      {"quart", VR_DASH_EASE_POWER, 4, {170, 120, 200}},
                      {"expo", VR_DASH_EASE_EXPO, 10, {220, 160, 60}},
                      {"smooth", VR_DASH_EASE_SMOOTH, 0, {200, 90, 90}}};
        for (const auto& c : curves) {
            Series s{{}, {}, c.color, c.name == config.ease ? 4.0f : 2.0f, false, false, c.name};
            for (int i = 0; i <= 100; ++i) {
                s.x.push_back(float(i) / 100.0f), s.y.push_back(vr_dash_ease(c.curve, c.power, float(i) / 100.0f));
            }
            ease.series.push_back(s);
        }
        ease.Draw(cv);
        Panel vig{W, 60, cv.w, cv.h, "VIGNETTE (STRENGTH " + Fmt("%.2f", config.vignette) + ", " +
                                        Fmt("%.0f", config.vignette_inner_deg) + "^ TO " +
                                        Fmt("%.0f", config.vignette_outer_deg) + "^)",
                  "ANGLE FROM THE HEAD'S FORWARD AXIS (DEGREES)", "LIGHT THROUGH (1 = NONE TAKEN)"};
        vig.fixed = true, vig.xmin = 0, vig.xmax = 90, vig.ymin = -0.03f, vig.ymax = 1.05f;
        Series full{{}, {}, kVignette, 3.0f, false, false, "AT FULL DASH SPEED"};
        Series half{{}, {}, {150, 175, 230}, 2.0f, false, false, "AT HALF"};
        for (int a = 0; a <= 90; ++a) {
            full.x.push_back(float(a)), half.x.push_back(float(a));
            full.y.push_back(Vignette::Transmission(float(a) * kDeg, config.vignette, config.vignette_inner_deg * kDeg,
                                                    config.vignette_outer_deg * kDeg));
            half.y.push_back(Vignette::Transmission(float(a) * kDeg, 0.5f * config.vignette,
                                                    config.vignette_inner_deg * kDeg, config.vignette_outer_deg * kDeg));
        }
        vig.series = {full, half};
        vig.marks = {{51.0f, "DREAM AIR EDGE (ABOUT 51^)"}};
        vig.Draw(cv);
        save(cv, "dash_curves.png", "ease curves, vignette profile");
    }
    return ok;
}

} // namespace sb
