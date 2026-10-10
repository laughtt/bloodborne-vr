// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "ui_render.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "json.h"
#include "model.h"
#include "png_read.h"

// SPIR-V compiled at build time (glslangValidator --vn, see CMakeLists.txt).
#include "ui_frag.h"
#include "ui_vert.h"

namespace sb {

using namespace m3;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool HasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT;
}

constexpr float kMargin = 0.002f; // meters around shapes for their antialiased edge

} // namespace

// ---- Atlas and font ------------------------------------------------------------------------------

bool UiAtlas::Load(const std::filesystem::path& dir, std::string& log) {
    bool ok = false;
    const std::string text = ReadText(dir / "icons.json", &ok);
    if (!ok) {
        log += "no " + (dir / "icons.json").string() +
               " (run scripts/vr_assets/ui_extract.py): menus show names instead of icons\n";
        return false;
    }
    std::string error;
    const json::Value doc = json::Parse(text, &error);
    if (!error.empty()) {
        log += (dir / "icons.json").string() + ": " + error + "\n";
        return false;
    }
    cell = doc["cell"].Int(128);
    icons.clear();
    if (doc["icons"].IsObject()) {
        for (const auto& [key, xy] : doc["icons"].object) {
            icons[std::atoi(key.c_str())] = {xy[0].Int(), xy[1].Int()};
        }
    }
    items.clear();
    for (const json::Value& v : doc["items"].array) {
        UiItem item;
        item.kind = v["kind"].Str();
        item.id = v["id"].Int();
        item.name = v["name"].Str();
        item.icon = v["icon"].Int(-1);
        item.max = v["max"].Int(1);
        item.category = v["category"].Str();
        item.hand = v["hand"].Str();
        item.model = v["model"].Str();
        item.info = v["info"].Str();
        item.use = v["use"].Str();
        item.effect = v["effect"].Str();
        items.push_back(item);
    }
    uint32_t w = 0, h = 0;
    int channels = 0;
    if (!ReadPng((dir / "icons.png").string(), w, h, channels, pixels, error) || channels != 4) {
        log += (dir / "icons.png").string() + ": " + (error.empty() ? "not RGBA" : error) + "\n";
        pixels.clear();
        return false;
    }
    width = w;
    height = h;
    log += "icons: " + std::to_string(icons.size()) + " pictures, " +
           std::to_string(items.size()) + " items (" + (dir / "icons.png").string() + ")\n";
    return true;
}

const UiItem* UiAtlas::Find(const std::string& key) const {
    if (key.empty()) {
        return nullptr;
    }
    const std::string low = Lower(key);
    for (const UiItem& item : items) {
        if (Lower(item.name) == low) {
            return &item;
        }
    }
    const size_t colon = low.find(':');
    if (colon != std::string::npos) {
        const std::string kind = low.substr(0, colon);
        const int id = std::atoi(low.c_str() + colon + 1);
        for (const UiItem& item : items) {
            if (item.kind == kind && item.id == id) {
                return &item;
            }
        }
    }
    for (const UiItem& item : items) {
        if (!item.model.empty() && Lower(item.model) == low) {
            return &item;
        }
    }
    for (const UiItem& item : items) {
        if (Lower(item.name).rfind(low, 0) == 0) {
            return &item;
        }
    }
    return nullptr;
}

bool UiAtlas::IconUv(int icon, float uv[4]) const {
    const auto it = icons.find(icon);
    if (it == icons.end() || !width || !height) {
        return false;
    }
    // Half a texel in, so bilinear filtering never reaches the next cell.
    uv[0] = (float(it->second.first) + 0.5f) / float(width);
    uv[1] = (float(it->second.second) + 0.5f) / float(height);
    uv[2] = (float(it->second.first + cell) - 0.5f) / float(width);
    uv[3] = (float(it->second.second + cell) - 0.5f) / float(height);
    return true;
}

