// SPDX-License-Identifier: GPL-2.0-or-later
// bbport VR image quality: see vk_xr_eye_iq.h.

#include "video_core/renderer_vulkan/vk_xr_eye_iq.h"
#include "video_core/renderer_vulkan/xr_dlss_logic.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace Vulkan::XrEyeIq {

namespace {

void Apply(Settings& s, const std::string& key, const std::string& value) {
    if (key == "aa") {
        s.fxaa = value == "fxaa";
    } else if (key == "upscale") {
        s.fsr1 = value == "fsr1";
        s.dlss = value == "dlss";
    } else if (key == "dlss_quality") {
        XrDlssLogic::Quality q{};
        if (XrDlssLogic::ParseQuality(value, q)) {
            s.dlss_quality = int(q);
            s.dlss_quality_set = true;
        } else if (value == "auto" || value.empty()) {
            s.dlss_quality_set = false;
        } else {
            std::printf("VR IQ: unknown dlss_quality '%s' (dlaa|quality|balanced|performance|"
                        "ultra|auto)\n",
                        value.c_str());
        }
    } else if (key == "dlss_preset") {
        u32 preset = 0;
        bool usable = false;
        if (XrDlssLogic::ParsePreset(value, preset, &usable)) {
            s.dlss_preset = preset;
            if (!usable) {
                std::printf("VR IQ: DLSS preset %c is not a model in SDK 310.9.1 (the driver "
                            "uses its default; use J, K, L or M)\n",
                            XrDlssLogic::PresetLetter(preset));
            }
        } else {
            std::printf("VR IQ: unknown dlss_preset '%s' (default|J|K|L|M)\n", value.c_str());
        }
    } else if (key == "dlss_sharpen") {
        s.dlss_sharpen = std::clamp(float(std::atof(value.c_str())), 0.0f, 1.0f);
    } else if (key == "sharpen") {
        s.sharpen = std::clamp(float(std::atof(value.c_str())), 0.0f, 1.0f);
    } else if (key == "res") {
        unsigned w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 &&
            h <= 8192) {
            s.width = w;
            s.height = h;
        } else if (const float f = float(std::atof(value.c_str())); f >= 1.0f && f <= 4.0f) {
            s.width = s.height = 0;
            s.scale = f;
        }
    }
}

Settings FromEnv() {
    Settings s{};
    const auto env = [&](const char* name, const char* key) {
        if (const char* v = std::getenv(name); v && *v) {
            Apply(s, key, v);
        }
    };
    env("BB_XR_AA", "aa");
    env("BB_XR_UPSCALE", "upscale");
    env("BB_XR_SHARPEN", "sharpen");
    env("BB_XR_EYE_RES", "res");
    env("BB_XR_DLSS_QUALITY", "dlss_quality");
    env("BB_XR_DLSS_PRESET", "dlss_preset");
    env("BB_XR_DLSS_SHARPEN", "dlss_sharpen");
    return s;
}

void Describe(const Settings& s, const char* where) {
    char res[48];
    if (s.width) {
        std::snprintf(res, sizeof(res), "%ux%u", s.width, s.height);
    } else {
        std::snprintf(res, sizeof(res), "%.2gx the display buffer", s.scale);
    }
    if (s.dlss) {
        char quality[48];
        if (s.dlss_quality_set) {
            std::snprintf(quality, sizeof(quality), "%s",
                          XrDlssLogic::QualityName(XrDlssLogic::Quality(s.dlss_quality)));
        } else {
            std::snprintf(quality, sizeof(quality), "from the size, frame %s", res);
        }
        std::printf("VR IQ (%s): upscale dlss (quality %s, preset %c, sharpen %.2f)\n", where,
                    quality, XrDlssLogic::PresetLetter(s.dlss_preset), s.dlss_sharpen);
    } else {
        std::printf("VR IQ (%s): aa %s, upscale %s (frame %s, sharpen %.2f)\n", where,
                    s.fxaa ? "fxaa" : "off", s.fsr1 ? "fsr1" : "off", res, s.sharpen);
    }
    std::fflush(stdout);
}

} // namespace

Settings Current() {
    static std::mutex mutex;
    static bool initialized = false;
    static Settings settings;
    static const char* control = std::getenv("BB_XR_IQ_CONTROL");
    static std::chrono::steady_clock::time_point checked;
    static long long mtime = -1;
    std::scoped_lock lock{mutex};
    if (!initialized) {
        initialized = true;
        settings = FromEnv();
        if (settings.fxaa || settings.fsr1 || settings.dlss || control) {
            Describe(settings, "environment");
        }
    }
    if (control) {
        const auto now = std::chrono::steady_clock::now();
        if (now - checked >= std::chrono::milliseconds(250)) {
            checked = now;
            struct stat st{};
            if (stat(control, &st) == 0 && (long long)st.st_mtime * 1000 + (long long)st.st_size != mtime) {
                mtime = (long long)st.st_mtime * 1000 + (long long)st.st_size;
                std::ifstream file(control);
                std::stringstream text;
                text << file.rdbuf();
                Settings s = FromEnv();
                std::string token;
                while (text >> token) {
                    const auto eq = token.find('=');
                    if (eq != std::string::npos) {
                        Apply(s, token.substr(0, eq), token.substr(eq + 1));
                    }
                }
                settings = s;
                Describe(settings, "control file");
            }
        }
    }
    return settings;
}

void TargetSize(const Settings& s, u32 src_w, u32 src_h, u32& out_w, u32& out_h) {
    out_w = out_h = 0;
    if ((!s.fsr1 && !s.dlss) || src_w == 0 || src_h == 0) {
        return;
    }
    u32 res_w, res_h;
    if (s.width) {
        res_w = s.width;
        res_h = s.height;
    } else {
        res_w = std::min(8192u, u32(float(src_w) * s.scale + 0.5f));
        res_h = std::min(8192u, u32(float(src_h) * s.scale + 0.5f));
    }
    if (s.dlss) {
        XrDlssLogic::OutputSize(src_w, src_h, s.dlss_quality_set,
                                XrDlssLogic::Quality(s.dlss_quality), res_w, res_h, out_w, out_h);
        return;
    }
    out_w = res_w;
    out_h = res_h;
}

} // namespace Vulkan::XrEyeIq
