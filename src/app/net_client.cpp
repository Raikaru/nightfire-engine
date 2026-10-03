#include "app/net_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "assets/game_files.hpp"
#include "game/input.hpp"
#include "net/net.hpp"
#include "game/world.hpp"

namespace nf::app {
namespace {

using Clock = std::chrono::steady_clock;

struct Endpoint { std::string host; std::uint16_t port = 27500; };
Endpoint parse_endpoint(std::string_view text) {
    const auto colon = text.rfind(':');
    Endpoint endpoint;
    endpoint.host = std::string(text.substr(0, colon));
    if (colon != std::string_view::npos) {
        const std::string port_text(text.substr(colon + 1));
        char* end = nullptr;
        const long port = std::strtol(port_text.c_str(), &end, 10);
        if (port_text.empty() || !end || *end || port < 1 || port > 65535) throw std::runtime_error("invalid --connect port");
        endpoint.port = std::uint16_t(port);
    }
    if (endpoint.host.empty()) throw std::runtime_error("--connect requires an IPv4 address");
    return endpoint;
}

std::vector<nf::PadState> parse_press_script(const std::string& script) {
    static const std::map<std::string, std::uint16_t> buttons = {
        {"up", nf::kPadUp}, {"down", nf::kPadDown}, {"left", nf::kPadLeft}, {"right", nf::kPadRight},
        {"cross", nf::kPadCross}, {"circle", nf::kPadCircle}, {"square", nf::kPadSquare},
        {"triangle", nf::kPadTriangle}, {"start", nf::kPadStart}, {"select", nf::kPadSelect},
        {"l1", nf::kPadL1}, {"l2", nf::kPadL2}, {"r1", nf::kPadR1}, {"r2", nf::kPadR2}};
    std::vector<nf::PadState> result;
    for (std::size_t at = 0; at < script.size();) {
        const auto end = script.find(',', at);
        const std::string token = script.substr(at, end == std::string::npos ? end : end - at);
        at = end == std::string::npos ? script.size() : end + 1;
        if (token.rfind("wait", 0) == 0) {
            int count = token.size() > 4 ? std::atoi(token.c_str() + 4) : 1;
            if (count < 0 || count > 18000) throw std::runtime_error("--press wait count out of range");
            result.insert(result.end(), std::size_t(count), nf::PadState{});
            continue;
        }
        nf::PadState state;
        for (std::size_t part = 0; part < token.size();) {
            const auto plus = token.find('+', part);
            const std::string key = token.substr(part, plus == std::string::npos ? plus : plus - part);
            part = plus == std::string::npos ? token.size() : plus + 1;
            if (key == "forward") state.ly = 0;
            else if (key == "back") state.ly = 255;
            else if (key == "strafe-left") state.lx = 0;
            else if (key == "strafe-right") state.lx = 255;
            else if (const auto it = buttons.find(key); it != buttons.end()) state.buttons |= it->second;
            else throw std::runtime_error("unknown --press token " + key);
        }
        result.push_back(state);
    }
    return result;
}

std::string hash_to_string(const std::vector<nf::net::PlayerSnapshot>& players) {
    std::string out;
    for (const auto& player : players) {
        if (!player.present) continue;
        if (!out.empty()) out += " | ";
        out += "slot" + std::to_string(player.slot) + (player.bot ? " bot" : " human") + " S" +
               std::to_string(player.score) + " K" + std::to_string(player.kills) + " D" +
               std::to_string(player.deaths) + " P" + std::to_string(player.points);
    }
    return out;
}
}  // namespace

struct NetworkSession::Impl {
    Impl(AppContext& ctx, NetworkClientOptions opts)
        : endpoint(parse_endpoint(opts.endpoint)), options(std::move(opts)) {
        if (options.chat.size() > 200) throw std::runtime_error("chat message is limited to 200 bytes");
        const nf::GameFile* map_file = ctx.files.find(options.map);
        if (!map_file) throw std::runtime_error("map not found in game data: " + options.map);
        const auto map_bytes = ctx.files.read(*map_file);
        const auto elf_bytes = nf::read_file(std::filesystem::path(ctx.gamedir) / "ACTION.ELF");
        const auto hash = nf::net::game_data_hash(elf_bytes, map_bytes);
        socket.simulate({unsigned(options.loss_percent), unsigned(options.latency_ms), 0x4e46434c, options.jitter_ms});
        std::string error;
        if (!socket.bind(0, &error)) throw std::runtime_error("network client " + error);
        hello.header.message = nf::net::Message::Hello;
        hello.payload = nf::net::encode_hello(hash, options.name, options.password, options.local_players);
        if (hello.payload.empty()) throw std::runtime_error("invalid local player count or password length");
        hello = reliability.prepare(std::move(hello), true, Clock::now());
    }

