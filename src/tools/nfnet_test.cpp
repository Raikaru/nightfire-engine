#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <utility>
#include "net/net.hpp"

namespace {
bool check(bool condition, const char* description) {
    if (!condition) std::fprintf(stderr, "nfnet-test: %s failed\n", description);
    return condition;
}
}  // namespace

int main() {
    using namespace nf::net;
    bool ok = true;
    PadInput input{0x10203040, 0xa55a, {0, 127, 128, 255}}, decoded_input;
    input.view_tick = 0x55667788;
    input.local_player = 2;
    const auto input_bytes = encode_input(input);
    ok &= check(decode_input(input_bytes, decoded_input), "input decode");
    ok &= check(decoded_input.tick == input.tick && decoded_input.buttons == input.buttons &&
                    decoded_input.sticks == input.sticks,
                "input round-trip");
    ok &= check(decoded_input.view_tick == input.view_tick && decoded_input.local_player == input.local_player,
                "input owner/view round-trip");
    ok &= check(!decode_input(std::span(input_bytes).first(9), decoded_input), "truncated input rejection");
    InputBatch batch;
    batch.count = 6;
    batch.samples = {PadInput{10, 1, {255, 128, 0, 128}}, PadInput{10, 2, {128, 128, 128, 128}},
                     PadInput{9, 3, {128, 128, 128, 128}}, PadInput{9, 4, {128, 128, 128, 128}},
                     PadInput{8, 5, {0, 128, 255, 128}}, PadInput{8, 6, {128, 128, 128, 128}}};
    for (std::size_t age = 0; age < 3; ++age) {
        batch.samples[age * 2].view_tick = std::uint32_t(10 - age);
        batch.samples[age * 2 + 1].view_tick = std::uint32_t(10 - age);
        batch.samples[age * 2 + 1].local_player = 1;
    }
    InputBatch decoded_batch;
    ok &= check(decode_input_batch(encode_input_batch(batch), decoded_batch), "two-player redundant batch decode");
    ok &= check(decoded_batch.count == 6 && decoded_batch.samples[0].tick == 10 &&
                    decoded_batch.samples[1].local_player == 1 && decoded_batch.samples[4].buttons == 5,
                "two-player redundant input order and contents");
    ok &= check(decoded_batch.samples[0].view_tick == 10 && decoded_batch.samples[5].view_tick == 8,
                "redundant shooter view ticks retained");
    batch.samples[5].tick = 11;
    ok &= check(encode_input_batch(batch).empty(), "non-monotonic input batch rejection");
    batch.samples[5].tick = 8;
    batch.samples[3].view_tick = 11;
    ok &= check(encode_input_batch(batch).empty(), "decreasing view tick rejection");

    std::array<std::uint8_t, kDataHashBytes> data_hash{};
    data_hash[7] = 0x3c;
    std::array<std::uint8_t, kDataHashBytes> decoded_hash{};
    std::string name, password;
    std::uint8_t local_players = 0;
    ok &= check(decode_hello(encode_hello(data_hash, "Nightfire", "secret", 2), decoded_hash, name, password,
                             local_players),
                "two-player hello decode");
    ok &= check(decoded_hash == data_hash && name == "Nightfire" && password == "secret" && local_players == 2,
                "two-player hello round-trip");
    auto malformed_hello = encode_hello(data_hash, "x", "secret", 2);
    malformed_hello.push_back(0);
    ok &= check(!decode_hello(malformed_hello, decoded_hash, name, password, local_players),
                "malformed hello rejection");
    ok &= check(encode_hello(data_hash, "x", std::string(kMaxPasswordBytes + 1, 'x')).empty(),
                "oversized password rejected");

    ServerInfo info{0x12345678, "Nightfire Local", "07000024.bin", 2, 3, 16, true};
    info.match_revision = 0x8877665544332211ull;
    info.bots = 12;
    info.slot_count = 16;
    ServerInfo decoded_info;
    ok &= check(decode_server_info(encode_server_info(info), decoded_info) && !decoded_info.modified_rules,
                "default server info leaves modified-rules flag clear");
    info.modified_rules = true;
    ok &= check(decode_server_info(encode_server_info(info), decoded_info), "server info decode");
    ok &= check(decoded_info.query_id == info.query_id && decoded_info.name == info.name &&
                    decoded_info.map == info.map && decoded_info.mode == info.mode &&
                    decoded_info.players == info.players && decoded_info.bots == info.bots &&
                    decoded_info.slot_count == info.slot_count && decoded_info.modified_rules &&
                    decoded_info.password_required && decoded_info.match_revision == info.match_revision,
                "server info round-trip including revision, custom rules, and extended capacity");
    auto invalid_server_info = encode_server_info(info);
    invalid_server_info[invalid_server_info.size() - 3] = std::uint8_t(kMaxServerBots + 1);
    ok &= check(!decode_server_info(invalid_server_info, decoded_info), "server info rejects excess bot count");
    invalid_server_info = encode_server_info(info);
    invalid_server_info.back() = 2;
    ok &= check(!decode_server_info(invalid_server_info, decoded_info), "server info rejects invalid rules flag");
    info.bots = std::uint8_t(kMaxServerBots + 1);
    ok &= check(encode_server_info(info).empty(), "server info encoder rejects excess bot count");
    info.bots = kMaxServerBots;
    info.players = 0;
    ok &= check(decode_server_info(encode_server_info(info), decoded_info) &&
                    decoded_info.bots == kMaxServerBots && decoded_info.slot_count == 16 &&
                    decoded_info.modified_rules,
                "extended server info accepts sixteen bot slots");

    ServerInfo ps2_boundary = info;
    ps2_boundary.max_players = 4;
    ps2_boundary.slot_count = 8;
    ps2_boundary.players = 4;
    ps2_boundary.bots = 4;
    auto ps2_bytes = encode_server_info(ps2_boundary);
    ok &= check(decode_server_info(ps2_bytes, decoded_info) && decoded_info.players == 4 &&
                    decoded_info.max_players == 4 && decoded_info.bots == 4,
                "PS2 server info accepts four humans and four bots");
    auto wrong_capacity = ps2_bytes;
    wrong_capacity[17] = 5;
    ok &= check(!decode_server_info(wrong_capacity, decoded_info), "server info rejects mismatched human capacity");
    ps2_boundary.max_players = 5;
    ok &= check(encode_server_info(ps2_boundary).empty(), "server info encoder rejects mismatched human capacity");
    ps2_boundary.max_players = 4;
    auto too_many_legacy_bots = ps2_bytes;
    too_many_legacy_bots[too_many_legacy_bots.size() - 3] = 5;
    ok &= check(!decode_server_info(too_many_legacy_bots, decoded_info), "PS2 server info rejects a fifth bot");
    ps2_boundary.bots = 5;
    ok &= check(encode_server_info(ps2_boundary).empty(), "server info encoder enforces legacy bot ceiling");

    ServerInfo gc_boundary = ps2_boundary;
    gc_boundary.slot_count = 10;
    gc_boundary.bots = 6;
    auto gc_bytes = encode_server_info(gc_boundary);
    ok &= check(decode_server_info(gc_bytes, decoded_info) && decoded_info.bots == 6 &&
                    decoded_info.slot_count == 10,
                "GC/Xbox server info accepts six bots");
    auto too_many_gc_bots = gc_bytes;
    too_many_gc_bots[too_many_gc_bots.size() - 3] = 7;
    ok &= check(!decode_server_info(too_many_gc_bots, decoded_info), "GC/Xbox server info rejects a seventh bot");

    ServerInfo over_capacity = info;
    over_capacity.players = 1;
    ok &= check(encode_server_info(over_capacity).empty(), "server info encoder rejects occupied slots over capacity");
    auto extended_bytes = encode_server_info(info);
    extended_bytes[16] = 1;
    ok &= check(!decode_server_info(extended_bytes, decoded_info), "server info decoder rejects occupied slots over capacity");
    std::uint32_t query_id = 0;
    ok &= check(decode_server_query(encode_server_query(info.query_id), query_id) && query_id == info.query_id,
                "server query round-trip");

    UdpSocket broadcast_socket;
    std::string socket_error;
    ok &= check(broadcast_socket.bind(0, &socket_error), "UDP ephemeral bind for LAN query");
    ok &= check(broadcast_socket.enable_broadcast(&socket_error), "UDP broadcast permission");
    Packet lan_query;
    lan_query.header.message = Message::ServerQuery;
    lan_query.payload = encode_server_query(info.query_id);
    ok &= check(broadcast_socket.send("255.255.255.255", 27500, lan_query), "UDP LAN broadcast send");

    Snapshot source;
    source.tick = 12345;
    source.match_revision = 0x1020304050607080ull;
    source.ack_input_tick = 12340;
    source.slot_count = 2;
    source.match_phase = 1;
    source.state_code = 3;
    source.score_limit = 10;
    source.elapsed = 120.0f;
    source.time_left = 480.0f;
    source.team_score = {3.0f, 2.0f};
    PlayerSnapshot p0;
    p0.slot = 0; p0.present = true; p0.alive = true; p0.x = 1.25f; p0.y = -2.0f; p0.z = 3.5f;
    p0.yaw = 0.75f; p0.pitch = -0.25f; p0.health = 100; p0.armor = 25; p0.kills = 8; p0.deaths = 3;
    p0.score = 12; p0.points = 6.25f; p0.velocity = {0.5f, 0.0f, -1.0f}; p0.team = 0;
    p0.substate = 4; p0.weapon = 12; p0.weapon_anim = 6; p0.weapon_clip = 4; p0.weapon_ammo = 48;
    p0.character = 3; p0.name = "Player A"; p0.aiming = true;
    PlayerSnapshot p1;
    p1.slot = 1; p1.present = true; p1.bot = true; p1.x = -9.0f; p1.y = 0; p1.z = 4; p1.yaw = -1.0f;
    p1.deaths = 4; p1.visible = false; p1.radar_x = 42.0f; p1.name = "Rook";
    source.players = {p0, p1};
    const auto snapshot_bytes = encode_snapshot(source);
    Snapshot decoded_snapshot;
    ok &= check(decode_snapshot(snapshot_bytes, decoded_snapshot), "snapshot decode");
    ok &= check(decoded_snapshot.tick == source.tick && decoded_snapshot.ack_input_tick == source.ack_input_tick &&
                    decoded_snapshot.slot_count == 2 && decoded_snapshot.match_phase == 1 &&
                    decoded_snapshot.score_limit == 10 && decoded_snapshot.time_left == 480.0f &&
                    decoded_snapshot.match_revision == source.match_revision,
                "authoritative match snapshot round-trip including revision");
    ok &= check(decoded_snapshot.players[0].x == 1.25f && decoded_snapshot.players[0].kills == 8 &&
                    decoded_snapshot.players[0].score == 12 && decoded_snapshot.players[0].points == 6.25f &&
                    decoded_snapshot.players[0].weapon == 12 && decoded_snapshot.players[0].weapon_clip == 4 &&
                    decoded_snapshot.players[0].pitch == -0.25f && decoded_snapshot.players[0].name == "Player A",
                "player scoreboard, animation and weapon state round-trip");
    ok &= check(decoded_snapshot.players[1].present && !decoded_snapshot.players[1].visible &&
                    decoded_snapshot.players[1].radar_x == 42.0f && decoded_snapshot.players[1].name == "Rook",
                "snapshot visibility, names and radar round-trip");
    auto truncated = snapshot_bytes;
    truncated.pop_back();
    ok &= check(!decode_snapshot(truncated, decoded_snapshot), "truncated snapshot rejection");
    Snapshot ten_slots;
    ten_slots.slot_count = 10;
    ten_slots.players.reserve(ten_slots.slot_count);
    for (std::uint8_t slot = 0; slot < ten_slots.slot_count; ++slot) {
        PlayerSnapshot player;
        player.slot = slot;
        player.bot = slot >= 4;
        player.name = slot >= 4 ? "Bot" : "Player";
        ten_slots.players.push_back(std::move(player));
    }
    const auto ten_slot_pages = split_snapshot(ten_slots);
    std::vector<PlayerSnapshot> decoded_ten_players;
    bool ten_slot_pages_decoded = ten_slot_pages.size() == 5;
    for (const Snapshot& page : ten_slot_pages) {
        const auto bytes = encode_snapshot(page);
        Snapshot decoded_page;
        ten_slot_pages_decoded &= decode_snapshot(bytes, decoded_page) &&
                                  decoded_page.page_index == page.page_index &&
                                  decoded_page.page_count == 5;
        if (ten_slot_pages_decoded)
            decoded_ten_players.insert(decoded_ten_players.end(), decoded_page.players.begin(),
                                       decoded_page.players.end());
    }
    ok &= check(ten_slot_pages_decoded && decoded_ten_players.size() == 10 &&
                    decoded_ten_players[9].slot == 9 && decoded_ten_players[9].name == "Bot",
                "ten-slot arena snapshot page round-trip");
    Snapshot extended_slots = ten_slots;
    extended_slots.slot_count = 16;
    for (std::uint8_t slot = 10; slot < extended_slots.slot_count; ++slot) {
        PlayerSnapshot player;
        player.slot = slot;
        player.bot = true;
        player.name = "Bot";
        if (slot == 14) {
            player.present = true;
            player.bot = false;
            player.owner_movement = OwnerMovementState{};
        }
        extended_slots.players.push_back(std::move(player));
    }
    const auto extended_pages = split_snapshot(extended_slots);
    bool extended_pages_decoded = extended_pages.size() == 8;
    bool extended_owner_movement_decoded = false;
    std::size_t extended_player_count = 0;
    for (const Snapshot& page : extended_pages) {
        const auto bytes = encode_snapshot(page);
        Snapshot decoded_page;
        extended_pages_decoded &= decode_snapshot(bytes, decoded_page);
        extended_player_count += decoded_page.players.size();
        if (decoded_page.page_index == 7 && !decoded_page.players.empty())
            extended_owner_movement_decoded = decoded_page.players[0].owner_movement.has_value();
    }
    ok &= check(extended_pages_decoded && extended_player_count == 16 && extended_owner_movement_decoded,
                "sixteen-slot owner movement snapshot page round-trip");
    auto duplicate_slot = snapshot_bytes;
    duplicate_slot[40 + 74 + p0.name.size()] = 0;
    ok &= check(!decode_snapshot(duplicate_slot, decoded_snapshot), "duplicate slot rejection");

    OwnerMovementState movement;
    movement.fall_velocity = {0.25f, -9.8f, 1.5f};
    movement.body_flags = 0x18;
    movement.ground_normal_y = 0.93f;
    movement.jump_state = 2;
    movement.ground_history = 0xB;
    movement.anim_random_timer = 0x155;
    movement.stand_height = 0.91f;
    movement.applied_height = 0.94f;
    movement.settled_pos = {1.0f, 2.0f, 3.0f};
    movement.prev_pos = {4.0f, 5.0f, 6.0f};
    movement.prev_velocity = {-1.0f, 0.5f, 0.25f};
    movement.yaw_step = 0.12f;
    movement.fall_timer = 18.0f;
    movement.enabled = 2;
    movement.input_frozen = true;
    movement.movement_frozen = false;
    movement.jump_delay = 3;
    movement.crouch_timer = 7;
    movement.turn_speed = 0.04f;
    movement.pitch_speed = 0.01f;
    movement.pitch_target = -0.2f;
    movement.aim_yaw = 0.15f;
    movement.scope_aiming = true;
    movement.zoom = 2.5f;
    movement.aim_state = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    movement.timing_rate = 29.97f;
    movement.body_basis = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    movement.look_state = 2;
    movement.water_room = -1;
    movement.water_room_from = {1.25f, -2.5f, 3.75f};
    movement.water_air = 42.0f;
    movement.water_surfaced = false;
    movement.water_meter_alpha = 0.625f;
    movement.water_meter_flags = 0x10;
    movement.water_meter_enabled = true;
    movement.water_frame = 0x1122334455667788ull;
    movement.walk_class = 3;
    Snapshot owner_source = source;
    owner_source.players[0].owner_movement = movement;
    const auto owner_bytes = encode_snapshot(owner_source);
    Snapshot decoded_owner;
    const bool owner_decoded = decode_snapshot(owner_bytes, decoded_owner);
    ok &= check(owner_decoded && decoded_owner.players[0].owner_movement.has_value() &&
                    !decoded_owner.players[1].owner_movement.has_value(),
                "owner movement block is present only on its encoded player record");
    if (owner_decoded && decoded_owner.players[0].owner_movement) {
        const OwnerMovementState& actual = *decoded_owner.players[0].owner_movement;
        ok &= check(actual.fall_velocity == movement.fall_velocity && actual.body_flags == movement.body_flags &&
                        actual.ground_normal_y == movement.ground_normal_y &&
                        actual.jump_state == movement.jump_state && actual.ground_history == movement.ground_history &&
                        actual.anim_random_timer == movement.anim_random_timer &&
                        actual.stand_height == movement.stand_height && actual.applied_height == movement.applied_height &&
                        actual.settled_pos == movement.settled_pos && actual.prev_pos == movement.prev_pos &&
                        actual.prev_velocity == movement.prev_velocity && actual.yaw_step == movement.yaw_step &&
                        actual.fall_timer == movement.fall_timer && actual.enabled == movement.enabled &&
                        actual.input_frozen == movement.input_frozen &&
                        actual.movement_frozen == movement.movement_frozen &&
                        actual.jump_delay == movement.jump_delay && actual.crouch_timer == movement.crouch_timer &&
                        actual.turn_speed == movement.turn_speed && actual.pitch_speed == movement.pitch_speed &&
                        actual.pitch_target == movement.pitch_target && actual.aim_yaw == movement.aim_yaw &&
                        actual.scope_aiming == movement.scope_aiming && actual.zoom == movement.zoom &&
                        actual.aim_state == movement.aim_state && actual.timing_rate == movement.timing_rate &&
                        actual.walk_class == movement.walk_class && actual.water_room == movement.water_room &&
                        actual.water_room_from == movement.water_room_from && actual.water_air == movement.water_air &&
                        actual.water_surfaced == movement.water_surfaced &&
                        actual.water_meter_alpha == movement.water_meter_alpha &&
                        actual.water_meter_flags == movement.water_meter_flags &&
                        actual.water_meter_enabled == movement.water_meter_enabled &&
                        actual.water_frame == movement.water_frame,
                    "owner movement schema-v2 state round-trip");
    }
    auto owner_truncated = owner_bytes;
    owner_truncated.pop_back();
    ok &= check(!decode_snapshot(owner_truncated, decoded_owner), "truncated owner movement block rejected");
    const std::size_t owner_block = 40 + 74 + p0.name.size();
    auto owner_old_schema = owner_bytes;
    owner_old_schema[owner_block] = 1;
    ok &= check(!decode_snapshot(owner_old_schema, decoded_owner), "old owner movement schema rejected");
    auto owner_unknown_schema = owner_bytes;
    owner_unknown_schema[owner_block] = std::uint8_t(kOwnerMovementSchemaVersion + 1);
    ok &= check(!decode_snapshot(owner_unknown_schema, decoded_owner), "unknown owner movement schema rejected");
    auto owner_bad_boolean = owner_bytes;
    owner_bad_boolean[owner_block + 77] = 2;
    ok &= check(!decode_snapshot(owner_bad_boolean, decoded_owner), "invalid owner movement boolean rejected");
    auto owner_bad_scope_flag = owner_bytes;
    owner_bad_scope_flag[owner_block + 97] = 2;
    ok &= check(!decode_snapshot(owner_bad_scope_flag, decoded_owner),
                "invalid scoped-aim movement boolean rejected");
    auto owner_bad_water_surfaced = owner_bytes;
    owner_bad_water_surfaced[owner_block + 188] = 2;
    ok &= check(!decode_snapshot(owner_bad_water_surfaced, decoded_owner),
                "invalid water surfaced flag rejected");
    auto owner_bad_water_meter = owner_bytes;
    owner_bad_water_meter[owner_block + 197] = 2;
    ok &= check(!decode_snapshot(owner_bad_water_meter, decoded_owner),
                "invalid water meter flag rejected");
    Snapshot bot_owner = source;
    bot_owner.players[1].owner_movement = movement;
    ok &= check(encode_snapshot(bot_owner).empty(), "bot owner movement block rejected");
    const std::size_t movement_bytes = owner_bytes.size() - snapshot_bytes.size();
    const std::size_t bot_record = owner_block + movement_bytes;
    const std::size_t bot_record_end = bot_record + 74 + p1.name.size();
    auto movement_on_bot = owner_bytes;
    movement_on_bot[40 + 1] &= std::uint8_t(~64u);
    movement_on_bot[bot_record + 1] |= 64;
    std::vector<std::uint8_t> malformed_bot_block(movement_on_bot.begin(),
                                                  movement_on_bot.begin() + std::ptrdiff_t(owner_block));
    malformed_bot_block.insert(malformed_bot_block.end(),
                               movement_on_bot.begin() + std::ptrdiff_t(bot_record),
                               movement_on_bot.begin() + std::ptrdiff_t(bot_record_end));
    malformed_bot_block.insert(malformed_bot_block.end(),
                               movement_on_bot.begin() + std::ptrdiff_t(owner_block),
                               movement_on_bot.begin() + std::ptrdiff_t(owner_block + movement_bytes));
    ok &= check(!decode_snapshot(malformed_bot_block, decoded_owner), "owner movement block on bot rejected");
    Snapshot multiple_owners = owner_source;
    multiple_owners.players[1].bot = false;
    multiple_owners.players[1].owner_movement = movement;
    Snapshot decoded_multiple_owners;
    const auto multiple_owner_bytes = encode_snapshot(multiple_owners);
    ok &= check(decode_snapshot(multiple_owner_bytes, decoded_multiple_owners) &&
                    decoded_multiple_owners.players[0].owner_movement.has_value() &&
                    decoded_multiple_owners.players[1].owner_movement.has_value(),
                "two local owner movement blocks round-trip");

    WorldState source_world;
    source_world.tick = 12345;
    source_world.pickups = {{2, 1, 0.75f}, {9, 2, 1.25f}};
    source_world.objectives = {{0, 0, 1, -1, 1, true, 1.0f, 2.0f, 3.0f, 2000.0f}};
    WorldState decoded_world;
    ok &= check(decode_world_state(encode_world_state(source_world), decoded_world), "world state decode");
    ok &= check(decoded_world.tick == source_world.tick && decoded_world.pickups.size() == 2 &&
                    decoded_world.pickups[1].state == 2 && decoded_world.objectives[0].hit_points == 2000.0f,
                "pickup and objective state round-trip");

    std::vector<ProjectileSnapshot> projectile_source(33);
    for (std::size_t i = 0; i < projectile_source.size(); ++i) {
        projectile_source[i].id = std::uint16_t(i);
        projectile_source[i].weapon = 6;
        projectile_source[i].owner = 1;
        projectile_source[i].state = 1;
        projectile_source[i].position = {float(i), 2.0f, 3.0f};
        projectile_source[i].direction = {0.0f, 0.0f, 1.0f};
        projectile_source[i].age = float(i) / 30.0f;
    }
    const auto projectile_pages = split_projectiles(source.tick, projectile_source);
    ok &= check(projectile_pages.size() == 2, "projectile snapshot fragmentation");
    ProjectilePage decoded_page;
    ok &= check(decode_projectile_page(encode_projectile_page(projectile_pages[0]), decoded_page) &&
                    decoded_page.tick == source.tick && decoded_page.total == 33 &&
                    decoded_page.index == 0 && decoded_page.projectiles.size() == 32 &&
                    decoded_page.projectiles[31].id == 31,
                "full projectile page round-trip");
    ok &= check(decode_projectile_page(encode_projectile_page(projectile_pages[1]), decoded_page) &&
                    decoded_page.tick == source.tick && decoded_page.total == 33 &&
                    decoded_page.index == 1 && decoded_page.projectiles.size() == 1 &&
                    decoded_page.projectiles[0].id == 32,
                "partial projectile page round-trip");

    ReplicationEvent source_event;
    source_event.id = 5; source_event.tick = 12345; source_event.kind = EventKind::Explosion;
    source_event.actor = 1; source_event.weapon = 6; source_event.position = {1.0f, 2.0f, 3.0f};
    source_event.radius = 8.0f; source_event.script = 0x06000052; source_event.text = "blast";
    ReplicationEvent decoded_event;
    ok &= check(decode_event(encode_event(source_event), decoded_event), "replicated event decode");
    ok &= check(decoded_event.id == 5 && decoded_event.script == source_event.script &&
                    decoded_event.position[2] == 3.0f && decoded_event.text == "blast",
                "effects and event text round-trip");

    Packet packet;
    packet.header.message = Message::Input;
    packet.header.sequence = 5;
    packet.payload = {1, 2, 3};
    auto datagram = encode(packet);
    const auto round_trip = decode(datagram);
    ok &= check(round_trip && round_trip->payload == packet.payload && round_trip->header.sequence == 5, "packet round-trip");
    datagram[0] ^= 0xff;
    ok &= check(!decode(datagram), "bad magic rejection");

    const auto now = std::chrono::steady_clock::now();
    Reliability sender, receiver;
    Packet event;
    event.header.message = Message::Event;
    const Packet reliable = sender.prepare(event, true, now);
    ok &= check(sender.retransmit_due(now + std::chrono::milliseconds(99)).empty(), "retransmit timer lower bound");
    ok &= check(sender.retransmit_due(now + std::chrono::milliseconds(100)).size() == 1, "retransmit timer boundary");
    ok &= check(receiver.observe(reliable.header), "first reliable packet is fresh");
    ok &= check(!receiver.observe(reliable.header), "duplicate packet is suppressed");
    Header later; later.sequence = 3;
    Header reordered; reordered.sequence = 2;
    ok &= check(receiver.observe(later) && receiver.observe(reordered), "reordered packet is fresh");
    ok &= check(!receiver.observe(reordered), "reordered duplicate is suppressed");
    Packet ack;
    const Packet ack_packet = receiver.prepare(ack, false, now);
    sender.observe(ack_packet.header);
    ok &= check(sender.retransmit_due(now + std::chrono::milliseconds(200)).empty(), "selective acknowledgement");
    if (ok) std::puts("nfnet-test: all checks passed");
    return ok ? 0 : 1;
}
