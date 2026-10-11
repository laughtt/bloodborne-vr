// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"

namespace Libraries::Np::NpMatching2 {

struct ContextObject;

void StartMatching2HandshakeThread();
void StopMatching2HandshakeThread();

bool SendMatching2StunPing(const ContextObject& ctx);

// Same-PC peer registry: instances on one PC publish their P2P UDP port so peers can be
// resolved when the server only knows a NATed endpoint (shadNet in Docker) or none at all.
void PublishLocalPeer(std::string_view online_id);
bool LookupLocalPeer(std::string_view online_id, u32* out_addr_nbo, u16* out_port_nbo);

void StartMatching2PeerHandshake(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                 OrbisNpMatching2RoomMemberId member_id);
void StartMatching2SignalingForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id);
void QueueMatching2DeadForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                    s32 error_code);
void QueueMatching2SignalingEvent(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                  OrbisNpMatching2RoomMemberId member_id,
                                  OrbisNpMatching2Event event, s32 error_code);

u32 GetRoomPingUs(const ContextObject& ctx, OrbisNpMatching2RoomId roomId);
void* BuildSignalingGetPingInfoPayload(ContextObject& ctx, OrbisNpMatching2RoomId roomId);
s32 FillMatching2ConnectionInfo(const ContextObject& ctx, OrbisNpMatching2RoomId roomId,
                                OrbisNpMatching2RoomMemberId memberId, u32 infoType, void* connInfo,
                                bool a_variant);

} // namespace Libraries::Np::NpMatching2