    Endpoint endpoint;
    NetworkClientOptions options;
    nf::net::UdpSocket socket;
    nf::net::Reliability reliability;
    nf::net::Packet hello;
    std::uint8_t slot = 0xff;
    std::uint8_t local_player_count = 0;
    std::uint32_t input_tick = 0, server_tick = 0, latest_snapshot_tick = 0;
    std::array<std::array<nf::net::PadInput, nf::net::kInputRedundancy>, nf::net::kMaxLocalPlayers> input_history{};
    std::uint8_t input_history_count = 0;
    bool chat_sent = false;
    Clock::time_point last_hello{}, last_server = Clock::now();
    struct ProjectileAssembly {
        std::uint16_t total = 0;
        std::uint8_t count = 0;
        std::vector<std::optional<std::vector<nf::net::ProjectileSnapshot>>> pages;
    };
    struct SnapshotAssembly {
        nf::net::Snapshot base;
        std::vector<std::optional<std::vector<nf::net::PlayerSnapshot>>> pages;
    };
    std::vector<nf::net::Snapshot> snapshots;
    std::map<std::uint32_t, SnapshotAssembly> snapshot_assemblies;
    std::optional<nf::net::Snapshot> latest_snapshot;
    std::optional<nf::net::WorldState> world_state;
    std::map<std::uint32_t, ProjectileAssembly> projectile_assemblies;
    std::vector<nf::net::ProjectileSnapshot> replicated_projectiles;
    std::uint32_t replicated_projectiles_tick = 0;
    std::vector<nf::net::ReplicationEvent> events;
    std::vector<std::string> chats;
};

NetworkSession::NetworkSession(AppContext& ctx, NetworkClientOptions options)
    : impl_(std::make_unique<Impl>(ctx, std::move(options))) {}
NetworkSession::~NetworkSession() = default;

void NetworkSession::poll() {
    Impl& s = *impl_;
    const auto now = Clock::now();
    if (s.slot == 0xff && now - s.last_hello >= std::chrono::milliseconds(500)) {
        if (!s.socket.send(s.endpoint.host, s.endpoint.port, s.hello))
            throw std::runtime_error("failed to send handshake");
        s.last_hello = now;
    }
    for (const nf::net::Received& incoming : s.socket.receive()) {
        if (incoming.address != s.endpoint.host || incoming.port != s.endpoint.port) continue;
        const bool fresh = s.reliability.observe(incoming.packet.header);
        s.last_server = now;
        if (!fresh) continue;
        if (incoming.packet.header.message == nf::net::Message::Reject)
            throw std::runtime_error(std::string(incoming.packet.payload.begin(), incoming.packet.payload.end()));
        else if (incoming.packet.header.message == nf::net::Message::Welcome &&
                 incoming.packet.payload.size() == 6) {
            if (s.slot != 0xff) continue;
            if (std::size_t(incoming.packet.payload[0]) + incoming.packet.payload[1] > nf::World::kMaxPlayers ||
                incoming.packet.payload[1] != s.options.local_players)
                throw std::runtime_error("server returned invalid local player slots");
            s.slot = incoming.packet.payload[0];
            s.local_player_count = incoming.packet.payload[1];
            s.server_tick = std::uint32_t(incoming.packet.payload[2]) |
                            (std::uint32_t(incoming.packet.payload[3]) << 8) |
                            (std::uint32_t(incoming.packet.payload[4]) << 16) |
                            (std::uint32_t(incoming.packet.payload[5]) << 24);
            s.input_tick = s.server_tick;
            if (!s.chat_sent && !s.options.chat.empty()) {
                nf::net::Packet chat;
                chat.header.message = nf::net::Message::Chat;
                chat.payload.assign(s.options.chat.begin(), s.options.chat.end());
                if (!s.socket.send(s.endpoint.host, s.endpoint.port,
                                   s.reliability.prepare(std::move(chat), true, now)))
                    throw std::runtime_error("failed to send chat");
                s.chat_sent = true;
            }
        } else if (incoming.packet.header.message == nf::net::Message::Snapshot) {
            nf::net::Snapshot page;
            if (!nf::net::decode_snapshot(incoming.packet.payload, page) || page.tick <= s.latest_snapshot_tick)
                continue;
            auto [it, inserted] = s.snapshot_assemblies.try_emplace(page.tick);
            auto& assembly = it->second;
            if (inserted) {
                assembly.base = page;
                assembly.base.players.clear();
                assembly.pages.resize(page.page_count);
            } else if (assembly.base.slot_count != page.slot_count ||
                       assembly.base.page_count != page.page_count ||
                       assembly.base.ack_input_tick != page.ack_input_tick ||
                       assembly.base.match_phase != page.match_phase ||
                       assembly.base.state_code != page.state_code ||
                       assembly.base.score_limit != page.score_limit ||
                       assembly.base.elapsed != page.elapsed ||
                       assembly.base.time_left != page.time_left ||
                       assembly.base.team_score != page.team_score ||
                       assembly.base.match_revision != page.match_revision) {
                s.snapshot_assemblies.erase(it);
                continue;
            }
            if (!assembly.pages[page.page_index])
                assembly.pages[page.page_index] = std::move(page.players);
            if (std::all_of(assembly.pages.begin(), assembly.pages.end(),
                            [](const auto& part) { return part.has_value(); })) {
                nf::net::Snapshot assembled = std::move(assembly.base);
                assembled.page_index = 0;
                assembled.page_count = 1;
                assembled.players.reserve(assembled.slot_count);
                for (auto& part : assembly.pages)
                    assembled.players.insert(assembled.players.end(), part->begin(), part->end());
                if (assembled.players.size() == assembled.slot_count && assembled.tick > s.latest_snapshot_tick) {
                    s.latest_snapshot_tick = assembled.tick;
                    if (s.snapshots.size() == 32) s.snapshots.erase(s.snapshots.begin());
                    s.latest_snapshot = assembled;
                    s.snapshots.push_back(std::move(assembled));
                    for (auto old = s.snapshot_assemblies.begin();
                         old != s.snapshot_assemblies.end() && old->first <= page.tick;)
                        old = s.snapshot_assemblies.erase(old);
                } else {
                    s.snapshot_assemblies.erase(it);
                }
            }
            while (s.snapshot_assemblies.size() > 4)
                s.snapshot_assemblies.erase(s.snapshot_assemblies.begin());
        } else if (incoming.packet.header.message == nf::net::Message::WorldState) {
            nf::net::WorldState state;
            if (nf::net::decode_world_state(incoming.packet.payload, state) &&
                (!s.world_state || state.tick > s.world_state->tick))
                s.world_state = std::move(state);
        } else if (incoming.packet.header.message == nf::net::Message::Projectiles) {
            nf::net::ProjectilePage page;
            if (!nf::net::decode_projectile_page(incoming.packet.payload, page) ||
                page.tick < s.replicated_projectiles_tick)
                continue;
            auto [it, inserted] = s.projectile_assemblies.try_emplace(page.tick);
            auto& assembly = it->second;
            if (inserted) {
                assembly.total = page.total;
                assembly.count = page.count;
                assembly.pages.resize(page.count);
            } else if (assembly.total != page.total || assembly.count != page.count) {
                s.projectile_assemblies.erase(it);
                continue;
            }
            if (!assembly.pages[page.index])
                assembly.pages[page.index] = std::move(page.projectiles);
            if (std::all_of(assembly.pages.begin(), assembly.pages.end(),
                            [](const auto& part) { return part.has_value(); })) {
                std::vector<nf::net::ProjectileSnapshot> assembled;
                assembled.reserve(assembly.total);
                for (auto& part : assembly.pages)
                    assembled.insert(assembled.end(), part->begin(), part->end());
                if (assembled.size() == assembly.total) {
                    s.replicated_projectiles = std::move(assembled);
                    s.replicated_projectiles_tick = page.tick;
                    for (auto old = s.projectile_assemblies.begin();
                         old != s.projectile_assemblies.end() && old->first <= page.tick;)
                        old = s.projectile_assemblies.erase(old);
                }
            }
            while (s.projectile_assemblies.size() > 4) s.projectile_assemblies.erase(s.projectile_assemblies.begin());
        } else if (incoming.packet.header.message == nf::net::Message::Event) {
            nf::net::ReplicationEvent event;
            if (nf::net::decode_event(incoming.packet.payload, event)) {
                if (s.events.size() == 128) s.events.erase(s.events.begin());
                s.events.push_back(std::move(event));
            }
        } else if (incoming.packet.header.message == nf::net::Message::Chat) {
            if (s.chats.size() == 64) s.chats.erase(s.chats.begin());
            s.chats.emplace_back(incoming.packet.payload.begin(), incoming.packet.payload.end());
        }
    }
    for (const nf::net::Packet& pending : s.reliability.retransmit_due(now))
        s.socket.send(s.endpoint.host, s.endpoint.port, pending);
    if (now - s.last_server > std::chrono::seconds(10))
        throw std::runtime_error(s.slot == 0xff ? "connection timed out before handshake completed" : "server timed out");
}

void NetworkSession::wait_for_connection() {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!connected() && Clock::now() < deadline) {
        poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!connected()) throw std::runtime_error("timed out waiting for server welcome");
}