bool UiFont::Load(const std::filesystem::path& dir, std::string& log) {
    bool ok = false;
    const std::string text = ReadText(dir / "font.json", &ok);
    if (!ok) {
        log += "no " + (dir / "font.json").string() +
               " (run scripts/vr_assets/ui_extract.py): menus without text\n";
        return false;
    }
    std::string error;
    const json::Value doc = json::Parse(text, &error);
    if (!error.empty()) {
        log += (dir / "font.json").string() + ": " + error + "\n";
        return false;
    }
    uint32_t w = 0, h = 0;
    int channels = 0;
    if (!ReadPng((dir / "font.png").string(), w, h, channels, pixels, error) || channels != 1) {
        log += (dir / "font.png").string() + ": " + (error.empty() ? "not gray" : error) + "\n";
        pixels.clear();
        return false;
    }
    width = w;
    height = h;
    const float em_px = doc["em_px"].Float(56.0f);
    spread = 0.5f / std::max(doc["spread_px"].Float(6.0f), 0.5f) * em_px;
    ascent = doc["ascent"].Float(0.88f);
    descent = doc["descent"].Float(0.12f);
    glyphs.clear();
    for (const auto& [key, g] : doc["glyphs"].object) {
        UiGlyph glyph{};
        const float x = g["x"].Float(), y = g["y"].Float(), gw = g["w"].Float(),
                    gh = g["h"].Float();
        glyph.u0 = x / float(w);
        glyph.v0 = y / float(h);
        glyph.u1 = (x + gw) / float(w);
        glyph.v1 = (y + gh) / float(h);
        glyph.xoff = g["xoff"].Float();
        glyph.yoff = g["yoff"].Float();
        glyph.w = gw / em_px;
        glyph.h = gh / em_px;
        glyph.advance = g["advance"].Float();
        glyphs[uint32_t(std::atoi(key.c_str()))] = glyph;
    }
    kerning.clear();
    for (const json::Value& k : doc["kerning"].array) {
        kerning[(uint64_t(k[0].Int()) << 32) | uint32_t(k[1].Int())] = k[2].Float();
    }
    log += "font: " + doc["font"].Str() + ", " + std::to_string(glyphs.size()) + " glyphs (" +
           (dir / "font.png").string() + ")\n";
    return true;
}

const UiGlyph* UiFont::Find(uint32_t code) const {
    const auto it = glyphs.find(code);
    if (it != glyphs.end()) {
        return &it->second;
    }
    const auto q = glyphs.find('?');
    return q != glyphs.end() ? &q->second : nullptr;
}

float UiFont::Kerning(uint32_t left, uint32_t right) const {
    if (kerning.empty()) {
        return 0.0f;
    }
    const auto it = kerning.find((uint64_t(left) << 32) | right);
    return it != kerning.end() ? it->second : 0.0f;
}

float UiFont::Measure(const std::string& utf8) const {
    float em = 0.0f;
    uint32_t prev = 0;
    for (uint32_t code : Utf8Decode(utf8)) {
        if (const UiGlyph* g = Find(code)) {
            em += g->advance + (prev ? Kerning(prev, code) : 0.0f);
        }
        prev = code;
    }
    return em;
}

std::vector<uint32_t> Utf8Decode(const std::string& text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const uint8_t c = uint8_t(text[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07, extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0F, extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1F, extra = 1;
        }
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i) {
            cp = (cp << 6) | (uint8_t(text[i]) & 0x3F);
        }
        out.push_back(cp);
    }
    return out;
}

// ---- Panels and canvas ---------------------------------------------------------------------------

bool UiPanel::Hit(Vec3 from, Vec3 dir, float& u, float& v, float& distance) const {
    const float denom = Dot(dir, z);
    if (denom > -1e-5f) {
        return false; // parallel, or reaching the back side
    }
    const float t = Dot(origin - from, z) / denom;
    if (t < 0.0f) {
        return false;
    }
    const Vec3 d = from + dir * t - origin;
    const float s = std::max(scale, 1e-4f);
    u = Dot(d, x) / s;
    v = Dot(d, y) / s;
    distance = t;
    return true;
}

void UiPanel::Local(Vec3 p, float& u, float& v, float& height) const {
    const Vec3 d = p - origin;
    const float s = std::max(scale, 1e-4f);
    u = Dot(d, x) / s;
    v = Dot(d, y) / s;
    height = Dot(d, z);
}

UiPanel UiPanel::Facing(Vec3 origin, Vec3 toward, Vec3 up) {
    UiPanel p;
    p.origin = origin;
    p.z = Normalize(toward - origin, {0, 0, 1});
    p.x = Normalize(Cross(up, p.z), AnyPerp(p.z));
    p.y = Cross(p.z, p.x);
    return p;
}

