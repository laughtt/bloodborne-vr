// Command buffers copied at submit (gpu/shadps4/video_core/amdgpu/pm4_snapshot.h): the GPU
// thread parses what the guest submitted even when the guest rewrites or frees its memory
// before that thread gets to it (docs/vr/findings/43-pm4-race.md).
// Build: ninja -C out/gpu pm4-snapshot-test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/video_core/amdgpu/pm4_snapshot.h"

using namespace AmdGpu::Pm4;

namespace {

constexpr u32 Type3(u32 opcode, u32 body_dwords) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8);
}
constexpr u32 Nop(u32 body_dwords) {
    return Type3(0x10, body_dwords);
}
constexpr u32 OpSetContextReg = 0x69;
constexpr u32 OpWriteData = 0x37;

void AppendIndirect(std::vector<u32>& buffer, const std::vector<u32>& target) {
    const auto address = reinterpret_cast<std::uintptr_t>(target.data());
    buffer.push_back(Type3(OpIndirectBuffer, 3));
    buffer.push_back(static_cast<u32>(address));
    buffer.push_back(static_cast<u32>(address >> 32) & 0xFFFF);
    buffer.push_back(static_cast<u32>(target.size()));
}

// The headers of the crashes ("Unimplemented PM4 type 0, base reg: 41162, size: 4372", base
// 2 / 0 with size 1, "Wrong PM4 type 1") are rejected; real packets and padding are not.
void TestHeaderCheck() {
    assert(CheckHeader(0x00000000u, 8) == HeaderCheck::BadType);
    assert(CheckHeader(0x00000002u, 8) == HeaderCheck::BadType);
    assert(CheckHeader(0x1113A0CAu, 8) == HeaderCheck::BadType); // base 0xA0CA, 4372 words
    assert(CheckHeader(0x4000A000u, 8) == HeaderCheck::BadType); // type 1
    assert(CheckHeader(0x80000000u, 1) == HeaderCheck::Padding); // type 2
    assert(CheckHeader(Nop(1), 2) == HeaderCheck::Packet);
    assert(CheckHeader(Nop(1), 1) == HeaderCheck::Truncated);
    assert(CheckHeader(Type3(OpSetContextReg, 3), 4) == HeaderCheck::Packet);
    assert(CheckHeader(Type3(OpSetContextReg, 3), 3) == HeaderCheck::Truncated);
    assert(Type3Dwords(Type3(OpWriteData, 4)) == 5);
    assert(Type3Opcode(Type3(OpRewind, 1)) == OpRewind);
    assert(PacketType(0x1113A0CAu) == 0 && PacketType(0xC0001000u) == 3);
}

// The race: the guest submits, then (after its stall timeout, at the world load) clears or
// reuses the buffer before the GPU thread parsed it. The copy keeps what was submitted, and the
// per-packet comparison sees the rewrite.
void TestCopyOutlivesGuestRewrite() {
    std::vector<u32> guest = {Type3(OpSetContextReg, 3), 0x318, 0x1234, 0x5678, Nop(2), 0, 0,
                              Type3(OpWriteData, 4), 0x500, 0x1000, 0, 1};
    const std::vector<u32> submitted = guest;
    std::size_t budget = SnapshotLimits{}.max_dwords;
    auto copy = TakeSnapshot(std::span<const u32>{guest}, OpIndirectBuffer,
                             [](std::uintptr_t, u64) { return true; }, SnapshotLimits{}, budget);
    assert(copy->size == guest.size());
    assert(copy->guest == guest.data());
    assert(copy->words.size() == guest.size() + CommandSnapshot::Padding);
    for (std::size_t i = 0; i < CommandSnapshot::Padding; ++i) {
        assert(copy->words[guest.size() + i] == 0); // lookahead past the last packet stays inside
    }

    std::fill(guest.begin() + 4, guest.end(), 0u); // the guest reused the buffer's tail
    const auto span = copy->Span();
    assert(std::equal(span.begin(), span.end(), submitted.begin(), submitted.end()));
    // The first packet is unchanged; the NOP at +4 now reads as "PM4 type 0" in guest memory.
    assert(FirstDifference(&span[0], &guest[0], 4) == 4);
    assert(FirstDifference(&span[4], &guest[4], 3) == 0);
    assert(CheckHeader(guest[4], guest.size() - 4) == HeaderCheck::BadType);
    assert(CheckHeader(span[4], span.size() - 4) == HeaderCheck::Packet);
    assert(copy->GuestAt(&span[4]) == &guest[4]);
    assert(copy->OffsetOf(&span[7]) == 7);
    assert(copy->Contains(&span[11]) && !copy->Contains(span.data() + span.size()));
}

