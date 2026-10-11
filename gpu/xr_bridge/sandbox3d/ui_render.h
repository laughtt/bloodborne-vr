// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// VR menu drawing for sandbox3d:
//   - UiAtlas: the item icons and item table, UiFont: the game's menu font as a signed distance
//     field (both made by scripts/vr_assets/ui_extract.py into out/vr_assets/ui/);
//   - UiCanvas: builds world-space UI triangles on the CPU every frame: panels (a 2D frame in the
//     world, in meters), rounded boxes, ring sectors, icons and text;
//   - UiRenderer: their Vulkan pipeline (shaders/ui.*), drawn inside each eye's pass through
//     Renderer's draw hook: premultiplied alpha, no depth writes; the depth-tested layer first
//     (hands and weapons cover it), then the layer drawn on top of everything.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "math3d.h"
#include "renderer.h"
#include "ui_layout.h"

namespace sb {

struct UiVertex {
    float position[3];
    float uv[2];
    float color[4]; // linear RGB, straight alpha
    float shape[4]; // mode, parameters (shaders/ui.frag)
    float shape2[4];
};
static_assert(sizeof(UiVertex) == 68, "vertex layout of shaders/ui.vert");

/// An item of ui/icons.json (EquipParamGoods / EquipParamWeapon rows with names and pictures).
struct UiItem {
    std::string kind; // goods, weapon, armor
    int id = 0;
    std::string name;
    int icon = -1;
    int max = 1;
    std::string category; // consumable, ammo, tool, key, material, chalice, trick, firearm, left
    std::string hand;     // weapons: left or right
    std::string model;    // weapons: wp_a_XXXX
    std::string info;
    std::string use;      // goods with doc 17's catalog: heal, ammo, throw, buff, tool, other
    std::string effect;   // ... its effect ("fire attack +80, 60 s")

    /// Using it uses one up (consumables and ammo); tools and key items (Notebook, Monocular,
    /// bells) stay.
    bool Consumable() const { return category == "consumable" || category == "ammo"; }
};

/// A tracked pose in the world space the menus are drawn in (the sandbox's scene world, the
/// bridge's LOCAL or STAGE).
struct UiPose {
    bool valid = false;
    m3::Vec3 position;
    m3::Quat rotation;
};

class UiAtlas {
public:
    /// <dir>/icons.json and icons.png. The pixels stay in `pixels` until uploaded.
    bool Load(const std::filesystem::path& dir, std::string& log);
    /// By name (case-insensitive), "goods:1000" / "weapon:7000000", weapon model (wp_a_0800), or
    /// the first name starting with it.
    const UiItem* Find(const std::string& key) const;
    /// u0, v0, u1, v1 of an icon id.
    bool IconUv(int icon, float uv[4]) const;
    bool Ready() const { return width > 0; }

    uint32_t width = 0, height = 0;
    int cell = 128;
    std::vector<uint8_t> pixels;
    std::vector<UiItem> items;

private:
    void AppendItemSheet(const std::filesystem::path& assets, std::string& log);
    std::unordered_map<int, std::pair<int, int>> icons;
};

struct UiGlyph {
    float u0, v0, u1, v1;              // atlas
    float xoff, yoff, w, h, advance;   // em; yoff from the baseline to the cell top, y down
};

class UiFont {
public:
    /// <dir>/font.json and font.png (8-bit SDF).
    bool Load(const std::filesystem::path& dir, std::string& log);
    const UiGlyph* Find(uint32_t code) const;
    float Kerning(uint32_t left, uint32_t right) const;
    /// Width of a line in em.
    float Measure(const std::string& utf8) const;
    bool Ready() const { return !glyphs.empty(); }

    float ascent = 0.88f, descent = 0.12f;
    float spread = 0.1f; // the SDF's spread in its 0..1 value per em (0.5 / spread_px * em_px)
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;

private:
    std::unordered_map<uint32_t, UiGlyph> glyphs;
    std::map<uint64_t, float> kerning;
};

std::vector<uint32_t> Utf8Decode(const std::string& text);

/// A flat 2D frame in the world: elements are laid out in meters on its X (right) / Y (up) plane,
/// Z faces the viewer. `scale` scales the layout about the origin (open/close animation).
struct UiPanel {
    m3::Vec3 origin, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    float scale = 1.0f;
    m3::Vec3 At(float u, float v) const { return origin + (x * u + y * v) * scale; }
    /// Where the ray meets the plane, in panel coordinates (front side only).
    bool Hit(m3::Vec3 from, m3::Vec3 dir, float& u, float& v, float& distance) const;
    /// Panel coordinates of a point and its height above the plane.
    void Local(m3::Vec3 p, float& u, float& v, float& height) const;
    /// A frame at `origin` facing `toward` (its +Z), its +Y as close to `up` as possible.
    static UiPanel Facing(m3::Vec3 origin, m3::Vec3 toward, m3::Vec3 up);
};

enum class UiAlign { Left, Center, Right };

class UiCanvas {
public:
    UiCanvas(const UiAtlas* atlas, const UiFont* font) : atlas(atlas), font(font) {}

