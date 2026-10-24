#include "protocol/packets.h"

#include <algorithm>
#include <cstring>

namespace sv::proto {

namespace {

constexpr char artnet_id[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
constexpr std::uint8_t acn_id[12] = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0};
constexpr std::uint16_t op_poll = 0x2000, op_poll_reply = 0x2100, op_dmx = 0x5000, op_sync = 0x5200,
                        op_timecode = 0x9700;
constexpr std::uint32_t vector_root_data = 4, vector_root_extended = 8, vector_frame_data = 2, vector_sync = 1;

std::uint16_t be16(const std::uint8_t *p) { return std::uint16_t(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t *p) { return std::uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put16(Bytes &b, std::size_t at, std::uint16_t v) { b[at] = std::uint8_t(v >> 8), b[at + 1] = std::uint8_t(v); }
void put32(Bytes &b, std::size_t at, std::uint32_t v) {
    put16(b, at, std::uint16_t(v >> 16)), put16(b, at + 2, std::uint16_t(v));
}
void put_flags_length(Bytes &b, std::size_t at) { put16(b, at, std::uint16_t(0x7000 | (b.size() - at))); }

Bytes artnet_header(std::size_t size, std::uint16_t opcode) {
    Bytes b(size, 0);
    std::memcpy(b.data(), artnet_id, 8);
    b[8] = std::uint8_t(opcode), b[9] = std::uint8_t(opcode >> 8); // OpCode is little-endian
    return b;
}

Bytes acn_root(std::size_t size, std::uint32_t vector, const Cid &cid) {
    Bytes b(size, 0);
    put16(b, 0, 0x0010);
    std::memcpy(&b[4], acn_id, 12);
    put_flags_length(b, 16);
    put32(b, 18, vector);
    std::copy(cid.begin(), cid.end(), b.begin() + 22);
    return b;
}

} // namespace

ArtNetPacket parse_artnet(std::span<const std::uint8_t> p) {
    if (p.size() < 12 || std::memcmp(p.data(), artnet_id, 8) != 0)
        return {};
    const std::uint16_t op = std::uint16_t(p[8] | p[9] << 8);
    if (op == op_poll)
        return ArtPoll{};
    if (op == op_sync)
        return ArtSync{};
    if (op == op_timecode) {
        if (p.size() < 19 || p[14] > 29 || p[15] > 59 || p[16] > 59 || p[17] > 23 || p[18] > 3)
            return {};
        return ArtTimeCode{p[14], p[15], p[16], p[17], p[18]};
    }
    if (op != op_dmx || p.size() < 18)
        return {};
    const std::size_t length = std::min<std::size_t>({be16(&p[16]), 512, p.size() - 18});
    if (length < 1)
        return {};
    return ArtDmx{std::uint16_t((p[15] & 0x7F) << 8 | p[14]), p[12], p.subspan(18, length)};
}

SacnPacket parse_sacn(std::span<const std::uint8_t> p) {
    if (p.size() < 49 || be16(&p[0]) != 0x0010 || be16(&p[2]) != 0 || std::memcmp(&p[4], acn_id, 12) != 0)
        return {};
    Cid cid;
    std::copy(p.begin() + 22, p.begin() + 38, cid.begin());
    const std::uint32_t root = be32(&p[18]), frame = be32(&p[40]);
    if (root == vector_root_extended && frame == vector_sync)
        return SacnSync{cid, p[44], be16(&p[45])};
    if (root != vector_root_data || frame != vector_frame_data || p.size() < 126)
        return {};
    // DMP layer: vector 2, address/data type 0xA1, first address 0, increment 1.
    if (p[117] != 0x02 || p[118] != 0xA1 || be16(&p[119]) != 0 || be16(&p[121]) != 1)
        return {};
    const std::size_t count = be16(&p[123]); // start code + slots
    if (count < 1 || count > 513 || 125 + count > p.size())
        return {};
    const std::uint16_t universe = be16(&p[113]);
    if (universe < 1 || universe > 63999)
        return {};
    const char *name = reinterpret_cast<const char *>(&p[44]);
    return SacnData{cid,
                    std::string(name, strnlen(name, 64)),
                    std::min<std::uint8_t>(p[108], 200),
                    be16(&p[109]),
                    p[111],
                    (p[112] & 0x40) != 0,
                    universe,
                    p[125],
                    p.subspan(126, count - 1)};
}

Bytes make_artdmx(std::uint16_t port_address, std::uint8_t sequence, std::span<const std::uint8_t> slots) {
    const std::size_t n = std::min<std::size_t>(slots.size(), 512);
    Bytes b = artnet_header(18 + n, op_dmx);
    b[11] = 14; // ProtVer
    b[12] = sequence;
    b[14] = std::uint8_t(port_address & 0xFF);
    b[15] = std::uint8_t((port_address >> 8) & 0x7F);
    put16(b, 16, std::uint16_t(n));
    std::copy_n(slots.begin(), n, b.begin() + 18);
    return b;
}

Bytes make_artsync() {
    Bytes b = artnet_header(14, op_sync);
    b[11] = 14;
    return b;
}

Bytes make_arttimecode(const ArtTimeCode &tc) {
    Bytes b = artnet_header(19, op_timecode);
    b[11] = 14;
    b[14] = tc.frames, b[15] = tc.seconds, b[16] = tc.minutes, b[17] = tc.hours, b[18] = tc.type;
    return b;
}

Bytes make_artpoll() {
    Bytes b = artnet_header(14, op_poll);
    b[11] = 14;
    return b;
}

Bytes make_artpollreply(std::array<std::uint8_t, 4> ip, std::string_view short_name, std::string_view long_name,
                        std::span<const std::uint16_t> port_addresses, std::uint8_t bind_index) {
    Bytes b = artnet_header(239, op_poll_reply);
    std::copy(ip.begin(), ip.end(), b.begin() + 10);
    b[14] = std::uint8_t(artnet_port & 0xFF), b[15] = std::uint8_t(artnet_port >> 8); // little-endian
    const std::size_t n = std::min<std::size_t>(port_addresses.size(), 4);
    if (n > 0) {
        b[18] = std::uint8_t((port_addresses[0] >> 8) & 0x7F); // NetSwitch
        b[19] = std::uint8_t((port_addresses[0] >> 4) & 0x0F); // SubSwitch
    }
    b[23] = 0xD0; // Status1: indicators normal, port-address set by network
    std::copy_n(short_name.begin(), std::min<std::size_t>(short_name.size(), 17), b.begin() + 26);
    std::copy_n(long_name.begin(), std::min<std::size_t>(long_name.size(), 63), b.begin() + 44);
    b[173] = std::uint8_t(n); // NumPortsLo
    for (std::size_t i = 0; i < n; ++i) {
        b[174 + i] = 0x80;                                   // PortTypes: output, DMX512
        b[182 + i] = 0x80;                                   // GoodOutput: data is being received
        b[190 + i] = std::uint8_t(port_addresses[i] & 0x0F); // SwOut
    }
    b[200] = 0x02; // Style: StVisual (visualiser)
    b[211] = bind_index;
    b[212] = 0x08; // Status2: supports 15-bit Port-Address
    return b;
}

Bytes make_sacn_data(const Cid &cid, std::string_view source_name, std::uint8_t priority, std::uint8_t sequence,
                     std::uint16_t universe, std::span<const std::uint8_t> slots, std::uint8_t start_code,
                     std::uint16_t sync_address, bool terminated) {
    const std::size_t n = std::min<std::size_t>(slots.size(), 512);
    Bytes b = acn_root(126 + n, vector_root_data, cid);
    put_flags_length(b, 38);
    put32(b, 40, vector_frame_data);
    std::copy_n(source_name.begin(), std::min<std::size_t>(source_name.size(), 63), b.begin() + 44);
    b[108] = priority;
    put16(b, 109, sync_address);
    b[111] = sequence;
    b[112] = terminated ? 0x40 : 0;
    put16(b, 113, universe);
    put_flags_length(b, 115);
    b[117] = 0x02;
    b[118] = 0xA1;
    put16(b, 121, 1);
    put16(b, 123, std::uint16_t(n + 1));
    b[125] = start_code;
    std::copy_n(slots.begin(), n, b.begin() + 126);
    return b;
}

Bytes make_sacn_sync(const Cid &cid, std::uint8_t sequence, std::uint16_t sync_address) {
    Bytes b = acn_root(49, vector_root_extended, cid);
    put_flags_length(b, 38);
    put32(b, 40, vector_sync);
    b[44] = sequence;
    put16(b, 45, sync_address);
    return b;
}

} // namespace sv::proto