void UiCanvas::Quad(const UiPanel& p, const float corners[4][2], const float uvs[4][2], UiColor c,
                    const float shape[4], const float shape2[4]) {
    std::vector<UiVertex>& out = on_top ? top : world;
    UiVertex v[4];
    for (int k = 0; k < 4; ++k) {
        const Vec3 w = p.At(corners[k][0], corners[k][1]);
        v[k] = {{w.x, w.y, w.z},
                {uvs[k][0], uvs[k][1]},
                {c.r, c.g, c.b, c.a * alpha},
                {shape[0], shape[1], shape[2], shape[3]},
                {shape2[0], shape2[1], shape2[2], shape2[3]}};
    }
    out.push_back(v[0]);
    out.push_back(v[1]);
    out.push_back(v[2]);
    out.push_back(v[0]);
    out.push_back(v[2]);
    out.push_back(v[3]);
}

void UiCanvas::Box(const UiPanel& p, float cx, float cy, float w, float h, UiColor fill,
                   float radius) {
    if (fill.a * alpha <= 0.0f || w <= 0.0f || h <= 0.0f) {
        return;
    }
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float ex = hw + kMargin, ey = hh + kMargin;
    const float corners[4][2] = {{cx - ex, cy + ey}, {cx + ex, cy + ey}, {cx + ex, cy - ey},
                                 {cx - ex, cy - ey}};
    const float uvs[4][2] = {{-ex, ey}, {ex, ey}, {ex, -ey}, {-ex, -ey}};
    const float shape[4] = {3.0f, hw, hh, std::clamp(radius, 0.0f, std::min(hw, hh))};
    const float shape2[4] = {};
    Quad(p, corners, uvs, fill, shape, shape2);
}

void UiCanvas::Frame(const UiPanel& p, float cx, float cy, float w, float h, UiColor fill,
                     UiColor border, float border_width, float radius) {
    if (border_width > 0.0f) {
        Box(p, cx, cy, w + 2.0f * border_width, h + 2.0f * border_width, border,
            radius + border_width);
    }
    Box(p, cx, cy, w, h, fill, radius);
}