    void Clear() {
        world.clear();
        top.clear();
    }
    /// A filled box centered at (cx, cy), corners rounded by `radius`.
    void Box(const UiPanel& p, float cx, float cy, float w, float h, UiColor fill,
             float radius = 0.0f);
    /// A box with a border (drawn as a larger box behind it).
    void Frame(const UiPanel& p, float cx, float cy, float w, float h, UiColor fill,
               UiColor border, float border_width, float radius);
    /// The icon of an icon id (false when the atlas has none).
    bool Icon(const UiPanel& p, float cx, float cy, float size, int icon, UiColor tint);
    /// A ring sector centered on `angle` (radians, 0 = +X, counterclockwise), `half` wide on each
    /// side (>= pi: the whole ring), between the radii; `gap` (meters) trims both edges.
    void Sector(const UiPanel& p, float cx, float cy, float angle, float half, float r_in,
                float r_out, UiColor c, float gap = 0.0f);
    void Disc(const UiPanel& p, float cx, float cy, float r, UiColor c) {
        Sector(p, cx, cy, 0.0f, 3.2f, 0.0f, r, c);
    }
    /// One line of text, baseline at y, `size` = the em height (meters). Text wider than
    /// max_width (when > 0) is shrunk to fit. Returns the drawn width.
    float Text(const UiPanel& p, float x, float y, float size, const std::string& text,
               UiColor c, UiAlign align = UiAlign::Left, float max_width = 0.0f);
    float TextWidth(const std::string& text, float size) const;

    bool on_top = false;  // the layer the next elements go to
    float alpha = 1.0f;   // multiplies every color's alpha (fades)
    float outline = 0.10f; // SDF widening of the outline drawn behind text (0: none)
    UiColor outline_color{0.0f, 0.0f, 0.0f, 0.85f};
    std::vector<UiVertex> world, top;

private:
    void Quad(const UiPanel& p, const float corners[4][2], const float uvs[4][2], UiColor c,
              const float shape[4], const float shape2[4]);
    const UiAtlas* atlas;
    const UiFont* font;
};

class UiRenderer : public DrawHook {
public:
    bool Create(const Renderer::Device& device, std::string& error);
    void Destroy();
    bool UploadIcons(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
    bool UploadFont(uint32_t width, uint32_t height, const std::vector<uint8_t>& r8);
    /// The triangles of the next frame (the previous frame must be finished: Renderer waits).
    bool SetGeometry(const UiCanvas& canvas);
    void Draw(VkCommandBuffer cmd, const m3::Mat4& view_proj, VkFormat color,
              VkFormat depth) override;
    const std::string& LastError() const { return last_error; }
    /// Alpha output: false (default) keeps the target's alpha (an opaque scene: the compositor
    /// needs 1); true accumulates coverage (premultiplied "over" into a transparent layer, the
    /// bridge's UI layer). Set before the first Draw.
    void SetAccumulateAlpha(bool on) { accumulate_alpha = on; }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Pipelines {
        VkPipeline depth_tested = VK_NULL_HANDLE, on_top = VK_NULL_HANDLE;
    };

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    bool Upload(Texture& texture, uint32_t binding, uint32_t width, uint32_t height,
                VkFormat format, const uint8_t* pixels, size_t bytes);
    void DestroyTexture(Texture& texture);
    VkPipeline CreatePipeline(bool depth_test, VkFormat color, VkFormat depth);
    const Pipelines* PipelinesFor(VkFormat color, VkFormat depth);

    Renderer::Device dev;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    std::map<uint64_t, Pipelines> pipelines;
    Texture textures[2]; // 0 icons, 1 font
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    void* vertex_mapped = nullptr;
    size_t vertex_capacity = 0;
    uint32_t world_count = 0, top_count = 0;
    bool accumulate_alpha = false;
    std::string last_error;
};

} // namespace sb