// Indirect buffers: copied (recursively) only when readable and within the limits; the parser
// finds a copy by the packet's offset and checks its address and size.
void TestIndirectBuffers() {
    std::vector<u32> leaf = {Nop(1), 7};
    std::vector<u32> middle = {Nop(1), 9};
    AppendIndirect(middle, leaf);
    std::vector<u32> top = {Nop(1), 1};
    AppendIndirect(top, middle);
    top.push_back(0x80000000u); // padding
    AppendIndirect(top, leaf);

    std::size_t budget = SnapshotLimits{}.max_dwords;
    auto copy = TakeSnapshot(std::span<const u32>{top}, OpIndirectBuffer,
                             [](std::uintptr_t, u64) { return true; }, SnapshotLimits{}, budget);
    assert(copy->children.size() == 2);
    assert(copy->children[0].offset == 2 && copy->children[1].offset == 7);
    const CommandSnapshot* mid = copy->ChildFor(&copy->words[2], ReadIndirect(&copy->words[2]));
    assert(mid && mid->guest == middle.data() && mid->size == middle.size());
    assert(mid->children.size() == 1 && mid->children[0].snapshot->guest == leaf.data());
    const CommandSnapshot* second = copy->ChildFor(&copy->words[7], ReadIndirect(&copy->words[7]));
    assert(second && second->guest == leaf.data());
    leaf[1] = 0xDEAD; // rewritten after submit: the copy keeps the submitted word
    assert(second->words[1] == 7);
    // Wrong offset or a different address/size: no copy (the parser reads guest memory).
    assert(!copy->ChildFor(&copy->words[0], ReadIndirect(&copy->words[2])));
    IndirectRef other = ReadIndirect(&copy->words[2]);
    other.size += 1;
    assert(!copy->ChildFor(&copy->words[2], other));
    assert(!copy->ChildFor(&top[2], ReadIndirect(&copy->words[2]))); // not in the copy

    // Depth 1 (the default in the emulator: Bloodborne calls none): no children.
    budget = SnapshotLimits{}.max_dwords;
    SnapshotLimits flat;
    flat.max_depth = 1;
    auto shallow = TakeSnapshot(std::span<const u32>{top}, OpIndirectBuffer,
                                [](std::uintptr_t, u64) { return true; }, flat, budget);
    assert(shallow->children.empty());

    // Unreadable (unmapped) targets are not touched.
    budget = SnapshotLimits{}.max_dwords;
    int asked = 0;
    auto unmapped = TakeSnapshot(
        std::span<const u32>{top}, OpIndirectBuffer,
        [&](std::uintptr_t address, u64 bytes) {
            ++asked;
            assert(bytes % 4 == 0);
            return address != reinterpret_cast<std::uintptr_t>(middle.data());
        },
        SnapshotLimits{}, budget);
    assert(asked == 2 && unmapped->children.size() == 1 && unmapped->children[0].offset == 7);

    // The budget bounds what one submission copies.
    std::size_t small = top.size() + 1;
    auto bounded = TakeSnapshot(std::span<const u32>{top}, OpIndirectBuffer,
                                [](std::uintptr_t, u64) { return true; }, SnapshotLimits{},
                                small);
    assert(bounded->children.empty());

    // Constant engine buffers call IndirectBufferConst, not IndirectBuffer.
    budget = SnapshotLimits{}.max_dwords;
    auto ce = TakeSnapshot(std::span<const u32>{top}, OpIndirectBufferConst,
                           [](std::uintptr_t, u64) { return true; }, SnapshotLimits{}, budget);
    assert(ce->children.empty());
}

// A scan stops at the first header that is no packet, as the parser does.
void TestScanStopsAtGarbage() {
    std::vector<u32> leaf = {Nop(1), 7};
    std::vector<u32> top = {0x1113A0CAu, 0, 0, 0};
    AppendIndirect(top, leaf);
    std::size_t budget = SnapshotLimits{}.max_dwords;
    auto copy = TakeSnapshot(std::span<const u32>{top}, OpIndirectBuffer,
                             [](std::uintptr_t, u64) { return true; }, SnapshotLimits{}, budget);
    assert(copy->children.empty() && copy->size == top.size());
}

} // namespace

int main() {
    TestHeaderCheck();
    TestCopyOutlivesGuestRewrite();
    TestIndirectBuffers();
    TestScanStopsAtGarbage();
    std::puts("pm4 snapshot tests passed");
    return 0;
}