bool UiCanvas::Icon(const UiPanel& p, float cx, float cy, float size, int icon, UiColor tint) {
    float uv[4];
    if (!atlas || !atlas->IconUv(icon, uv)) {
        return false;
    }
    const float h = size * 0.5f;
    const float corners[4][2] = {{cx - h, cy + h}, {cx + h, cy + h}, {cx + h, cy - h},
                                 {cx - h, cy - h}};
    const float uvs[4][2] = {{uv[0], uv[1]}, {uv[2], uv[1]}, {uv[2], uv[3]}, {uv[0], uv[3]}};
    const float shape[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float shape2[4] = {};
    Quad(p, corners, uvs, tint, shape, shape2);
    return true;
}

void UiCanvas::Sector(const UiPanel& p, float cx, float cy, float angle, float half, float r_in,
                      float r_out, UiColor c, float gap) {
    if (c.a * alpha <= 0.0f || r_out <= 0.0f) {
        return;
    }
    float x0, x1, y0, y1;
    if (half >= kPi * 0.5f) {
        x0 = y0 = -r_out - kMargin;
        x1 = y1 = r_out + kMargin;
    } else {
        x0 = r_in * std::cos(half) - kMargin;
        x1 = r_out + kMargin;
        y1 = r_out * std::sin(half) + kMargin;
        y0 = -y1;
    }
    const float ca = std::cos(angle), sa = std::sin(angle);
    const float local[4][2] = {{x0, y1}, {x1, y1}, {x1, y0}, {x0, y0}};
    float corners[4][2], uvs[4][2];
    for (int k = 0; k < 4; ++k) {
        corners[k][0] = cx + local[k][0] * ca - local[k][1] * sa;
        corners[k][1] = cy + local[k][0] * sa + local[k][1] * ca;
        uvs[k][0] = local[k][0];
        uvs[k][1] = local[k][1];
    }
    const float shape[4] = {4.0f, r_in, r_out, half};
    const float shape2[4] = {gap, 0.0f, 0.0f, 0.0f};
    Quad(p, corners, uvs, c, shape, shape2);
}

float UiCanvas::TextWidth(const std::string& text, float size) const {
    return font && font->Ready() ? font->Measure(text) * size : 0.0f;
}

float UiCanvas::Text(const UiPanel& p, float x, float y, float size, const std::string& text,
                     UiColor c, UiAlign align, float max_width) {
    if (!font || !font->Ready() || text.empty() || c.a * alpha <= 0.0f) {
        return 0.0f;
    }
    float width = font->Measure(text) * size;
    if (max_width > 0.0f && width > max_width) {
        size *= max_width / width;
        width = max_width;
    }
    float pen = align == UiAlign::Left ? x : align == UiAlign::Center ? x - width * 0.5f
                                                                       : x - width;
    const std::vector<uint32_t> codes = Utf8Decode(text);
    const float shape2[4] = {};
    for (int pass = outline > 0.0f ? 0 : 1; pass < 2; ++pass) {
        float at = pen;
        uint32_t prev = 0;
        const UiColor color = pass == 0 ? outline_color.WithAlpha(c.a) : c;
        const float shape[4] = {2.0f, pass == 0 ? outline : 0.0f, pass == 0 ? 0.04f : 0.0f, 0.0f};
        for (uint32_t code : codes) {
            const UiGlyph* g = font->Find(code);
            if (!g) {
                continue;
            }
            if (prev) {
                at += font->Kerning(prev, code) * size;
            }
            prev = code;
            if (g->w > 0.0f) {
                const float left = at + g->xoff * size, right = left + g->w * size;
                const float topy = y - g->yoff * size, bottom = topy - g->h * size;
                const float corners[4][2] = {{left, topy}, {right, topy}, {right, bottom},
                                             {left, bottom}};
                const float uvs[4][2] = {{g->u0, g->v0}, {g->u1, g->v0}, {g->u1, g->v1},
                                         {g->u0, g->v1}};
                Quad(p, corners, uvs, color, shape, shape2);
            }
            at += g->advance * size;
        }
    }
    return width;
}

// ---- Vulkan --------------------------------------------------------------------------------------

uint32_t UiRenderer::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool UiRenderer::Create(const Renderer::Device& device, std::string& error) {
    dev = device;
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &memory_properties);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = dev.family;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateCommandPool(dev.device, &pool_info, nullptr, &pool) != VK_SUCCESS ||
        (alloc.commandPool = pool, vkAllocateCommandBuffers(dev.device, &alloc, &cmd)) !=
            VK_SUCCESS ||
        vkCreateFence(dev.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        error = "ui: command pool/buffer/fence";
        return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpool.maxSets = 1;
    dpool.poolSizeCount = 1;
    dpool.pPoolSizes = &pool_size;
    if (vkCreateSampler(dev.device, &sampler_info, nullptr, &sampler) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(dev.device, &set_info, nullptr, &set_layout) != VK_SUCCESS ||
        vkCreateDescriptorPool(dev.device, &dpool, nullptr, &descriptor_pool) != VK_SUCCESS) {
        error = "ui: sampler/descriptors";
        return false;
    }
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    VkShaderModuleCreateInfo vs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vs.codeSize = sizeof(ui_vert_spv);
    vs.pCode = ui_vert_spv;
    VkShaderModuleCreateInfo fs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fs.codeSize = sizeof(ui_frag_spv);
    fs.pCode = ui_frag_spv;
    if (vkAllocateDescriptorSets(dev.device, &set_alloc, &set) != VK_SUCCESS ||
        vkCreatePipelineLayout(dev.device, &layout_info, nullptr, &layout) != VK_SUCCESS ||
        vkCreateShaderModule(dev.device, &vs, nullptr, &vert) != VK_SUCCESS ||
        vkCreateShaderModule(dev.device, &fs, nullptr, &frag) != VK_SUCCESS) {
        error = "ui: descriptor set/pipeline layout/shaders";
        return false;
    }
    // White 1x1 stand-ins until the atlases are uploaded (the set is always complete).
    const uint8_t white[4] = {255, 255, 255, 255};
    if (!Upload(textures[0], 0, 1, 1, VK_FORMAT_R8G8B8A8_SRGB, white, 4) ||
        !Upload(textures[1], 1, 1, 1, VK_FORMAT_R8_UNORM, white, 1)) {
        error = "ui: " + last_error;
        return false;
    }
    return true;
}

void UiRenderer::DestroyTexture(Texture& texture) {
    if (texture.view) {
        vkDestroyImageView(dev.device, texture.view, nullptr);
    }
    if (texture.image) {
        vkDestroyImage(dev.device, texture.image, nullptr);
    }
    if (texture.memory) {
        vkFreeMemory(dev.device, texture.memory, nullptr);
    }
    texture = {};
}

void UiRenderer::Destroy() {
    if (!dev.device) {
        return;
    }
    for (auto& [key, p] : pipelines) {
        for (VkPipeline pipe : {p.depth_tested, p.on_top}) {
            if (pipe) {
                vkDestroyPipeline(dev.device, pipe, nullptr);
            }
        }
    }
    pipelines.clear();
    for (Texture& t : textures) {
        DestroyTexture(t);
    }
    if (vertex_buffer) {
        vkDestroyBuffer(dev.device, vertex_buffer, nullptr);
    }
    if (vertex_memory) {
        vkFreeMemory(dev.device, vertex_memory, nullptr);
    }
    vertex_buffer = VK_NULL_HANDLE;
    vertex_memory = VK_NULL_HANDLE;
    for (VkShaderModule m : {vert, frag}) {
        if (m) {
            vkDestroyShaderModule(dev.device, m, nullptr);
        }
    }
    if (layout) {
        vkDestroyPipelineLayout(dev.device, layout, nullptr);
    }
    if (descriptor_pool) {
        vkDestroyDescriptorPool(dev.device, descriptor_pool, nullptr);
    }
    if (set_layout) {
        vkDestroyDescriptorSetLayout(dev.device, set_layout, nullptr);
    }
    if (sampler) {
        vkDestroySampler(dev.device, sampler, nullptr);
    }
    if (fence) {
        vkDestroyFence(dev.device, fence, nullptr);
    }
    if (pool) {
        vkDestroyCommandPool(dev.device, pool, nullptr);
    }
    dev = {};
}

bool UiRenderer::UploadIcons(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    return rgba.size() >= size_t(width) * height * 4 &&
           Upload(textures[0], 0, width, height, VK_FORMAT_R8G8B8A8_SRGB, rgba.data(),
                  size_t(width) * height * 4);
}

bool UiRenderer::UploadFont(uint32_t width, uint32_t height, const std::vector<uint8_t>& r8) {
    return r8.size() >= size_t(width) * height &&
           Upload(textures[1], 1, width, height, VK_FORMAT_R8_UNORM, r8.data(),
                  size_t(width) * height);
}

bool UiRenderer::Upload(Texture& texture, uint32_t binding, uint32_t width, uint32_t height,
                        VkFormat format, const uint8_t* pixels, size_t bytes) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(dev.physical, format, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    uint32_t mips = 1;
    if ((props.optimalTilingFeatures & need) == need) {
        for (uint32_t s = std::max(width, height); s > 1; s >>= 1) {
            ++mips;
        }
    }
    Texture next;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(dev.device, &info, nullptr, &next.image) != VK_SUCCESS) {
        last_error = "texture image";
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device, next.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(dev.device, &alloc, nullptr, &next.memory) != VK_SUCCESS ||
        vkBindImageMemory(dev.device, next.image, next.memory, 0) != VK_SUCCESS) {
        DestroyTexture(next);
        last_error = "texture memory";
        return false;
    }
    // Staging buffer.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    binfo.size = bytes;
    binfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev.device, &binfo, nullptr, &staging) != VK_SUCCESS) {
        DestroyTexture(next);
        last_error = "staging buffer";
        return false;
    }
    vkGetBufferMemoryRequirements(dev.device, staging, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(dev.device, &alloc, nullptr, &staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, staging, staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, staging_memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, staging, nullptr);
        if (staging_memory) {
            vkFreeMemory(dev.device, staging_memory, nullptr);
        }
        DestroyTexture(next);
        last_error = "staging memory";
        return false;
    }
    std::memcpy(mapped, pixels, bytes);
    vkUnmapMemory(dev.device, staging_memory);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    auto barrier = [&](uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = next.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, next.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    int32_t w = int32_t(width), h = int32_t(height);
    for (uint32_t mip = 1; mip < mips; ++mip) {
        barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cmd, next.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, next.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (dev.lock) {
        dev.lock(dev.lock_user, 1);
    }
    const VkResult result = vkQueueSubmit(dev.queue, 1, &submit, fence);
    if (dev.lock) {
        dev.lock(dev.lock_user, 0);
    }
    bool ok = result == VK_SUCCESS;
    if (ok) {
        vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &fence);
    }
    vkDestroyBuffer(dev.device, staging, nullptr);
    vkFreeMemory(dev.device, staging_memory, nullptr);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = next.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (!ok || vkCreateImageView(dev.device, &view, nullptr, &next.view) != VK_SUCCESS) {
        DestroyTexture(next);
        last_error = "texture upload";
        return false;
    }
    // Earlier frames finished (Renderer waits for each), so the old texture is free.
    DestroyTexture(texture);
    texture = next;
    VkDescriptorImageInfo image_info{sampler, texture.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
    return true;
}

bool UiRenderer::SetGeometry(const UiCanvas& canvas) {
    const size_t count = canvas.world.size() + canvas.top.size();
    world_count = top_count = 0;
    if (!count) {
        return true;
    }
    const size_t bytes = count * sizeof(UiVertex);
    if (bytes > vertex_capacity) {
        if (vertex_buffer) {
            vkDestroyBuffer(dev.device, vertex_buffer, nullptr);
            vkFreeMemory(dev.device, vertex_memory, nullptr);
            vertex_buffer = VK_NULL_HANDLE;
            vertex_memory = VK_NULL_HANDLE;
            vertex_capacity = 0;
        }
        size_t capacity = 64 * 1024;
        while (capacity < bytes) {
            capacity *= 2;
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vkCreateBuffer(dev.device, &info, nullptr, &vertex_buffer) != VK_SUCCESS) {
            last_error = "ui vertex buffer";
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev.device, vertex_buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex =
            MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(dev.device, &alloc, nullptr, &vertex_memory) != VK_SUCCESS ||
            vkBindBufferMemory(dev.device, vertex_buffer, vertex_memory, 0) != VK_SUCCESS ||
            vkMapMemory(dev.device, vertex_memory, 0, capacity, 0, &vertex_mapped) !=
                VK_SUCCESS) {
            last_error = "ui vertex memory";
            return false;
        }
        vertex_capacity = capacity;
    }
    auto* out = static_cast<UiVertex*>(vertex_mapped);
    if (!canvas.world.empty()) {
        std::memcpy(out, canvas.world.data(), canvas.world.size() * sizeof(UiVertex));
    }
    if (!canvas.top.empty()) {
        std::memcpy(out + canvas.world.size(), canvas.top.data(),
                    canvas.top.size() * sizeof(UiVertex));
    }
    world_count = uint32_t(canvas.world.size());
    top_count = uint32_t(canvas.top.size());
    return true;
}

VkPipeline UiRenderer::CreatePipeline(bool depth_test, VkFormat color, VkFormat depth) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    const VkVertexInputBindingDescription binding{0, uint32_t(sizeof(UiVertex)),
                                                  VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(UiVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, shape)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, shape2)}};
    VkPipelineVertexInputStateCreateInfo input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 5;
    input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Premultiplied color; the destination alpha stays (the compositor needs 1), or accumulates
    // coverage into a transparent layer (SetAccumulateAlpha).
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = accumulate_alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor =
        accumulate_alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = depth;
    rendering.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_state;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

