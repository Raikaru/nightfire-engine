#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/arena_session.hpp"
#include "net/net.hpp"

namespace nf::net {

struct MatchConfig {
    std::string map;
    MatchOptions match;
};

struct ServerConfig {
    std::filesystem::path data_dir;
    std::string map = "07000024.bin";
    MatchOptions match;
    std::uint16_t port = 27500;
    std::string name = "Nightfire";
    std::string password;
    std::string master_host;
    std::uint16_t master_port = 27501;
    bool visibility_culling = true;
    NetSim net_sim{};
    std::vector<MatchConfig> rotation;
};

struct ServerStatus {
    bool running = false;
    bool match_over = false;
    std::size_t match_index = 0;
    std::string map;
    std::uint32_t mode = 0;
    std::uint64_t revision = 0;
};

// Owns the authoritative game simulation and UDP endpoint on a worker thread. start() is repeatable after stop();
// destruction always stops and joins the worker. Each match transition resets peer state and requires clients to
// rejoin; map changes additionally change the handshake hash.
class ServerRuntime {
public:
    explicit ServerRuntime(ServerConfig config);
    ~ServerRuntime();
    ServerRuntime(ServerRuntime&&) noexcept;
    ServerRuntime& operator=(ServerRuntime&&) noexcept;
    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool running() const;
    std::string error() const;
    ServerStatus current_match() const;
    bool enqueue_match(MatchConfig match);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf::net