void NetworkSession::send_inputs(std::span<const nf::PadState> pads, std::uint32_t view_tick) {
    Impl& s = *impl_;
    if (s.slot == 0xff) return;
    if (pads.size() != s.local_player_count) throw std::runtime_error("local input count does not match welcome");
    nf::net::InputBatch batch;
    const std::uint32_t tick = ++s.input_tick;
    for (std::size_t local = 0; local < pads.size(); ++local) {
        nf::net::PadInput input;
        input.tick = tick;
        input.view_tick = view_tick;
        input.buttons = pads[local].buttons;
        input.sticks = {pads[local].rx, pads[local].ry, pads[local].lx, pads[local].ly};
        input.local_player = std::uint8_t(local);
        for (std::size_t age = std::min<std::size_t>(s.input_history_count, nf::net::kInputRedundancy - 1);
             age > 0; --age)
            s.input_history[local][age] = s.input_history[local][age - 1];
        s.input_history[local][0] = input;
    }
    if (s.input_history_count < nf::net::kInputRedundancy) ++s.input_history_count;
    for (std::size_t age = 0; age < s.input_history_count; ++age)
        for (std::size_t local = 0; local < pads.size(); ++local)
            batch.samples[batch.count++] = s.input_history[local][age];
    nf::net::Packet packet;
    packet.header.message = nf::net::Message::Input;
    packet.payload = nf::net::encode_input_batch(batch);
    if (!s.socket.send(s.endpoint.host, s.endpoint.port,
                       s.reliability.prepare(std::move(packet), false, Clock::now())))
        throw std::runtime_error("failed to send input");
}

