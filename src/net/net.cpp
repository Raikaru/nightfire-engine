#include "net/net.hpp"

#include <bit>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf::net {
namespace {
constexpr std::uint32_t kMagic = 0x544e464e;  // "NFNT" on the wire
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kMaxNameBytes = 32;
constexpr std::size_t kMaxServerNameBytes = 64;
constexpr std::size_t kMaxMapNameBytes = 64;
constexpr std::size_t kOwnerMovementBytes = 206;

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(std::uint8_t(v)); out.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (i * 8)));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(std::uint8_t(v >> (i * 8)));
}
std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | (std::uint16_t(p[1]) << 8)); }
std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= std::uint64_t(p[i]) << (i * 8);
    return v;
}
void put_float(std::vector<std::uint8_t>& out, float value) {
    put32(out, std::bit_cast<std::uint32_t>(value));
}
float get_float(const std::uint8_t* p) { return std::bit_cast<float>(get32(p)); }

void put_owner_movement(std::vector<std::uint8_t>& out, const OwnerMovementState& s) {
    out.push_back(kOwnerMovementSchemaVersion);
    for (float value : s.fall_velocity) put_float(out, value);
    put16(out, s.body_flags);
    put_float(out, s.ground_normal_y);
    out.push_back(s.jump_state);
    put16(out, s.ground_history);
    put16(out, s.anim_random_timer);
    put_float(out, s.stand_height);
    put_float(out, s.applied_height);
    for (float value : s.settled_pos) put_float(out, value);
    for (float value : s.prev_pos) put_float(out, value);
    for (float value : s.prev_velocity) put_float(out, value);
    put_float(out, s.yaw_step);
    put_float(out, s.fall_timer);
    out.push_back(s.enabled);
    out.push_back(std::uint8_t(s.input_frozen));
    out.push_back(std::uint8_t(s.movement_frozen));
    out.push_back(s.jump_delay);
    out.push_back(s.crouch_timer);
    put_float(out, s.turn_speed);
    put_float(out, s.pitch_speed);
    put_float(out, s.pitch_target);
    put_float(out, s.aim_yaw);
    out.push_back(std::uint8_t(s.scope_aiming));
    put_float(out, s.zoom);
    for (float value : s.aim_state) put_float(out, value);
    put_float(out, s.timing_rate);
    for (float value : s.body_basis) put_float(out, value);
    out.push_back(s.look_state);
    out.push_back(s.walk_class);
    put32(out, std::bit_cast<std::uint32_t>(s.water_room));
    for (float value : s.water_room_from) put_float(out, value);
    put_float(out, s.water_air);
    out.push_back(std::uint8_t(s.water_surfaced));
    put_float(out, s.water_meter_alpha);
    put32(out, s.water_meter_flags);
    out.push_back(std::uint8_t(s.water_meter_enabled));
    put64(out, s.water_frame);
}

bool get_owner_movement(std::span<const std::uint8_t> bytes, OwnerMovementState& s) {
    if (bytes.size() < kOwnerMovementBytes || bytes[0] != kOwnerMovementSchemaVersion) return false;
    std::size_t at = 1;
    auto read_float = [&] {
        const float value = get_float(bytes.data() + at);
        at += 4;
        return value;
    };
    for (float& value : s.fall_velocity) value = read_float();
    s.body_flags = get16(bytes.data() + at); at += 2;
    s.ground_normal_y = read_float();
    s.jump_state = bytes[at++];
    s.ground_history = get16(bytes.data() + at); at += 2;
    s.anim_random_timer = get16(bytes.data() + at); at += 2;
    s.stand_height = read_float();
    s.applied_height = read_float();
    for (float& value : s.settled_pos) value = read_float();
    for (float& value : s.prev_pos) value = read_float();
    for (float& value : s.prev_velocity) value = read_float();
    s.yaw_step = read_float();
    s.fall_timer = read_float();
    s.enabled = bytes[at++];
    const std::uint8_t input_frozen = bytes[at++];
    const std::uint8_t movement_frozen = bytes[at++];
    if (input_frozen > 1 || movement_frozen > 1) return false;
    s.input_frozen = input_frozen != 0;
    s.movement_frozen = movement_frozen != 0;
    s.jump_delay = bytes[at++];
    s.crouch_timer = bytes[at++];
    s.turn_speed = read_float();
    s.pitch_speed = read_float();
    s.pitch_target = read_float();
    s.aim_yaw = read_float();
    const std::uint8_t scope_aiming = bytes[at++];
    if (scope_aiming > 1) return false;
    s.scope_aiming = scope_aiming != 0;
    s.zoom = read_float();
    for (float& value : s.aim_state) value = read_float();
    s.timing_rate = read_float();
    for (float& value : s.body_basis) value = read_float();
    s.look_state = bytes[at++];
    s.walk_class = bytes[at++];
    s.water_room = std::bit_cast<std::int32_t>(get32(bytes.data() + at)); at += 4;
    for (float& value : s.water_room_from) value = read_float();
    s.water_air = read_float();
    const std::uint8_t surfaced = bytes[at++];
    if (surfaced > 1) return false;
    s.water_surfaced = surfaced != 0;
    s.water_meter_alpha = read_float();
    s.water_meter_flags = get32(bytes.data() + at); at += 4;
    const std::uint8_t meter_enabled = bytes[at++];
    if (meter_enabled > 1) return false;
    s.water_meter_enabled = meter_enabled != 0;
    s.water_frame = get64(bytes.data() + at); at += 8;
    return at == kOwnerMovementBytes;
}

