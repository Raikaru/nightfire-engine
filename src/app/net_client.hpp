#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "app/app.hpp"
#include "game/input.hpp"
#include "net/net.hpp"

namespace nf::app {

struct NetworkClientOptions {
    std::string endpoint;
    std::string map = "07000024.bin";
    std::string name = "Player";
    std::string password;
    std::string press;
    std::string chat;
    long frames = -1;
    int loss_percent = 0;
    int latency_ms = 0;
    unsigned jitter_ms = 0;
    std::uint8_t local_players = 1;
    std::array<bool, nf::net::kMaxLocalPlayers> auto_aim{true, true, true, true};
};
class NetworkSession {
public:
    NetworkSession(AppContext& ctx, NetworkClientOptions options);
    ~NetworkSession();
    NetworkSession(const NetworkSession&) = delete;
    NetworkSession& operator=(const NetworkSession&) = delete;

    void poll();
    void wait_for_connection();
    void send_inputs(std::span<const nf::PadState> pads, std::uint32_t view_tick);
    std::uint8_t slot() const;
    std::uint8_t local_players() const;
    std::uint32_t server_tick() const;
    std::uint32_t latest_snapshot_tick() const;
    const nf::net::Snapshot* latest_snapshot() const;
    bool connected() const;
    std::vector<nf::net::Snapshot> take_snapshots();
    std::vector<nf::net::WorldState> take_world_states();
    const std::vector<nf::net::ProjectileSnapshot>& projectiles() const;
    std::uint32_t projectiles_tick() const;
    std::vector<nf::net::ReplicationEvent> take_events();
    std::vector<std::string> take_chat();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};


int run_network_client(AppContext& ctx, const NetworkClientOptions& options);

}  // namespace nf::app
