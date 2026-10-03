#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <iterator>
#include <string_view>

#include "app/net_prediction.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "core/rng.hpp"
#include "game/actions.hpp"
#include "game/world.hpp"

namespace {

struct Frame {
    nf::PadState pad;
    float rate = nf::World::kTickHz;
};

struct FieldDiff {
    const char* name;
    std::uint64_t server;
    std::uint64_t replay;
};

struct CompareResult {
    std::array<FieldDiff, 96> fields{};
    std::size_t count = 0;
};

std::vector<Frame> read_inputs(const std::string& path) {
    if (path.rfind("scenario:", 0) == 0) {
        if (path != "scenario:combined") throw std::runtime_error("unknown scenario: " + path);
        std::vector<Frame> frames(1800);
        for (std::size_t tick = 0; tick < frames.size(); ++tick) {
            Frame& frame = frames[tick];
            if (tick < 360 || (tick >= 720 && tick < 1440) || tick >= 1440) frame.pad.ly = 0xFF;
            if ((tick >= 360 && tick < 720) || tick >= 1440) frame.pad.rx = 0x00;
            if (tick >= 720 && tick < 1080 && tick % 90 == 0) frame.pad.buttons |= nf::kPadTriangle;
            if (tick == 1080) frame.pad.buttons |= nf::kPadL2;
        }
        return frames;
    }
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open input file " + path);
    std::vector<Frame> frames;
    std::string line;
    while (std::getline(input, line)) {
        if (const std::size_t comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        std::istringstream row(line);
        long tick = 0;
        unsigned word = 0, rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        Frame frame;
        if (!(row >> tick)) continue;
        if (!(row >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly))
            throw std::runtime_error("bad input line: " + line);
        row >> frame.rate;
        if (tick < 0 || rx > 255 || ry > 255 || lx > 255 || ly > 255 || word > 0xFFFF)
            throw std::runtime_error("input value out of range: " + line);
        if (frames.size() <= std::size_t(tick)) frames.resize(std::size_t(tick) + 1);
        frame.pad.buttons = nf::buttons_from_sony_pad_word(std::uint16_t(word));
        frame.pad.rx = std::uint8_t(rx);
        frame.pad.ry = std::uint8_t(ry);
        frame.pad.lx = std::uint8_t(lx);
        frame.pad.ly = std::uint8_t(ly);
        frames[std::size_t(tick)] = frame;
    }
    return frames;
}

void add_float(CompareResult& out, const char* name, float a, float b) {
    const auto aa = std::bit_cast<std::uint32_t>(a);
    const auto bb = std::bit_cast<std::uint32_t>(b);
    if (aa != bb && out.count < out.fields.size()) out.fields[out.count++] = {name, aa, bb};
}

void add_int(CompareResult& out, const char* name, std::uint64_t a, std::uint64_t b) {
    if (a != b && out.count < out.fields.size()) out.fields[out.count++] = {name, a, b};
}

void add_vec(CompareResult& out, const char* x_name, const char* y_name, const char* z_name, const nf::Vec3& a,
            const nf::Vec3& b) {
    add_float(out, x_name, a[0], b[0]);
    add_float(out, y_name, a[1], b[1]);
    add_float(out, z_name, a[2], b[2]);
}

CompareResult compare(const nf::PlayerPredictionState& a, const nf::PlayerPredictionState& b) {
    CompareResult out;
    add_vec(out, "pos.x", "pos.y", "pos.z", a.pos, b.pos);
    add_vec(out, "velocity.x", "velocity.y", "velocity.z", a.velocity, b.velocity);
    add_vec(out, "fall_velocity.x", "fall_velocity.y", "fall_velocity.z", a.fall_velocity, b.fall_velocity);
    add_vec(out, "prev_pos.x", "prev_pos.y", "prev_pos.z", a.prev_pos, b.prev_pos);
    add_vec(out, "prev_velocity.x", "prev_velocity.y", "prev_velocity.z", a.prev_velocity, b.prev_velocity);
    add_vec(out, "settled_pos.x", "settled_pos.y", "settled_pos.z", a.settled_pos, b.settled_pos);
    add_vec(out, "capsule_a.x", "capsule_a.y", "capsule_a.z", a.capsule_a, b.capsule_a);
    add_vec(out, "capsule_b.x", "capsule_b.y", "capsule_b.z", a.capsule_b, b.capsule_b);
    add_float(out, "capsule_radius", a.capsule_radius, b.capsule_radius);
    add_vec(out, "cylinder_push_out.x", "cylinder_push_out.y", "cylinder_push_out.z", a.cylinder_push_out,
            b.cylinder_push_out);
    add_vec(out, "cylinder_a.x", "cylinder_a.y", "cylinder_a.z", a.cylinder_a, b.cylinder_a);
    add_vec(out, "cylinder_b.x", "cylinder_b.y", "cylinder_b.z", a.cylinder_b, b.cylinder_b);
    add_int(out, "cylinder_contact", a.cylinder_contact, b.cylinder_contact);
    add_int(out, "cylinder_hit_count", a.cylinder_hit_count, b.cylinder_hit_count);
    add_float(out, "yaw", a.yaw, b.yaw);
    add_float(out, "pitch", a.pitch, b.pitch);
    add_float(out, "ground_normal_y", a.ground_normal_y, b.ground_normal_y);
    add_float(out, "stand_height", a.stand_height, b.stand_height);
    add_float(out, "applied_height", a.applied_height, b.applied_height);
    add_float(out, "yaw_step", a.yaw_step, b.yaw_step);
    add_float(out, "turn_speed", a.turn_speed, b.turn_speed);
    add_float(out, "pitch_speed", a.pitch_speed, b.pitch_speed);
    add_float(out, "pitch_target", a.pitch_target, b.pitch_target);
    add_float(out, "aim_yaw", a.aim_yaw, b.aim_yaw);
    add_float(out, "fall_timer", a.fall_timer, b.fall_timer);
    add_int(out, "body_flags", a.body_flags, b.body_flags);
    add_int(out, "ground_history", a.ground_history, b.ground_history);
    add_int(out, "anim_random_timer", a.anim_random_timer, b.anim_random_timer);
    add_int(out, "jump_state", a.jump_state, b.jump_state);
    add_int(out, "jump_delay", a.jump_delay, b.jump_delay);
    add_int(out, "crouch_timer", a.crouch_timer, b.crouch_timer);
    add_int(out, "look_state", a.look_state, b.look_state);
    add_int(out, "walk_class", a.walk_class, b.walk_class);
    add_int(out, "enabled", a.enabled, b.enabled);
    add_int(out, "input_frozen", a.input_frozen, b.input_frozen);
    add_int(out, "movement_frozen", a.movement_frozen, b.movement_frozen);
    add_int(out, "scope_aiming", a.scope_aiming, b.scope_aiming);
    add_float(out, "zoom", a.zoom, b.zoom);
    add_int(out, "water.room", std::uint64_t(std::uint32_t(a.water.room)), std::uint64_t(std::uint32_t(b.water.room)));
    add_vec(out, "water.room_from.x", "water.room_from.y", "water.room_from.z", a.water.room_from, b.water.room_from);
    add_float(out, "water.air", a.water.air, b.water.air);
    add_int(out, "water.surfaced", a.water.surfaced, b.water.surfaced);
    add_float(out, "water.meter_alpha", a.water.meter_alpha, b.water.meter_alpha);
    add_int(out, "water.meter_flags", a.water.meter_flags, b.water.meter_flags);
    add_int(out, "water.meter_enabled", a.water.meter_enabled, b.water.meter_enabled);
    add_int(out, "water.frame", a.water.frame, b.water.frame);
    add_int(out, "substate", std::uint16_t(a.substate), std::uint16_t(b.substate));
    const float aim_a[] = {a.aim.cursor_x, a.aim.cursor_y, a.aim.turn_x, a.aim.turn_y, a.aim.scope_x, a.aim.scope_y};
    const float aim_b[] = {b.aim.cursor_x, b.aim.cursor_y, b.aim.turn_x, b.aim.turn_y, b.aim.scope_x, b.aim.scope_y};
    constexpr const char* aim_names[] = {"aim.cursor_x", "aim.cursor_y", "aim.turn_x", "aim.turn_y", "aim.scope_x", "aim.scope_y"};
    for (std::size_t i = 0; i < std::size(aim_a); ++i) add_float(out, aim_names[i], aim_a[i], aim_b[i]);
    add_float(out, "timing.rate", a.timing.rate, b.timing.rate);
    constexpr const char* body_names[] = {"body_basis[0]", "body_basis[1]", "body_basis[2]", "body_basis[3]",
                                         "body_basis[4]", "body_basis[5]", "body_basis[6]", "body_basis[7]",
                                         "body_basis[8]"};
    for (std::size_t i = 0; i < a.body_basis.size(); ++i) add_float(out, body_names[i], a.body_basis[i], b.body_basis[i]);
    return out;
}

struct DiffCounters {
    std::array<std::uint64_t, 96> counts{};
    std::array<const char*, 96> names{};
    std::size_t fields = 0;
    std::uint64_t mismatch_ticks = 0;
    void record(long tick, const CompareResult& result, const char* mode) {
        if (result.count != 0) ++mismatch_ticks;
        for (std::size_t i = 0; i < result.count; ++i) {
            std::size_t slot = 0;
            while (slot < fields && std::string_view(names[slot]) != result.fields[i].name) ++slot;
            if (slot == fields && fields < names.size()) names[fields++] = result.fields[i].name;
            if (slot < counts.size()) ++counts[slot];
            if (slot == fields - 1 && counts[slot] == 1)
                std::printf("diff mode=%s tick=%ld field=%s server=%08llx replay=%08llx\n", mode, tick,
                            result.fields[i].name, (unsigned long long)result.fields[i].server,
                            (unsigned long long)result.fields[i].replay);
        }
    }
    void print(const char* mode, long ticks) const {
        std::printf("state-diff mode=%s ticks=%ld mismatch_ticks=%llu fields=%zu bit_identical=%s\n", mode, ticks,
                    (unsigned long long)mismatch_ticks, fields, mismatch_ticks == 0 ? "yes" : "no");
        for (std::size_t i = 0; i < fields; ++i)
            std::printf("  %s mismatched_ticks=%llu\n", names[i], (unsigned long long)counts[i]);
    }
};

nf::net::PlayerSnapshot snapshot_of(const nf::Player& player, bool full) {
    nf::net::PlayerSnapshot snapshot;
    snapshot.present = true;
    snapshot.alive = player.alive();
    snapshot.x = player.pos[0];
    snapshot.y = player.pos[1];
    snapshot.z = player.pos[2];
    snapshot.yaw = player.yaw;
    snapshot.pitch = player.pitch;
    snapshot.velocity = player.velocity;
    snapshot.substate = std::uint8_t(player.substate);
    if (full) snapshot.owner_movement = nf::app::owner_movement_state(player);
    return snapshot;
}

void restore_partial(nf::Player& player, const nf::net::PlayerSnapshot& snapshot) {
    player.pos = {snapshot.x, snapshot.y, snapshot.z};
    player.yaw = snapshot.yaw;
    player.pitch = snapshot.pitch;
    player.velocity = {snapshot.velocity[0], snapshot.velocity[1], snapshot.velocity[2]};
    player.substate = static_cast<nf::SubState>(snapshot.substate);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: nfnet-predict-diff <game-dir> <level.bin> <inputs.txt> <checkpoint-tick>\n");
        return 2;
    }
    try {
        const std::string game_dir = argv[1];
        const std::string level_name = argv[2];
        const std::vector<Frame> frames = read_inputs(argv[3]);
        const long checkpoint = std::strtol(argv[4], nullptr, 10);
        if (checkpoint < 0 || std::size_t(checkpoint) >= frames.size())
            throw std::runtime_error("checkpoint tick is outside input sequence");

        nf::GameFiles files(game_dir);
        const nf::GameFile* level_file = files.find(level_name);
        const nf::GameFile* tuning_file = files.find("TuningVars.txt");
        if (!level_file) throw std::runtime_error("level not found in game FILES.BIN: " + level_name);
        nf::Level level(files.read(*level_file));
        const nf::Elf32 action_elf(nf::read_file(std::filesystem::path(game_dir) / "ACTION.ELF"));
        const std::vector<nf::SpawnPoint> spawns = nf::find_spawn_points(level);
        if (spawns.empty()) throw std::runtime_error("level has no player spawn point");
        std::string tuning;
        if (tuning_file) {
            const auto bytes = files.read(*tuning_file);
            tuning.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        nf::PlayerParams params = nf::player_params_from_tuning(tuning, "MULTIPLAYER");
        params.health.damage.mode = nf::GameMode::Multiplayer;
        const nf::InputTables tables = nf::InputTables::from_elf(action_elf);
        nf::World server(level, tables, params);
        nf::World partial(level, tables, params);
        nf::World owner(level, tables, params);
        nf::Player& server_player = server.spawn_player(0, spawns.front());
        nf::Player& partial_player = partial.spawn_player(0, spawns.front());
        nf::Player& owner_player = owner.spawn_player(0, spawns.front());
        nf::GameRng server_rng, partial_rng, owner_rng;
        DiffCounters partial_diff, owner_diff;
        bool restored = false;
        long simulated = 0;
        std::uint64_t contact_ticks = 0, airborne_ticks = 0, crouch_ticks = 0, wall_slide_ticks = 0;
        for (std::size_t tick = 0; tick < frames.size(); ++tick) {
            nf::PadInputs pads{};
            pads[0] = frames[tick].pad;
            const nf::FrameTiming timing{frames[tick].rate};
            {
                nf::ScopedGameRng bind(server_rng);
                server.tick(pads, timing);
            }
            ++simulated;
            if ((server_player.body_flags & nf::body::kTouching) != 0) ++contact_ticks;
            if (server_player.jump_state != 0) ++airborne_ticks;
            if (server_player.substate == nf::SubState::Crouch) ++crouch_ticks;
            if (tick >= 1440 &&
                (std::fabs(server_player.prediction_state().cylinder_push_out[0]) > 0.0001f ||
                 std::fabs(server_player.prediction_state().cylinder_push_out[2]) > 0.0001f))
                ++wall_slide_ticks;
            if (long(tick) == checkpoint) {
                const nf::net::PlayerSnapshot snapshot = snapshot_of(server_player, true);
                restore_partial(partial_player, snapshot);
                nf::app::restore_owner_movement_state(owner_player, snapshot);
                partial_rng.seed(server_rng.seed_x(), server_rng.seed_y());
                owner_rng.seed(server_rng.seed_x(), server_rng.seed_y());
                restored = true;
                std::printf("checkpoint tick=%zu pos=%.9g,%.9g,%.9g substate=%u\n", tick, snapshot.x, snapshot.y,
                            snapshot.z, unsigned(snapshot.substate));
                continue;
            }
            if (!restored) continue;
            {
                nf::ScopedGameRng bind(partial_rng);
                (void)partial_rng.rand_int(20000);   // Env_Update, once per authoritative world tick.
                partial.replay_player(0, server.input(0), timing);
            }
            {
                nf::ScopedGameRng bind(owner_rng);
                (void)owner_rng.rand_int(20000);
                owner.replay_player(0, server.input(0), timing);
            }
            partial_diff.record(long(tick), compare(server_player.prediction_state(), partial_player.prediction_state()), "wire");
            owner_diff.record(long(tick), compare(server_player.prediction_state(), owner_player.prediction_state()), "owner");
        }
        if (!restored) throw std::runtime_error("checkpoint was not reached");
        partial_diff.print("wire", simulated);
        owner_diff.print("owner", simulated);
        std::printf("coverage contact_ticks=%llu jump_ticks=%llu crouch_ticks=%llu wall_slide_ticks=%llu\n",
                    (unsigned long long)contact_ticks, (unsigned long long)airborne_ticks,
                    (unsigned long long)crouch_ticks, (unsigned long long)wall_slide_ticks);
        return owner_diff.mismatch_ticks == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nfnet-predict-diff: %s\n", error.what());
        return 1;
    }
}
