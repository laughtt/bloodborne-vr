// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: VR Phase 4 prototype (docs/vr/findings/23-phase4-renderer-stereo.md). Per-eye clip
// position for perspective draws: x' = x + offset, with the offset per draw from a special
// buffer (BufferType::StereoView, filled by Rasterizer::BindBuffers). Both eyes use the game's
// symmetric projection P and differ from the center camera by a translation e along the camera's
// right axis, so P * T(e) * P^-1 reduces to clip.x -= P00 * e: one add, exact for every vertex the
// game transformed with the center camera, whatever buffer its matrices came from.
//
// Only perspective draws are changed: the clip w must depend on buffer data (a camera matrix).
// Screen-space draws (full-screen passes, light rectangles, UI) write w from vertex attributes or
// constants and keep their positions. Everything computed from the clip x (varyings that copy it,
// which the game's pixel shaders turn into screen coordinates, and screen coordinates the vertex
// shader derives itself) uses the eye's value.

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

namespace {

bool IsMemoryLoad(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::LoadBufferU32:
    case IR::Opcode::LoadBufferU32x2:
    case IR::Opcode::LoadBufferU32x3:
    case IR::Opcode::LoadBufferU32x4:
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::LoadBufferF32x2:
    case IR::Opcode::LoadBufferF32x3:
    case IR::Opcode::LoadBufferF32x4:
    case IR::Opcode::LoadBufferFormatF32:
        return true;
    default:
        return false;
    }
}

/// Whether `value` is computed from data loaded from a buffer or constant memory.
bool DependsOnMemory(const IR::Value& value, std::unordered_set<const IR::Inst*>& seen) {
    const IR::Inst* inst = value.TryInst();
    if (!inst || !seen.insert(inst).second) {
        return false;
    }
    if (IsMemoryLoad(inst->GetOpcode())) {
        return true;
    }
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (DependsOnMemory(inst->Arg(i), seen)) {
            return true;
        }
    }
    return false;
}

/// Whether `inst` only computes clip distances (user clip planes, LowerUserClipPlanes, or a
/// guest export): plane . clip. The planes are given in the center camera's clip space, so they
/// are evaluated on the unshifted position: the same distance per point for both eyes.
bool FeedsOnlyClipDistance(const IR::Inst* inst, u32 depth = 0) {
    if (depth > 16 || !inst->HasUses()) {
        return false;
    }
    for (const IR::Use& use : inst->Uses()) {
        const IR::Inst* user = use.user;
        switch (user->GetOpcode()) {
        case IR::Opcode::SetAttribute:
            if (user->Arg(0).Attribute() != IR::Attribute::ClipDistance) {
                return false;
            }
            break;
        case IR::Opcode::FPAdd32:
        case IR::Opcode::FPMul32:
        case IR::Opcode::FPFma32:
            if (!FeedsOnlyClipDistance(user, depth + 1)) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

/// x' = x + StereoView[0] right after the definition of x (after the phis of its block, so it
/// dominates every store of x); every use of x but clip distances then reads x'. Returns the
/// number of uses rewritten.
u32 ShiftClipX(IR::Inst* def, u32 binding) {
    IR::Block* const block = def->GetParent();
    auto it = std::next(IR::Block::InstructionList::s_iterator_to(*def));
    while (it != block->end() && it->GetOpcode() == IR::Opcode::Phi) {
        ++it;
    }
    IR::IREmitter ir{*block, it};
    const IR::F32 offset{ir.LoadBufferF32(1, ir.Imm32(binding), ir.Imm32(0u), {})};
    const IR::F32 shifted{ir.FPAdd(IR::F32{IR::Value{def}}, offset)};

    // Every use of the clip x sees the eye's value: the position, varyings that copy it, and
    // varyings derived from it (soft particles and distortion compute 0.5 * x / w + 0.5 screen
    // coordinates in the vertex shader; 31 of the game's 275 vertex-stage shaders do). Not the
    // clip distances (P4.1, FeedsOnlyClipDistance).
    IR::Inst* const add = shifted.Inst();
    std::vector<IR::Use> uses;
    for (const IR::Use& use : def->Uses()) {
        if (use.user != add && use.user != offset.Inst() && !FeedsOnlyClipDistance(use.user)) {
            uses.push_back(use);
        }
    }
    for (const IR::Use& use : uses) {
        use.user->SetArg(use.operand, shifted);
    }
    return static_cast<u32>(uses.size());
}

} // namespace

void LowerStereoRemap(IR::Program& program, const RuntimeInfo& runtime_info) {
    Info& info = program.info;
    const bool vertex = info.hw_stage == HwStage::Vertex && runtime_info.hw.vs.stereo_remap;
    const bool geometry = info.hw_stage == HwStage::Geometry && runtime_info.hw.gs.stereo_remap;
    if (!vertex && !geometry) {
        return;
    }
    const char* const stage = vertex ? "vs" : "gs";
    // The clip x and w of every position store. A vertex shader has one (exports are not in
    // branches); a geometry shader stores the position before each EmitVertex.
    std::vector<IR::Value> xs, ws;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                inst.Arg(0).Attribute() != IR::Attribute::Position0) {
                continue;
            }
            const u32 element = inst.Arg(2).U32();
            if (element == 0) {
                xs.push_back(inst.Arg(1));
            } else if (element == 3) {
                ws.push_back(inst.Arg(1));
            }
        }
    }
    const auto log = [&](const char* what) {
        std::printf("Stereo: %s %016llx %s\n", stage,
                    static_cast<unsigned long long>(info.pgm_hash), what);
    };
    if (xs.empty() || ws.empty() || (vertex && xs.size() != 1)) {
        log("not remapped (position stores)");
        return;
    }
    // Perspective: every clip w comes from memory (a camera matrix). Screen-space draws
    // (full-screen passes, light rectangles, UI) take w from vertex data or immediates.
    for (const IR::Value& w : ws) {
        std::unordered_set<const IR::Inst*> seen;
        if (!DependsOnMemory(w, seen)) {
            log("screen-space (w not from memory)");
            return;
        }
    }
    std::vector<IR::Inst*> defs;
    for (const IR::Value& x : xs) {
        IR::Inst* const def = x.TryInst();
        if (!def) {
            log("not remapped (immediate x)");
            return;
        }
        if (std::ranges::find(defs, def) == defs.end()) {
            defs.push_back(def);
        }
    }

    const u32 binding = static_cast<u32>(info.buffers.size());
    info.buffers.push_back(BufferResource{
        .used_types = IR::Type::F32,
        .buffer_type = BufferType::StereoView,
    });
    u32 uses = 0;
    for (IR::Inst* const def : defs) {
        uses += ShiftClipX(def, binding);
    }
    char what[64];
    std::snprintf(what, sizeof(what), "perspective (%zu x, %u uses shifted)", defs.size(), uses);
    log(what);
}

} // namespace Shader::Optimization