// Compact SHA-256 implementation used so handshake hashes do not depend on OpenSSL availability.
constexpr std::array<std::uint32_t, 64> kShaK = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> msg(bytes.begin(), bytes.end());
    const std::uint64_t bit_len = std::uint64_t(msg.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(std::uint8_t(bit_len >> (i * 8)));
    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t block = 0; block < msg.size(); block += 64) {
        std::array<std::uint32_t,64> w{};
        for (unsigned i=0;i<16;++i) w[i]=(std::uint32_t(msg[block+i*4])<<24)|(std::uint32_t(msg[block+i*4+1])<<16)|
                                           (std::uint32_t(msg[block+i*4+2])<<8)|msg[block+i*4+3];
        for (unsigned i=16;i<64;++i) {
            const auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            const auto s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(unsigned i=0;i<64;++i){
            const auto s1=rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^(~e&g);
            const auto t1=z+s1+ch+kShaK[i]+w[i];
            const auto s0=rotr(a,2)^rotr(a,13)^rotr(a,22), maj=(a&b)^(a&c)^(b&c);
            const auto t2=s0+maj; z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    std::array<std::uint8_t,32> out{};
    for(unsigned i=0;i<8;++i) for(unsigned j=0;j<4;++j) out[i*4+j]=std::uint8_t(h[i]>>(24-j*8));
    return out;
}
}  // namespace

std::vector<std::uint8_t> encode(const Packet& packet) {
    if (packet.payload.size() > kMaxDatagramBytes - kHeaderBytes) return {};
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderBytes + packet.payload.size());
    put32(out,kMagic); put16(out,kProtocolVersion); out.push_back(std::uint8_t(packet.header.message)); out.push_back(0);
    put32(out,packet.header.sequence); put32(out,packet.header.acknowledgement); put32(out,packet.header.acknowledgement_bits);
    out.insert(out.end(),packet.payload.begin(),packet.payload.end());
    return out;
}

std::optional<Packet> decode(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<kHeaderBytes || bytes.size()>kMaxDatagramBytes || get32(bytes.data())!=kMagic ||
       get16(bytes.data()+4)!=kProtocolVersion || bytes[7]!=0) return std::nullopt;
    const auto kind=bytes[6];
    if (kind < std::uint8_t(Message::Hello) || kind > std::uint8_t(Message::Projectiles)) return std::nullopt;
    Packet p;
    p.header.message=Message(kind); p.header.sequence=get32(bytes.data()+8);
    p.header.acknowledgement=get32(bytes.data()+12); p.header.acknowledgement_bits=get32(bytes.data()+16);
    p.payload.assign(bytes.begin()+kHeaderBytes,bytes.end());
    return p;
}

std::vector<std::uint8_t> encode_hello(const std::array<std::uint8_t, kDataHashBytes>& hash, std::string_view name,
                                       std::string_view password) {
    if (password.size() > kMaxPasswordBytes) return {};
    const auto n = std::min(name.size(), kMaxNameBytes);
    std::vector<std::uint8_t> out(hash.begin(), hash.end());
    out.push_back(std::uint8_t(n));
    out.push_back(std::uint8_t(password.size()));
    out.insert(out.end(), name.begin(), name.begin() + std::ptrdiff_t(n));
    out.insert(out.end(), password.begin(), password.end());
    return out;
}

bool decode_hello(std::span<const std::uint8_t> payload, std::array<std::uint8_t, kDataHashBytes>& hash,
                  std::string& name, std::string& password) {
    if (payload.size() < kDataHashBytes + 2 || payload[kDataHashBytes] > kMaxNameBytes ||
        payload[kDataHashBytes + 1] > kMaxPasswordBytes ||
        payload.size() != kDataHashBytes + 2 + payload[kDataHashBytes] + payload[kDataHashBytes + 1])
        return false;
    std::copy_n(payload.begin(), kDataHashBytes, hash.begin());
    const std::size_t name_size = payload[kDataHashBytes];
    const std::size_t password_size = payload[kDataHashBytes + 1];
    name.assign(reinterpret_cast<const char*>(payload.data() + kDataHashBytes + 2), name_size);
    password.assign(reinterpret_cast<const char*>(payload.data() + kDataHashBytes + 2 + name_size), password_size);
    return true;
}

bool decode_hello(std::span<const std::uint8_t> payload, std::array<std::uint8_t, kDataHashBytes>& hash,
                  std::string& name) {
    std::string password;
    return decode_hello(payload, hash, name, password);
}

std::vector<std::uint8_t> encode_server_query(std::uint32_t query_id) {
    std::vector<std::uint8_t> out;
    out.reserve(4);
    put32(out, query_id);
    return out;
}

bool decode_server_query(std::span<const std::uint8_t> payload, std::uint32_t& query_id) {
    if (payload.size() != 4) return false;
    query_id = get32(payload.data());
    return true;
}

