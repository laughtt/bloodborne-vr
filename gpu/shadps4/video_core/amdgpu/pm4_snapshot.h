// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: command buffers copied at submission (docs/vr/findings/43-pm4-race.md).
//
// The GPU command thread parses a submission later than the guest submits it: behind other
// queues, pipeline compiles, the draw pipe, the presenter (BB_FRAMES_AHEAD). The guest owns its
// command memory again once it believes the GPU is done with it, and Bloodborne's GX code does
// not wait forever: after a stall ("== Stall during rendering at flush N") it can rewrite or free
// the buffer. Parsing guest memory then read whatever the guest wrote next (zeros, data) as PM4:
// "Unimplemented PM4 type 0". The parser reads these copies instead, including the indirect
// buffers the copy points at, so what it decodes is what the guest submitted.
//
// Dependency-free (no shadPS4 headers) so tests/test_pm4_snapshot.cpp builds on its own.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace AmdGpu::Pm4 {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 OpIndirectBufferConst = 0x33;
constexpr u32 OpIndirectBuffer = 0x3F;
constexpr u32 OpRewind = 0x59;

constexpr u32 PacketType(u32 header) {
    return header >> 30;
}
constexpr u32 Type3Opcode(u32 header) {
    return (header >> 8) & 0xFF;
}
/// Header plus body, as the parser advances (PM4Type3Header::NumWords() + 1).
constexpr u32 Type3Dwords(u32 header) {
    return ((((header >> 16) & 0x3FFF) + 1) & 0x3FFF) + 1;
}

enum class HeaderCheck {
    Packet,    ///< type 3, fits in the buffer
    Padding,   ///< type 2: one dword
    BadType,   ///< type 0 or 1: never in Gnm command buffers; garbage
    Truncated, ///< type 3 longer than what is left of the buffer
};

constexpr HeaderCheck CheckHeader(u32 header, std::size_t remaining_dwords) {
    switch (PacketType(header)) {
    case 2:
        return HeaderCheck::Padding;
    case 3:
        return Type3Dwords(header) <= remaining_dwords ? HeaderCheck::Packet
                                                         : HeaderCheck::Truncated;
    default:
        return HeaderCheck::BadType;
    }
}

constexpr const char* HeaderCheckName(HeaderCheck check) {
    switch (check) {
    case HeaderCheck::Packet:
        return "packet";
    case HeaderCheck::Padding:
        return "padding";
    case HeaderCheck::BadType:
        return "invalid packet type";
    case HeaderCheck::Truncated:
        return "packet runs past the end of the buffer";
    }
    return "?";
}

/// Indirect buffer packet: header, base lo, base hi (16 bits), size in dwords (20 bits).
struct IndirectRef {
    const u32* address;
    u32 size;
};
inline IndirectRef ReadIndirect(const u32* packet) {
    const u64 address = (u64(packet[2] & 0xFFFF) << 32) | packet[1];
    return {reinterpret_cast<const u32*>(address), packet[3] & 0xFFFFF};
}

/// A command buffer as submitted, and copies of the indirect buffers it calls.
struct CommandSnapshot {
    /// Zero dwords after the copy: the parser looks a few dwords past a packet (the NOP hints
    /// after render target setup), which must not leave the allocation.
    static constexpr std::size_t Padding = 64;

    const u32* guest = nullptr; ///< the guest memory the words were copied from
    std::vector<u32> words;     ///< `size` dwords, then Padding zero dwords
    std::size_t size = 0;       ///< dwords copied
    std::chrono::steady_clock::time_point taken{}; ///< when (the submit)
    struct Child {
        u32 offset; ///< dword offset of the IndirectBuffer packet in `words`
        std::unique_ptr<CommandSnapshot> snapshot;
    };
    std::vector<Child> children; ///< ascending offsets

    std::span<const u32> Span() const {
        return {words.data(), size};
    }
    bool Contains(const void* p) const {
        const auto* w = static_cast<const u32*>(p);
        return w >= words.data() && w < words.data() + size;
    }
    u32 OffsetOf(const void* p) const {
        return static_cast<u32>(static_cast<const u32*>(p) - words.data());
    }
    /// The guest address a word of the copy came from.
    const u32* GuestAt(const void* p) const {
        return guest + OffsetOf(p);
    }
    /// The copy of the indirect buffer called by the packet at `packet` (in `words`), if it was
    /// taken and still matches the packet's address and size.
    const CommandSnapshot* ChildFor(const void* packet, IndirectRef ref) const {
        if (!Contains(packet)) {
            return nullptr;
        }
        const u32 offset = OffsetOf(packet);
        const auto it = std::lower_bound(children.begin(), children.end(), offset,
                                         [](const Child& c, u32 o) { return c.offset < o; });
        if (it == children.end() || it->offset != offset || !it->snapshot ||
            it->snapshot->guest != ref.address || it->snapshot->size != ref.size) {
            return nullptr;
        }
        return it->snapshot.get();
    }
};

struct SnapshotLimits {
    u32 max_depth = 4;                    ///< nesting of indirect buffers copied
    std::size_t max_dwords = 16u << 20;   ///< per submission, all levels (64 MiB)
};

/// Copies `guest` and, recursively, the indirect buffers it calls (opcode `ib_opcode`: 0x3F for
/// the draw engine, 0x33 for the constant engine). `readable(address, bytes)` says whether an
/// indirect buffer may be read now; one that may not (or past the limits) is left to the parser,
/// which then reads guest memory as before.
template <typename Readable>
std::unique_ptr<CommandSnapshot> TakeSnapshot(std::span<const u32> guest, u32 ib_opcode,
                                              Readable&& readable, const SnapshotLimits& limits,
                                              std::size_t& budget, u32 depth = 0) {
    auto snap = std::make_unique<CommandSnapshot>();
    snap->guest = guest.data();
    snap->size = guest.size();
    snap->taken = std::chrono::steady_clock::now();
    snap->words.resize(guest.size() + CommandSnapshot::Padding);
    if (!guest.empty()) {
        std::memcpy(snap->words.data(), guest.data(), guest.size_bytes());
    }
    budget = budget > guest.size() ? budget - guest.size() : 0;
    if (depth + 1 >= limits.max_depth) {
        return snap;
    }
    const auto& words = snap->words;
    for (std::size_t at = 0; at < snap->size;) {
        const u32 header = words[at];
        const HeaderCheck check = CheckHeader(header, snap->size - at);
        if (check == HeaderCheck::Padding) {
            ++at;
            continue;
        }
        if (check != HeaderCheck::Packet) {
            break; // the parser reports it
        }
        const u32 dwords = Type3Dwords(header);
        if (Type3Opcode(header) == ib_opcode && dwords >= 4) {
            const IndirectRef ref = ReadIndirect(&words[at]);
            if (ref.address && ref.size && ref.size <= budget &&
                readable(reinterpret_cast<std::uintptr_t>(ref.address), u64(ref.size) * 4)) {
                auto child = TakeSnapshot(std::span<const u32>{ref.address, ref.size}, ib_opcode,
                                          readable, limits, budget, depth + 1);
                snap->children.push_back({static_cast<u32>(at), std::move(child)});
            }
        }
        at += dwords;
    }
    return snap;
}

/// First dword where `live` differs from `copy` (count dwords), or count if equal.
inline std::size_t FirstDifference(const u32* copy, const volatile u32* live, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (copy[i] != live[i]) {
            return i;
        }
    }
    return count;
}

} // namespace AmdGpu::Pm4