std::uint8_t NetworkSession::slot() const { return impl_->slot; }
std::uint8_t NetworkSession::local_players() const { return impl_->local_player_count; }
std::uint32_t NetworkSession::server_tick() const { return impl_->server_tick; }
std::uint32_t NetworkSession::latest_snapshot_tick() const { return impl_->latest_snapshot_tick; }
const nf::net::Snapshot* NetworkSession::latest_snapshot() const {
    return impl_->latest_snapshot ? &*impl_->latest_snapshot : nullptr;
}
bool NetworkSession::connected() const { return impl_->slot != 0xff; }
std::vector<nf::net::Snapshot> NetworkSession::take_snapshots() {
    auto result = std::move(impl_->snapshots);
    impl_->snapshots.clear();
    return result;
}
std::vector<nf::net::WorldState> NetworkSession::take_world_states() {
    std::vector<nf::net::WorldState> result;
    if (impl_->world_state) {
        result.push_back(std::move(*impl_->world_state));
        impl_->world_state.reset();
    }
    return result;
}
const std::vector<nf::net::ProjectileSnapshot>& NetworkSession::projectiles() const {
    return impl_->replicated_projectiles;
}
std::uint32_t NetworkSession::projectiles_tick() const { return impl_->replicated_projectiles_tick; }
std::vector<nf::net::ReplicationEvent> NetworkSession::take_events() {
    auto result = std::move(impl_->events);
    impl_->events.clear();
    return result;
}
std::vector<std::string> NetworkSession::take_chat() {
    auto result = std::move(impl_->chats);
    impl_->chats.clear();
    return result;
}

