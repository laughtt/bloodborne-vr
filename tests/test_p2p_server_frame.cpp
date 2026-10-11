// Unit test of the P2P <-> matching server framing (p2p_server_frame.h): the STUN ping the client
// sends, the echo the server answers, and which echoed address is advertised. It replays both
// sides on byte buffers, including the old 4-byte-header framing that the server dropped.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/libraries/network/p2p_server_frame.h"

using namespace Libraries::Net::ServerFrame;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

static uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t bytes[4] = {a, b, c, d};
    uint32_t v;
    std::memcpy(&v, bytes, 4);
    return v; // network order
}

static uint16_t PortNbo(uint16_t host) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(host >> 8), static_cast<uint8_t>(host)};
    uint16_t v;
    std::memcpy(&v, bytes, 2);
    return v;
}

// The shadNet STUN server's view (stun_server.cpp): drop anything without its header, then
// cmd 0x01 with >= 21 bytes is a ping. Returns the online id it would record, "" if dropped.
static std::string ServerAcceptsPing(const std::vector<uint8_t>& datagram) {
    if (datagram.size() < kHeaderSize || std::memcmp(datagram.data(), kHeader, kHeaderSize) != 0) {
        return {};
    }
    const uint8_t* data = datagram.data() + kHeaderSize;
    const size_t size = datagram.size() - kHeaderSize;
    if (size < 21 || data[0] != 0x01) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data + 1),
                       strnlen(reinterpret_cast<const char*>(data + 1), 16));
}

// The server's reply: header + ext ip (4) + ext port (2), both network order.
static std::vector<uint8_t> ServerEcho(uint32_t ip_nbo, uint16_t port_nbo) {
    std::vector<uint8_t> out(kHeader, kHeader + kHeaderSize);
    const auto* ip = reinterpret_cast<const uint8_t*>(&ip_nbo);
    const auto* port = reinterpret_cast<const uint8_t*>(&port_nbo);
    out.insert(out.end(), ip, ip + 4);
    out.insert(out.end(), port, port + 2);
    return out;
}

static std::vector<uint8_t> Ping(const char* online_id) {
    std::vector<uint8_t> ping(21, 0);
    ping[0] = 0x01;
    std::memcpy(ping.data() + 1, online_id, std::strlen(online_id));
    return ping;
}

int main() {
    // --- client -> server: the ping ---
    const auto ping = Ping("FriendTwo");
    uint8_t frame[64];
    const size_t n = Build(ping.data(), ping.size(), frame, sizeof(frame));
    CHECK(n == 27);
    CHECK(ServerAcceptsPing(std::vector<uint8_t>(frame, frame + n)) == "FriendTwo");
    // the old framing (4-byte vport header FF FF FF FF): the server dropped every ping
    std::vector<uint8_t> old_frame = {0xFF, 0xFF, 0xFF, 0xFF};
    old_frame.insert(old_frame.end(), ping.begin(), ping.end());
    CHECK(ServerAcceptsPing(old_frame).empty());
    // too small a buffer / null payload with length
    CHECK(Build(ping.data(), ping.size(), frame, 26) == 0);
    CHECK(Build(nullptr, 3, frame, sizeof(frame)) == 0);
    CHECK(Build(nullptr, 0, frame, sizeof(frame)) == kHeaderSize);

    // --- server -> client: the echo ---
    const uint32_t pub = Ip(203, 0, 113, 7);
    const auto echo = ServerEcho(pub, PortNbo(50123));
    CHECK(echo.size() == 12);
    CHECK(HasHeader(echo.data(), echo.size()));
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(ParseEcho(echo.data() + kHeaderSize, echo.size() - kHeaderSize, &ip, &port));
    CHECK(ip == pub);
    CHECK(port == PortNbo(50123));
    // the old client stripped only the 4-byte vport header: 8 bytes (FF FF + echo), no echo
    CHECK(!ParseEcho(echo.data() + 4, echo.size() - 4, &ip, &port));
    // zero address / port, short, long
    const auto zero = ServerEcho(0, PortNbo(1));
    CHECK(!ParseEcho(zero.data() + kHeaderSize, 6, &ip, &port));
    const auto zport = ServerEcho(pub, 0);
    CHECK(!ParseEcho(zport.data() + kHeaderSize, 6, &ip, &port));
    CHECK(!ParseEcho(echo.data() + kHeaderSize, 5, &ip, &port));
    CHECK(!ParseEcho(echo.data() + kHeaderSize, 7, &ip, &port));

    // --- peer frames are not server frames ---
    // internal signaling (vports FFFF/FFFF) + "SHAD"
    const uint8_t sig[] = {0xFF, 0xFF, 0xFF, 0xFF, 'S', 'H', 'A', 'D', 0x07};
    CHECK(!HasHeader(sig, sizeof(sig)));
    // game frame: vports 30000 -> 30001 (network order)
    const uint8_t game[] = {0x75, 0x30, 0x75, 0x31, 1, 2, 3, 4};
    CHECK(!HasHeader(game, sizeof(game)));
    CHECK(!HasHeader(kHeader, kHeaderSize - 1));
    CHECK(!HasHeader(nullptr, 10));

    // --- which echoed address is advertised ---
    const uint32_t lan = Ip(192, 168, 1, 20);
    CHECK(AcceptMappedAddr(pub, lan));                        // internet: the NAT's outside
    CHECK(AcceptMappedAddr(pub, 0));                          // LAN address unknown
    CHECK(AcceptMappedAddr(lan, lan));                        // server on the same LAN, no NAT
    CHECK(!AcceptMappedAddr(Ip(172, 19, 0, 1), lan));         // Docker Desktop gateway
    CHECK(!AcceptMappedAddr(Ip(192, 168, 1, 1), lan));        // a router between
    CHECK(!AcceptMappedAddr(Ip(10, 0, 0, 5), lan));
    CHECK(!AcceptMappedAddr(Ip(100, 72, 3, 4), lan));         // carrier-grade NAT
    CHECK(!AcceptMappedAddr(Ip(127, 0, 0, 1), lan));          // loopback
    CHECK(!AcceptMappedAddr(Ip(169, 254, 9, 9), lan));        // link-local
    CHECK(!AcceptMappedAddr(0, lan));
    CHECK(!AcceptMappedAddr(Ip(172, 19, 0, 1), 0));
    CHECK(AcceptMappedAddr(Ip(172, 32, 0, 1), lan));          // public (outside 172.16/12)
    CHECK(AcceptMappedAddr(Ip(100, 128, 0, 1), lan));         // public (outside 100.64/10)
    CHECK(Octet(pub, 0) == 203 && Octet(pub, 3) == 7);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
