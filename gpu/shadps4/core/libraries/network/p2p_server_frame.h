// Framing of the UDP packets between the P2P port and the shadNet matching server (STUN ping and
// its echo), and the check of the address the echo reports. Pure and standalone (no project
// includes) so a unit test can include it.
//
// Peer-to-peer frames on the P2P port carry a 4-byte header (source vport, destination vport;
// 0xFFFF/0xFFFF for internal signaling). The shadNet STUN server instead expects and answers a
// 6-byte header FF 83 FF FF FF FF. A ping sent with the 4-byte header never matched it (the
// server dropped every ping, so it never learned anyone's public endpoint), and its echo came back
// to the client as an 8-byte payload (FF FF + ip + port) that the 6-byte echo check never matched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Libraries::Net::ServerFrame {

inline constexpr uint8_t kHeader[6] = {0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF};
inline constexpr size_t kHeaderSize = sizeof(kHeader);
inline constexpr size_t kEchoSize = 6; // ext ip (4, network order) + ext port (2, network order)

inline bool HasHeader(const uint8_t* buf, size_t size) {
    return buf != nullptr && size >= kHeaderSize && std::memcmp(buf, kHeader, kHeaderSize) == 0;
}

// Writes header + payload into out (capacity out_cap); returns the frame size, 0 if it does not fit.
inline size_t Build(const void* payload, size_t len, uint8_t* out, size_t out_cap) {
    if (out == nullptr || out_cap < kHeaderSize + len || (payload == nullptr && len != 0)) {
        return 0;
    }
    std::memcpy(out, kHeader, kHeaderSize);
    if (len != 0) {
        std::memcpy(out + kHeaderSize, payload, len);
    }
    return kHeaderSize + len;
}

// Parses an echo payload (header already stripped). Both values stay in network order.
inline bool ParseEcho(const uint8_t* payload, size_t size, uint32_t* ip_nbo, uint16_t* port_nbo) {
    if (payload == nullptr || size != kEchoSize || ip_nbo == nullptr || port_nbo == nullptr) {
        return false;
    }
    std::memcpy(ip_nbo, payload, 4);
    std::memcpy(port_nbo, payload + 4, 2);
    return *ip_nbo != 0 && *port_nbo != 0;
}

// a.b.c.d of a network-order IPv4 address (the first octet is the lowest byte in memory).
inline uint8_t Octet(uint32_t ip_nbo, int i) {
    uint8_t b[4];
    std::memcpy(b, &ip_nbo, 4);
    return b[i];
}

inline bool IsPrivateOrLocal(uint32_t ip_nbo) {
    const uint8_t a = Octet(ip_nbo, 0);
    const uint8_t b = Octet(ip_nbo, 1);
    return a == 0 || a == 10 || a == 127 || (a == 169 && b == 254) ||
           (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127) /* carrier-grade NAT */;
}

// Whether the address the server saw may be advertised as this PC's external address.
// A public address is the NAT's outside (normal internet case). A private address is right only
// when it is this PC's own LAN address (server on the same LAN, no NAT between). Any other private
// address is a NAT in front of the server, e.g. Docker Desktop's bridge gateway (172.x.0.1, which
// every client on the PC shares and nobody can reach), so advertising it would break peers.
inline bool AcceptMappedAddr(uint32_t mapped_nbo, uint32_t own_lan_nbo) {
    if (mapped_nbo == 0) {
        return false;
    }
    if (!IsPrivateOrLocal(mapped_nbo)) {
        return true;
    }
    return own_lan_nbo != 0 && mapped_nbo == own_lan_nbo;
}

} // namespace Libraries::Net::ServerFrame