int run_network_client(AppContext& ctx, const NetworkClientOptions& options) {
    try {
        const std::vector<nf::PadState> script = parse_press_script(options.press);
        NetworkSession client(ctx, options);
        long frames = 0;
        std::uint32_t latest_snapshot_tick = 0;
        std::uint8_t printed_slot = 0xff;
        auto next_frame = Clock::now();
        std::printf("nightfire: connecting to %s map=%s\n", options.endpoint.c_str(), options.map.c_str());
        while (options.frames < 0 || frames < options.frames) {
            client.poll();
            if (client.connected() && printed_slot == 0xff) {
                printed_slot = client.slot();
                std::printf("nightfire: connected slot=%u server-tick=%u\n",
                            unsigned(printed_slot), client.server_tick());
            }
            for (const auto& snapshot : client.take_snapshots()) {
                latest_snapshot_tick = snapshot.tick;
                std::printf("net snapshot tick=%u %s\n", snapshot.tick, hash_to_string(snapshot.players).c_str());
            }
            for (const std::string& message : client.take_chat())
                std::printf("chat: %s\n", message.c_str());
            const auto now = Clock::now();
            if (now < next_frame) { std::this_thread::sleep_until(next_frame); continue; }
            if (client.connected()) {
                std::array<nf::PadState, nf::net::kMaxLocalPlayers> pads{};
                if (!script.empty()) pads[0] = script[std::size_t(frames) % script.size()];
                client.send_inputs(std::span<const nf::PadState>(pads.data(), options.local_players), latest_snapshot_tick);
                ++frames;
            }
            next_frame += std::chrono::nanoseconds(1000000000 / int(nf::World::kTickHz));
        }
        if (!client.connected()) throw std::runtime_error("connection timed out before handshake completed");
        std::printf("nightfire: network client stopped after %ld input frames; last snapshot=%u\n",
                    frames, latest_snapshot_tick);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nightfire network: %s\n", e.what());
        return 1;
    }
}

}  // namespace nf::app
