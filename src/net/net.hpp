#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nf::net {

constexpr std::uint16_t kProtocolVersion = 7;
constexpr std::uint8_t kOwnerMovementSchemaVersion = 2;
constexpr std::uint8_t kMaxServerBots = 16;
constexpr std::size_t kDataHashBytes = 32;
constexpr std::size_t kMaxDatagramBytes = 1200;
constexpr std::size_t kMaxPasswordBytes = 64;
constexpr std::uint8_t kMaxLocalPlayers = 4;
constexpr std::size_t kInputRedundancy = 3;
enum class Message : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    Input = 4,
    Snapshot = 5,
    Chat = 6,
    Event = 7,
    Ack = 8,
    ServerQuery = 9,
    ServerInfo = 10,
    WorldState = 11,
    Projectiles = 12,
};

struct PadInput {
    std::uint32_t tick = 0;
    std::uint16_t buttons = 0;
    std::array<std::uint8_t, 4> sticks{0x80, 0x80, 0x80, 0x80};
    std::uint32_t view_tick = 0;  // newest server snapshot represented by the client's rendered/interpolated pose
    std::uint8_t local_player = 0;  // per-connection local player index

};
struct Header {
    Message message = Message::Ack;
    std::uint32_t sequence = 0;
    std::uint32_t acknowledgement = 0;
    std::uint32_t acknowledgement_bits = 0;
};

struct Packet {
    Header header;
    std::vector<std::uint8_t> payload;
};

