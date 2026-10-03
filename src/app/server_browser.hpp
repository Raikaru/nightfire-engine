#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/net.hpp"

namespace nf::app {

struct ServerBrowserEntry {
    std::string endpoint;
    std::string name;
    std::string map;
    std::uint32_t mode = 0;
    std::uint8_t players = 0;
    std::uint8_t max_players = 0;
    std::uint32_t ping_ms = 0;
    bool password_required = false;
    std::uint64_t match_revision = 0;
    std::uint8_t bots = 0;
    std::uint8_t slot_count = 8;
    bool modified_rules = false;
};

// Synchronous, bounded discovery used by the Online join page. Game-info data is always queried
// from the selected UDP server; master replies contain only advertised endpoint/name pairs.
class ServerBrowser {
public:
    ServerBrowser();
    std::vector<ServerBrowserEntry> lan(std::uint16_t game_port = 27500,
                                        std::uint32_t timeout_ms = 500);
    std::vector<ServerBrowserEntry> master(const std::string& host, std::uint16_t port = 27501,
                                           std::uint32_t timeout_ms = 1000);
    std::vector<ServerBrowserEntry> direct(const std::string& endpoint,
                                           std::uint32_t timeout_ms = 500);

private:
    nf::net::UdpSocket socket_;
    std::uint32_t next_query_id_ = 1;
    std::string error_;
};

}  // namespace nf::app