const UiRenderer::Pipelines* UiRenderer::PipelinesFor(VkFormat color, VkFormat depth) {
    const uint64_t key = (uint64_t(color) << 32) | uint64_t(depth);
    const auto it = pipelines.find(key);
    if (it != pipelines.end()) {
        return &it->second;
    }
    Pipelines p;
    p.depth_tested = CreatePipeline(true, color, depth);
    p.on_top = CreatePipeline(false, color, depth);
    if (!p.depth_tested || !p.on_top) {
        last_error = "ui pipeline creation failed";
        return nullptr;
    }
    return &(pipelines[key] = p);
}

void UiRenderer::Draw(VkCommandBuffer c, const Mat4& view_proj, VkFormat color, VkFormat depth) {
    if (!world_count && !top_count) {
        return;
    }
    const Pipelines* pipes = PipelinesFor(color, depth);
    if (!pipes || !vertex_buffer) {
        return;
    }
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(c, 0, 1, &vertex_buffer, &zero);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (world_count) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->depth_tested);
        vkCmdPushConstants(c, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, view_proj.m);
        vkCmdDraw(c, world_count, 1, 0, 0);
    }
    if (top_count) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->on_top);
        vkCmdPushConstants(c, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, view_proj.m);
        vkCmdDraw(c, top_count, 1, world_count, 0);
    }
}

} // namespace sb