// All integer fields are explicitly little-endian; decode rejects malformed, oversized,
// wrong-version, or unknown-message packets before exposing payload bytes.
std::vector<std::uint8_t> encode(const Packet& packet);
std::optional<Packet> decode(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode_hello(const std::array<std::uint8_t, kDataHashBytes>& data_hash,
                                      std::string_view player_name, std::string_view password = {},
                                      std::uint8_t local_players = 1);
bool decode_hello(std::span<const std::uint8_t> payload,
                  std::array<std::uint8_t, kDataHashBytes>& data_hash, std::string& player_name,
                  std::string& password, std::uint8_t& local_players);
bool decode_hello(std::span<const std::uint8_t> payload,
                  std::array<std::uint8_t, kDataHashBytes>& data_hash, std::string& player_name,
                  std::string& password);
bool decode_hello(std::span<const std::uint8_t> payload,
                  std::array<std::uint8_t, kDataHashBytes>& data_hash, std::string& player_name);
struct ServerInfo {
    std::uint32_t query_id = 0;
    std::string name;
    std::string map;
    std::uint32_t mode = 0;
    std::uint8_t players = 0;
    std::uint8_t max_players = 0;
    bool password_required = false;
    std::uint64_t match_revision = 0;
    std::uint8_t bots = 0;
    std::uint8_t slot_count = 8;
    bool modified_rules = false;
};
std::vector<std::uint8_t> encode_server_query(std::uint32_t query_id);
bool decode_server_query(std::span<const std::uint8_t> payload, std::uint32_t& query_id);
std::vector<std::uint8_t> encode_server_info(const ServerInfo& info);
bool decode_server_info(std::span<const std::uint8_t> payload, ServerInfo& info);

std::array<std::uint8_t, kDataHashBytes> game_data_hash(std::span<const std::uint8_t> action_elf,
                                                        std::span<const std::uint8_t> level);
std::vector<std::uint8_t> encode_input(const PadInput& input);
bool decode_input(std::span<const std::uint8_t> payload, PadInput& input);
struct InputBatch {
    std::uint8_t count = 0;
    std::array<PadInput, kMaxLocalPlayers * kInputRedundancy> samples{};
};
std::vector<std::uint8_t> encode_input_batch(const InputBatch& batch);
bool decode_input_batch(std::span<const std::uint8_t> payload, InputBatch& batch);

struct OwnerMovementState {
    std::array<float, 3> fall_velocity{};
    std::uint16_t body_flags = 0;
    float ground_normal_y = 1.0f;
    std::uint8_t jump_state = 0;
    std::uint16_t ground_history = 0, anim_random_timer = 0;
    float stand_height = 0.0f, applied_height = 0.0f;
    std::array<float, 3> settled_pos{}, prev_pos{}, prev_velocity{};
    float yaw_step = 0.0f, fall_timer = 0.0f;
    std::uint8_t enabled = 0;
    bool input_frozen = false, movement_frozen = false;
    std::uint8_t jump_delay = 0, crouch_timer = 0;
    float turn_speed = 0.0f, pitch_speed = 0.0f, pitch_target = 0.0f, aim_yaw = 0.0f;
    bool scope_aiming = false;
    float zoom = 1.0f;
    std::array<float, 6> aim_state{};  // cursor x/y, turn x/y, scope x/y
    float timing_rate = 30.0f;
    std::array<float, 9> body_basis{};
    std::uint8_t look_state = 0, walk_class = 1;
    std::int32_t water_room = -1;
    std::array<float, 3> water_room_from{};
    float water_air = 100.0f;
    bool water_surfaced = true;
    float water_meter_alpha = 0.0f;
    std::uint32_t water_meter_flags = 0;
    bool water_meter_enabled = false;
    std::uint64_t water_frame = 0;
};

struct PlayerSnapshot {
    std::uint8_t slot = 0;
    bool present = false;
    bool alive = false;
    bool bot = false;
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    float health = 0, armor = 0;
    std::array<float, 3> velocity{};
    std::int16_t kills = 0, deaths = 0, score = 0;
    float points = 0;
    float radar_x = 0, radar_y = 0, radar_z = 0;
    std::int8_t team = -1;
    std::uint8_t substate = 0;
    std::uint8_t weapon = 0;
    std::uint8_t weapon_anim = 0;
    std::int16_t weapon_clip = 0;
    std::uint16_t weapon_ammo = 0;
    std::uint8_t character = 0;
    bool visible = true;
    bool out = false;
    bool aiming = false;
    std::string name;
    std::optional<OwnerMovementState> owner_movement;
};

constexpr std::size_t kSnapshotPlayersPerPage = 2;
struct Snapshot {
    std::uint32_t tick = 0;
    std::uint32_t ack_input_tick = 0;
    std::uint8_t slot_count = 0;
    std::uint8_t match_phase = 0;
    std::uint8_t state_code = 0;
    std::int16_t score_limit = -1;
    float elapsed = 0, time_left = -1;
    std::array<float, 2> team_score{};
    std::uint64_t match_revision = 0;
    std::uint8_t page_index = 0, page_count = 1;
    std::vector<PlayerSnapshot> players;
};
std::vector<Snapshot> split_snapshot(const Snapshot& snapshot);
std::vector<std::uint8_t> encode_snapshot(const Snapshot& snapshot);
bool decode_snapshot(std::span<const std::uint8_t> payload, Snapshot& snapshot);


struct PickupSnapshot {
    std::uint16_t id = 0;
    std::uint8_t state = 0;
    float spin = 0;
};
struct ObjectiveSnapshot {
    std::uint8_t id = 0, kind = 0;
    std::int8_t team = -1, carrier = -1;
    std::uint8_t state = 0;
    bool visible = true;
    float x = 0, y = 0, z = 0, hit_points = 0;
};
struct WorldState {
    std::uint32_t tick = 0;
    std::vector<PickupSnapshot> pickups;
    std::vector<ObjectiveSnapshot> objectives;
};
std::vector<std::uint8_t> encode_world_state(const WorldState& state);
bool decode_world_state(std::span<const std::uint8_t> payload, WorldState& state);

struct ProjectileSnapshot {
    std::uint16_t id = 0, weapon = 0;
    std::int16_t owner = -1;
    std::uint8_t state = 0;
    bool resting = false, delete_me = false;
    std::array<float, 3> position{}, direction{};
    float age = 0;
};
struct ProjectilePage {
    std::uint32_t tick = 0;
    std::uint16_t total = 0;
    std::uint8_t index = 0, count = 1;
    std::vector<ProjectileSnapshot> projectiles;
};
std::vector<ProjectilePage> split_projectiles(std::uint32_t tick, std::span<const ProjectileSnapshot> projectiles);
std::vector<std::uint8_t> encode_projectile_page(const ProjectilePage& page);
bool decode_projectile_page(std::span<const std::uint8_t> payload, ProjectilePage& page);

enum class EventKind : std::uint8_t { Sound = 1, Impact = 2, Explosion = 3, Message = 4, Pickup = 5, Kill = 6 };
struct ReplicationEvent {
    std::uint32_t id = 0, tick = 0;
    EventKind kind = EventKind::Sound;
    std::int16_t actor = -1, target = -1, code = 0, weapon = 0, slot = -1;
    bool positional = false, on_body = false;
    std::array<float, 3> position{}, normal{};
    float radius = 0, yaw = 0;
    std::uint16_t frames = 0;
    std::uint32_t script = 0, label = 0, arg_label = 0;
    std::int16_t count = 0;
    std::string text;
};
std::vector<std::uint8_t> encode_event(const ReplicationEvent& event);
bool decode_event(std::span<const std::uint8_t> payload, ReplicationEvent& event);

// Outbound fault injection for a socket. Loss is probabilistic; latency queues datagrams
// around the configured one-way delay, with optional bounded symmetric jitter.
inline constexpr unsigned kDefaultNetSimJitterMs = 5;
struct NetSim {
    unsigned loss_percent = 0;
    unsigned latency_ms = 0;
    std::uint32_t random_seed = 0x4e46504e;
    unsigned jitter_ms = 0;
};

struct Received {
    Packet packet;
    std::string address;
    std::uint16_t port = 0;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool bind(std::uint16_t port, std::string* error = nullptr);
    bool enable_broadcast(std::string* error = nullptr);
    bool send(std::string_view address, std::uint16_t port, const Packet& packet);
    void simulate(NetSim sim) { sim_ = sim; }
    std::vector<Received> receive();
    bool valid() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    NetSim sim_;
};

// Selective-ack reliability for small event/chat messages. `observe()` returns true only for a
// newly received sequence; call `prepare()` before sending and `retransmit_due()` on network updates.
class Reliability {
public:
    Packet prepare(Packet packet, bool reliable, std::chrono::steady_clock::time_point now);
    bool observe(const Header& received);
    std::vector<Packet> retransmit_due(std::chrono::steady_clock::time_point now);

private:
    struct Pending {
        Packet packet;
        std::chrono::steady_clock::time_point sent;
    };
    std::uint32_t next_sequence_ = 1;
    std::uint32_t received_latest_ = 0;
    std::uint32_t received_bits_ = 0;
    bool received_any_ = false;
    std::vector<Pending> pending_;
};

// SHA-256 over a concatenation of two already-loaded files, with domain separation.
std::string hash_hex(const std::array<std::uint8_t, kDataHashBytes>& hash);

}  // namespace nf::net