std::vector<std::uint8_t> encode_server_info(const ServerInfo& info) {
    if (info.name.size() > kMaxServerNameBytes || info.map.size() > kMaxMapNameBytes ||
        info.max_players > 8 || info.players > info.max_players || info.bots > kMaxServerBots)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(22 + info.name.size() + info.map.size());
    put32(out, info.query_id);
    put32(out, info.mode);
    put64(out, info.match_revision);
    out.push_back(info.players);
    out.push_back(info.max_players);
    out.push_back(info.password_required ? 1 : 0);
    out.push_back(std::uint8_t(info.name.size()));
    out.push_back(std::uint8_t(info.map.size()));
    out.insert(out.end(), info.name.begin(), info.name.end());
    out.insert(out.end(), info.map.begin(), info.map.end());
    out.push_back(info.bots);
    return out;
}

bool decode_server_info(std::span<const std::uint8_t> payload, ServerInfo& info) {
    if (payload.size() < 22) return false;
    const std::size_t name_size = payload[19], map_size = payload[20];
    if (payload[18] > 1 || name_size > kMaxServerNameBytes || map_size > kMaxMapNameBytes ||
        payload.size() != 22 + name_size + map_size || payload[16] > payload[17] || payload[17] > 8 ||
        payload.back() > kMaxServerBots)
        return false;
    ServerInfo decoded;
    decoded.query_id = get32(payload.data());
    decoded.mode = get32(payload.data() + 4);
    decoded.match_revision = get64(payload.data() + 8);
    decoded.players = payload[16];
    decoded.max_players = payload[17];
    decoded.password_required = payload[18] != 0;
    decoded.name.assign(reinterpret_cast<const char*>(payload.data() + 21), name_size);
    decoded.map.assign(reinterpret_cast<const char*>(payload.data() + 21 + name_size), map_size);
    decoded.bots = payload.back();
    info = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encode_input(const PadInput& input) {
    std::vector<std::uint8_t> out;
    out.reserve(14);
    put32(out, input.tick);
    put16(out, input.buttons);
    out.insert(out.end(), input.sticks.begin(), input.sticks.end());
    put32(out, input.view_tick);
    return out;
}

bool decode_input(std::span<const std::uint8_t> payload, PadInput& input) {
    if (payload.size() != 14) return false;
    input.tick = get32(payload.data());
    input.buttons = get16(payload.data() + 4);
    std::copy_n(payload.begin() + 6, 4, input.sticks.begin());
    input.view_tick = get32(payload.data() + 10);
    return true;
}

std::vector<std::uint8_t> encode_input_batch(const InputBatch& batch) {
    if (batch.count == 0 || batch.count > batch.samples.size()) return {};
    for (std::size_t i = 1; i < batch.count; ++i)
        if (batch.samples[i - 1].tick <= batch.samples[i].tick ||
            batch.samples[i - 1].view_tick < batch.samples[i].view_tick)
            return {};
    std::vector<std::uint8_t> out;
    out.reserve(1 + std::size_t(batch.count) * 14);
    out.push_back(batch.count);
    for (std::size_t i = 0; i < batch.count; ++i) {
        const auto bytes = encode_input(batch.samples[i]);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

bool decode_input_batch(std::span<const std::uint8_t> payload, InputBatch& batch) {
    if (payload.empty() || payload[0] == 0 || payload[0] > 3 ||
        payload.size() != 1 + std::size_t(payload[0]) * 14) return false;
    InputBatch decoded;
    decoded.count = payload[0];
    for (std::size_t i = 0; i < decoded.count; ++i) {
        if (!decode_input(payload.subspan(1 + i * 14, 14), decoded.samples[i])) return false;
        if (i && (decoded.samples[i - 1].tick <= decoded.samples[i].tick ||
                  decoded.samples[i - 1].view_tick < decoded.samples[i].view_tick))
            return false;
    }
    batch = decoded;
    return true;
}

std::vector<std::uint8_t> encode_snapshot(const Snapshot& snapshot) {
    constexpr std::size_t kHeaderSize = 37, kPlayerFixedSize = 74, kMaxPlayerName = 32;
    if (snapshot.slot_count > 8 || snapshot.players.size() != snapshot.slot_count || snapshot.match_phase > 3)
        return {};
    bool has_owner_movement = false;
    for (const PlayerSnapshot& p : snapshot.players) {
        if (!p.owner_movement) continue;
        if (has_owner_movement || p.slot >= 4 || !p.present || p.bot) return {};
        has_owner_movement = true;
    }
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + snapshot.players.size() * (kPlayerFixedSize + kMaxPlayerName) +
                (has_owner_movement ? kOwnerMovementBytes : 0));
    put32(out, snapshot.tick);
    put32(out, snapshot.ack_input_tick);
    out.push_back(snapshot.slot_count);
    out.push_back(snapshot.match_phase);
    out.push_back(snapshot.state_code);
    put16(out, std::bit_cast<std::uint16_t>(snapshot.score_limit));
    put32(out, std::bit_cast<std::uint32_t>(snapshot.elapsed));
    put32(out, std::bit_cast<std::uint32_t>(snapshot.time_left));
    put32(out, std::bit_cast<std::uint32_t>(snapshot.team_score[0]));
    put32(out, std::bit_cast<std::uint32_t>(snapshot.team_score[1]));
    put64(out, snapshot.match_revision);
    std::uint8_t seen = 0;
    for (const PlayerSnapshot& p : snapshot.players) {
        if (p.slot >= snapshot.slot_count || (seen & (1u << p.slot)) || p.name.size() > kMaxPlayerName)
            return {};
        seen |= std::uint8_t(1u << p.slot);
        out.push_back(p.slot);
        out.push_back(std::uint8_t((p.present ? 1 : 0) | (p.alive ? 2 : 0) | (p.bot ? 4 : 0) |
                                   (p.visible ? 8 : 0) | (p.out ? 16 : 0) | (p.aiming ? 32 : 0) |
                                   (p.owner_movement ? 64 : 0)));
        put32(out, std::bit_cast<std::uint32_t>(p.x));
        put32(out, std::bit_cast<std::uint32_t>(p.y));
        put32(out, std::bit_cast<std::uint32_t>(p.z));
        put32(out, std::bit_cast<std::uint32_t>(p.yaw));
        put32(out, std::bit_cast<std::uint32_t>(p.health));
        put32(out, std::bit_cast<std::uint32_t>(p.armor));
        put16(out, std::bit_cast<std::uint16_t>(p.kills));
        put16(out, std::bit_cast<std::uint16_t>(p.deaths));
        put16(out, std::bit_cast<std::uint16_t>(p.score));
        put32(out, std::bit_cast<std::uint32_t>(p.points));
        put32(out, std::bit_cast<std::uint32_t>(p.radar_x));
        put32(out, std::bit_cast<std::uint32_t>(p.radar_y));
        put32(out, std::bit_cast<std::uint32_t>(p.radar_z));
        put32(out, std::bit_cast<std::uint32_t>(p.pitch));
        for (float velocity : p.velocity) put32(out, std::bit_cast<std::uint32_t>(velocity));
        out.push_back(std::uint8_t(p.team));
        out.push_back(p.substate);
        out.push_back(p.weapon);
        out.push_back(p.weapon_anim);
        put16(out, std::bit_cast<std::uint16_t>(p.weapon_clip));
        put16(out, p.weapon_ammo);
        out.push_back(p.character);
        out.push_back(std::uint8_t(p.name.size()));
        out.insert(out.end(), p.name.begin(), p.name.end());
        if (p.owner_movement) put_owner_movement(out, *p.owner_movement);
        if (out.size() > kMaxDatagramBytes - kHeaderBytes) return {};
    }
    if (snapshot.slot_count && seen != std::uint8_t((1u << snapshot.slot_count) - 1u)) return {};
    return out;
}

bool decode_snapshot(std::span<const std::uint8_t> payload, Snapshot& snapshot) {
    constexpr std::size_t kHeaderSize = 37, kPlayerFixedSize = 74, kMaxPlayerName = 32;
    if (payload.size() < kHeaderSize || payload.size() > kMaxDatagramBytes - kHeaderBytes ||
        payload[8] > 8 || payload[9] > 3)
        return false;
    Snapshot decoded;
    decoded.tick = get32(payload.data());
    decoded.ack_input_tick = get32(payload.data() + 4);
    decoded.slot_count = payload[8];
    decoded.match_phase = payload[9];
    decoded.state_code = payload[10];
    decoded.score_limit = std::bit_cast<std::int16_t>(get16(payload.data() + 11));
    decoded.elapsed = std::bit_cast<float>(get32(payload.data() + 13));
    decoded.time_left = std::bit_cast<float>(get32(payload.data() + 17));
    decoded.team_score[0] = std::bit_cast<float>(get32(payload.data() + 21));
    decoded.team_score[1] = std::bit_cast<float>(get32(payload.data() + 25));
    decoded.match_revision = get64(payload.data() + 29);
    decoded.players.reserve(decoded.slot_count);
    std::uint8_t seen = 0;
    bool has_owner_movement = false;
    std::size_t at = kHeaderSize;
    for (std::size_t i = 0; i < decoded.slot_count; ++i) {
        if (payload.size() - at < kPlayerFixedSize) return false;
        PlayerSnapshot p;
        p.slot = payload[at];
        const std::uint8_t flags = payload[at + 1];
        const std::size_t name_size = payload[at + 73];
        const bool has_movement = (flags & 64) != 0;
        if (p.slot >= decoded.slot_count || (seen & (1u << p.slot)) || (flags & 0x80) ||
            name_size > kMaxPlayerName || payload.size() - at < kPlayerFixedSize + name_size)
            return false;
        seen |= std::uint8_t(1u << p.slot);
        p.present = (flags & 1) != 0;
        p.alive = (flags & 2) != 0;
        p.bot = (flags & 4) != 0;
        p.visible = (flags & 8) != 0;
        p.out = (flags & 16) != 0;
        p.aiming = (flags & 32) != 0;
        p.x = std::bit_cast<float>(get32(payload.data() + at + 2));
        p.y = std::bit_cast<float>(get32(payload.data() + at + 6));
        p.z = std::bit_cast<float>(get32(payload.data() + at + 10));
        p.yaw = std::bit_cast<float>(get32(payload.data() + at + 14));
        p.health = std::bit_cast<float>(get32(payload.data() + at + 18));
        p.armor = std::bit_cast<float>(get32(payload.data() + at + 22));
        p.kills = std::bit_cast<std::int16_t>(get16(payload.data() + at + 26));
        p.deaths = std::bit_cast<std::int16_t>(get16(payload.data() + at + 28));
        p.score = std::bit_cast<std::int16_t>(get16(payload.data() + at + 30));
        p.points = std::bit_cast<float>(get32(payload.data() + at + 32));
        p.radar_x = std::bit_cast<float>(get32(payload.data() + at + 36));
        p.radar_y = std::bit_cast<float>(get32(payload.data() + at + 40));
        p.radar_z = std::bit_cast<float>(get32(payload.data() + at + 44));
        p.pitch = std::bit_cast<float>(get32(payload.data() + at + 48));
        for (std::size_t axis = 0; axis < 3; ++axis)
            p.velocity[axis] = std::bit_cast<float>(get32(payload.data() + at + 52 + axis * 4));
        p.team = std::bit_cast<std::int8_t>(payload[at + 64]);
        p.substate = payload[at + 65];
        p.weapon = payload[at + 66];
        p.weapon_anim = payload[at + 67];
        p.weapon_clip = std::bit_cast<std::int16_t>(get16(payload.data() + at + 68));
        p.weapon_ammo = get16(payload.data() + at + 70);
        p.character = payload[at + 72];
        p.name.assign(reinterpret_cast<const char*>(payload.data() + at + kPlayerFixedSize), name_size);
        at += kPlayerFixedSize + name_size;
        if (has_movement) {
            if (has_owner_movement || p.slot >= 4 || !p.present || p.bot ||
                payload.size() - at < kOwnerMovementBytes)
                return false;
            OwnerMovementState movement;
            if (!get_owner_movement(payload.subspan(at, kOwnerMovementBytes), movement)) return false;
            p.owner_movement = movement;
            has_owner_movement = true;
            at += kOwnerMovementBytes;
        }
        decoded.players.push_back(std::move(p));
    }
    if (at != payload.size() ||
        (decoded.slot_count && seen != std::uint8_t((1u << decoded.slot_count) - 1u)))
        return false;
    snapshot = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encode_world_state(const WorldState& state) {
    constexpr std::size_t kPickupBytes = 7, kObjectiveBytes = 22;
    if (state.pickups.size() > 128 || state.objectives.size() > 32 ||
        7 + state.pickups.size() * kPickupBytes + state.objectives.size() * kObjectiveBytes >
            kMaxDatagramBytes - kHeaderBytes)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(7 + state.pickups.size() * kPickupBytes + state.objectives.size() * kObjectiveBytes);
    put32(out, state.tick);
    put16(out, std::uint16_t(state.pickups.size()));
    out.push_back(std::uint8_t(state.objectives.size()));
    for (std::size_t i = 0; i < state.pickups.size(); ++i) {
        const PickupSnapshot& p = state.pickups[i];
        if (p.state > 3) return {};
        for (std::size_t j = 0; j < i; ++j)
            if (state.pickups[j].id == p.id) return {};
        put16(out, p.id);
        out.push_back(p.state);
        put32(out, std::bit_cast<std::uint32_t>(p.spin));
    }
    for (std::size_t i = 0; i < state.objectives.size(); ++i) {
        const ObjectiveSnapshot& o = state.objectives[i];
        if (o.kind > 9) return {};
        for (std::size_t j = 0; j < i; ++j)
            if (state.objectives[j].id == o.id) return {};
        out.push_back(o.id);
        out.push_back(o.kind);
        out.push_back(std::uint8_t(o.team));
        out.push_back(std::uint8_t(o.carrier));
        out.push_back(o.state);
        out.push_back(o.visible ? 1 : 0);
        put32(out, std::bit_cast<std::uint32_t>(o.x));
        put32(out, std::bit_cast<std::uint32_t>(o.y));
        put32(out, std::bit_cast<std::uint32_t>(o.z));
        put32(out, std::bit_cast<std::uint32_t>(o.hit_points));
    }
    return out;
}

bool decode_world_state(std::span<const std::uint8_t> payload, WorldState& state) {
    constexpr std::size_t kHeaderBytes = 7, kPickupBytes = 7, kObjectiveBytes = 22;
    if (payload.size() < kHeaderBytes) return false;
    const std::size_t pickup_count = get16(payload.data() + 4), objective_count = payload[6];
    if (pickup_count > 128 || objective_count > 32 ||
        payload.size() != kHeaderBytes + pickup_count * kPickupBytes + objective_count * kObjectiveBytes)
        return false;
    WorldState decoded;
    decoded.tick = get32(payload.data());
    decoded.pickups.reserve(pickup_count);
    decoded.objectives.reserve(objective_count);
    std::size_t at = kHeaderBytes;
    for (std::size_t i = 0; i < pickup_count; ++i, at += kPickupBytes) {
        PickupSnapshot p{get16(payload.data() + at), payload[at + 2],
                         std::bit_cast<float>(get32(payload.data() + at + 3))};
        if (p.state > 3) return false;
        for (const PickupSnapshot& previous : decoded.pickups)
            if (previous.id == p.id) return false;
        decoded.pickups.push_back(p);
    }
    for (std::size_t i = 0; i < objective_count; ++i, at += kObjectiveBytes) {
        ObjectiveSnapshot o;
        o.id = payload[at];
        o.kind = payload[at + 1];
        o.team = std::bit_cast<std::int8_t>(payload[at + 2]);
        o.carrier = std::bit_cast<std::int8_t>(payload[at + 3]);
        o.state = payload[at + 4];
        if (o.kind > 9 || payload[at + 5] > 1) return false;
        o.visible = payload[at + 5] != 0;
        o.x = std::bit_cast<float>(get32(payload.data() + at + 6));
        o.y = std::bit_cast<float>(get32(payload.data() + at + 10));
        o.z = std::bit_cast<float>(get32(payload.data() + at + 14));
        o.hit_points = std::bit_cast<float>(get32(payload.data() + at + 18));
        for (const ObjectiveSnapshot& previous : decoded.objectives)
            if (previous.id == o.id) return false;
        decoded.objectives.push_back(o);
    }
    state = std::move(decoded);
    return true;
}

std::vector<ProjectilePage> split_projectiles(std::uint32_t tick, std::span<const ProjectileSnapshot> projectiles) {
    constexpr std::size_t kProjectilesPerPage = 32;
    if (projectiles.size() > 255 * kProjectilesPerPage) return {};
    const std::size_t page_count = std::max<std::size_t>(1, (projectiles.size() + kProjectilesPerPage - 1) /
                                                            kProjectilesPerPage);
    std::vector<ProjectilePage> pages;
    pages.reserve(page_count);
    for (std::size_t i = 0; i < page_count; ++i) {
        const std::size_t first = i * kProjectilesPerPage;
        const std::size_t count = std::min(kProjectilesPerPage, projectiles.size() - first);
        ProjectilePage page;
        page.tick = tick;
        page.total = std::uint16_t(projectiles.size());
        page.index = std::uint8_t(i);
        page.count = std::uint8_t(page_count);
        page.projectiles.assign(projectiles.begin() + std::ptrdiff_t(first),
                                projectiles.begin() + std::ptrdiff_t(first + count));
        pages.push_back(std::move(page));
    }
    return pages;
}

std::vector<std::uint8_t> encode_projectile_page(const ProjectilePage& page) {
    constexpr std::size_t kHeaderSize = 9, kProjectileBytes = 36, kProjectilesPerPage = 32;
    if (page.count == 0 || page.index >= page.count || page.total > 255 * kProjectilesPerPage ||
        page.projectiles.size() > kProjectilesPerPage || page.projectiles.size() > page.total ||
        kHeaderSize + page.projectiles.size() * kProjectileBytes > kMaxDatagramBytes - kHeaderBytes)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + page.projectiles.size() * kProjectileBytes);
    put32(out, page.tick);
    put16(out, page.total);
    out.push_back(page.index);
    out.push_back(page.count);
    out.push_back(std::uint8_t(page.projectiles.size()));
    for (const ProjectileSnapshot& p : page.projectiles) {
        out.push_back(std::uint8_t(p.id));
        out.push_back(std::uint8_t(p.id >> 8));
        put16(out, p.weapon);
        put16(out, std::bit_cast<std::uint16_t>(p.owner));
        out.push_back(p.state);
        out.push_back(std::uint8_t((p.resting ? 1 : 0) | (p.delete_me ? 2 : 0)));
        for (float v : p.position) put32(out, std::bit_cast<std::uint32_t>(v));
        for (float v : p.direction) put32(out, std::bit_cast<std::uint32_t>(v));
        put32(out, std::bit_cast<std::uint32_t>(p.age));
    }
    return out;
}

bool decode_projectile_page(std::span<const std::uint8_t> payload, ProjectilePage& page) {
    constexpr std::size_t kHeaderSize = 9, kProjectileBytes = 36, kProjectilesPerPage = 32;
    if (payload.size() < kHeaderSize) return false;
    ProjectilePage decoded;
    decoded.tick = get32(payload.data());
    decoded.total = get16(payload.data() + 4);
    decoded.index = payload[6];
    decoded.count = payload[7];
    const std::size_t projectile_count = payload[8];
    if (decoded.count == 0 || decoded.index >= decoded.count || decoded.total > 255 * kProjectilesPerPage ||
        projectile_count > kProjectilesPerPage || projectile_count > decoded.total ||
        payload.size() != kHeaderSize + projectile_count * kProjectileBytes)
        return false;
    decoded.projectiles.reserve(projectile_count);
    for (std::size_t i = 0, at = kHeaderSize; i < projectile_count; ++i, at += kProjectileBytes) {
        ProjectileSnapshot p;
        p.id = get16(payload.data() + at);
        p.weapon = get16(payload.data() + at + 2);
        p.owner = std::bit_cast<std::int16_t>(get16(payload.data() + at + 4));
        p.state = payload[at + 6];
        const std::uint8_t flags = payload[at + 7];
        if (p.state > 4 || (flags & 0xfc)) return false;
        p.resting = (flags & 1) != 0;
        p.delete_me = (flags & 2) != 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
            p.position[axis] = std::bit_cast<float>(get32(payload.data() + at + 8 + axis * 4));
        for (std::size_t axis = 0; axis < 3; ++axis)
            p.direction[axis] = std::bit_cast<float>(get32(payload.data() + at + 20 + axis * 4));
        p.age = std::bit_cast<float>(get32(payload.data() + at + 32));
        decoded.projectiles.push_back(p);
    }
    page = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encode_event(const ReplicationEvent& event) {
    constexpr std::size_t kMaxEventText = 200, kFixedSize = 69;
    if (event.kind < EventKind::Sound || event.kind > EventKind::Kill || event.text.size() > kMaxEventText ||
        kFixedSize + event.text.size() > kMaxDatagramBytes - kHeaderBytes)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(kFixedSize + event.text.size());
    put32(out, event.id);
    put32(out, event.tick);
    out.push_back(std::uint8_t(event.kind));
    put16(out, std::bit_cast<std::uint16_t>(event.actor));
    put16(out, std::bit_cast<std::uint16_t>(event.target));
    put16(out, std::bit_cast<std::uint16_t>(event.code));
    put16(out, std::bit_cast<std::uint16_t>(event.weapon));
    put16(out, std::bit_cast<std::uint16_t>(event.slot));
    out.push_back(std::uint8_t((event.positional ? 1 : 0) | (event.on_body ? 2 : 0)));
    for (float v : event.position) put32(out, std::bit_cast<std::uint32_t>(v));
    for (float v : event.normal) put32(out, std::bit_cast<std::uint32_t>(v));
    put32(out, std::bit_cast<std::uint32_t>(event.radius));
    put32(out, std::bit_cast<std::uint32_t>(event.yaw));
    put16(out, event.frames);
    put32(out, event.script);
    put32(out, event.label);
    put32(out, event.arg_label);
    put16(out, std::bit_cast<std::uint16_t>(event.count));
    out.push_back(std::uint8_t(event.text.size()));
    out.insert(out.end(), event.text.begin(), event.text.end());
    return out;
}

bool decode_event(std::span<const std::uint8_t> payload, ReplicationEvent& event) {
    constexpr std::size_t kMaxEventText = 200, kFixedSize = 69;
    if (payload.size() < kFixedSize || payload[8] < std::uint8_t(EventKind::Sound) ||
        payload[8] > std::uint8_t(EventKind::Kill) || (payload[19] & 0xfc) || payload[68] > kMaxEventText ||
        payload.size() != kFixedSize + payload[68])
        return false;
    ReplicationEvent decoded;
    decoded.id = get32(payload.data());
    decoded.tick = get32(payload.data() + 4);
    decoded.kind = EventKind(payload[8]);
    decoded.actor = std::bit_cast<std::int16_t>(get16(payload.data() + 9));
    decoded.target = std::bit_cast<std::int16_t>(get16(payload.data() + 11));
    decoded.code = std::bit_cast<std::int16_t>(get16(payload.data() + 13));
    decoded.weapon = std::bit_cast<std::int16_t>(get16(payload.data() + 15));
    decoded.slot = std::bit_cast<std::int16_t>(get16(payload.data() + 17));
    decoded.positional = (payload[19] & 1) != 0;
    decoded.on_body = (payload[19] & 2) != 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
        decoded.position[axis] = std::bit_cast<float>(get32(payload.data() + 20 + axis * 4));
    for (std::size_t axis = 0; axis < 3; ++axis)
        decoded.normal[axis] = std::bit_cast<float>(get32(payload.data() + 32 + axis * 4));
    decoded.radius = std::bit_cast<float>(get32(payload.data() + 44));
    decoded.yaw = std::bit_cast<float>(get32(payload.data() + 48));
    decoded.frames = get16(payload.data() + 52);
    decoded.script = get32(payload.data() + 54);
    decoded.label = get32(payload.data() + 58);
    decoded.arg_label = get32(payload.data() + 62);
    decoded.count = std::bit_cast<std::int16_t>(get16(payload.data() + 66));
    decoded.text.assign(reinterpret_cast<const char*>(payload.data() + kFixedSize), payload[68]);
    event = std::move(decoded);
    return true;
}

std::array<std::uint8_t,kDataHashBytes> game_data_hash(std::span<const std::uint8_t> elf,std::span<const std::uint8_t> level) {
    constexpr std::string_view domain="Nightfire multiplayer data v1";
    std::vector<std::uint8_t> bytes(domain.begin(),domain.end());
    for(unsigned i=0;i<8;++i) bytes.push_back(std::uint8_t(std::uint64_t(elf.size())>>(i*8)));
    bytes.insert(bytes.end(),elf.begin(),elf.end());
    for(unsigned i=0;i<8;++i) bytes.push_back(std::uint8_t(std::uint64_t(level.size())>>(i*8)));
    bytes.insert(bytes.end(),level.begin(),level.end());
    return sha256(bytes);
}
std::string hash_hex(const std::array<std::uint8_t,kDataHashBytes>& hash) {
    constexpr char digits[]="0123456789abcdef"; std::string out; out.reserve(64);
    for(auto b:hash){out.push_back(digits[b>>4]);out.push_back(digits[b&15]);} return out;
}

struct UdpSocket::Impl {
    int fd=-1;
    struct Delayed { std::chrono::steady_clock::time_point due; sockaddr_in peer; std::vector<std::uint8_t> data; };
    std::deque<Delayed> delayed;
    std::uint32_t rng=0x4e46504e;
};
UdpSocket::UdpSocket() : impl_(new Impl) {}
UdpSocket::~UdpSocket(){if(impl_){if(impl_->fd>=0) close(impl_->fd);delete impl_;}}
bool UdpSocket::valid() const{return impl_&&impl_->fd>=0;}
bool UdpSocket::bind(std::uint16_t port,std::string* error) {
    if(!impl_) return false;
    if(impl_->fd>=0) close(impl_->fd);
    impl_->fd=socket(AF_INET,SOCK_DGRAM,0);
    if(impl_->fd<0){if(error)*error="socket() failed";return false;}
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_ANY);addr.sin_port=htons(port);
    if(::bind(impl_->fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){if(error)*error="bind() failed";close(impl_->fd);impl_->fd=-1;return false;}
    const int flags=fcntl(impl_->fd,F_GETFL,0);fcntl(impl_->fd,F_SETFL,flags|O_NONBLOCK);
    impl_->rng=sim_.random_seed; return true;
}
bool UdpSocket::enable_broadcast(std::string* error) {
    if (!valid()) {
        if (error) *error = "socket is not bound";
        return false;
    }
    const int enabled = 1;
    if (setsockopt(impl_->fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) {
        if (error) *error = "setsockopt(SO_BROADCAST) failed";
        return false;
    }
    return true;
}

bool UdpSocket::send(std::string_view host,std::uint16_t port,const Packet& packet) {
    if(!valid())return false;
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);
    const std::string h(host);
    if(inet_pton(AF_INET,h.c_str(),&addr.sin_addr)!=1)return false;
    auto data=encode(packet);if(data.empty())return false;
    impl_->rng^=impl_->rng<<13;impl_->rng^=impl_->rng>>17;impl_->rng^=impl_->rng<<5;
    if(sim_.loss_percent && impl_->rng%100<sim_.loss_percent)return true;
    if(sim_.latency_ms){impl_->delayed.push_back({std::chrono::steady_clock::now()+std::chrono::milliseconds(sim_.latency_ms),addr,std::move(data)});return true;}
    return sendto(impl_->fd,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==ssize_t(data.size());
}
std::vector<Received> UdpSocket::receive() {
    std::vector<Received> out;if(!valid())return out;
    const auto now=std::chrono::steady_clock::now();
    while(!impl_->delayed.empty()&&impl_->delayed.front().due<=now){auto& d=impl_->delayed.front();sendto(impl_->fd,d.data.data(),d.data.size(),0,reinterpret_cast<sockaddr*>(&d.peer),sizeof(d.peer));impl_->delayed.pop_front();}
    std::array<std::uint8_t,kMaxDatagramBytes+1> buffer{};
    for(;;){sockaddr_in peer{};socklen_t len=sizeof(peer);const auto n=recvfrom(impl_->fd,buffer.data(),buffer.size(),0,reinterpret_cast<sockaddr*>(&peer),&len);if(n<=0)break;
        const auto decoded=decode(std::span<const std::uint8_t>(buffer.data(),std::size_t(n)));if(!decoded)continue;
        char host[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&peer.sin_addr,host,sizeof(host));
        out.push_back({*decoded,host,ntohs(peer.sin_port)});
    }
    return out;
}

Packet Reliability::prepare(Packet packet, bool reliable, std::chrono::steady_clock::time_point now) {
    packet.header.sequence = next_sequence_++;
    packet.header.acknowledgement = received_latest_;
    packet.header.acknowledgement_bits = received_bits_;
    if (reliable) pending_.push_back({packet, now});
    return packet;
}

bool Reliability::observe(const Header& received) {
    const auto acknowledged = [&](std::uint32_t sequence) {
        if (sequence == received.acknowledgement) return true;
        const std::uint32_t age = received.acknowledgement - sequence;
        return age >= 1 && age <= 32 && ((received.acknowledgement_bits >> (age - 1)) & 1u) != 0;
    };
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) {
        return acknowledged(p.packet.header.sequence);
    }), pending_.end());

    const std::uint32_t sequence = received.sequence;
    if (!received_any_) {
        received_latest_ = sequence;
        received_bits_ = 0;
        received_any_ = true;
        return true;
    }
    const std::int32_t delta = static_cast<std::int32_t>(sequence - received_latest_);
    if (delta > 0) {
        if (delta >= 32) received_bits_ = delta == 32 ? 0x80000000u : 0;
        else received_bits_ = (received_bits_ << delta) | (1u << (delta - 1));
        received_latest_ = sequence;
        return true;
    }
    const std::uint32_t age = received_latest_ - sequence;
    if (age == 0 || age > 32) return false;
    const std::uint32_t bit = 1u << (age - 1);
    const bool fresh = (received_bits_ & bit) == 0;
    received_bits_ |= bit;
    return fresh;
}

std::vector<Packet> Reliability::retransmit_due(std::chrono::steady_clock::time_point now) {
    std::vector<Packet> due;
    for (Pending& pending : pending_) {
        if (now - pending.sent < std::chrono::milliseconds(100)) continue;
        pending.sent = now;
        pending.packet.header.acknowledgement = received_latest_;
        pending.packet.header.acknowledgement_bits = received_bits_;
        due.push_back(pending.packet);
    }
    return due;
}

}  // namespace nf::net
