#include "app/server_browser.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
namespace nf::app {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array<std::uint8_t, 4> kMasterMagic{'N', 'F', 'M', 'R'};
std::uint16_t get16(const std::vector<std::uint8_t>& bytes, std::size_t& at) {
    const std::uint16_t value = std::uint16_t(bytes[at]) | (std::uint16_t(bytes[at + 1]) << 8);
    at += 2;
    return value;
}
struct Endpoint { std::string host; std::uint16_t port = 27500; };
Endpoint parse_endpoint(const std::string& text, std::uint16_t default_port) {
    const std::size_t colon = text.rfind(':');
    Endpoint result{text.substr(0, colon), default_port};
    if (colon != std::string::npos) {
        const std::string port_text = text.substr(colon + 1);
        char* end = nullptr;
        const long value = std::strtol(port_text.c_str(), &end, 10);
        if (port_text.empty() || !end || *end || value < 1 || value > 65535)
            throw std::runtime_error("invalid server endpoint port");
        result.port = std::uint16_t(value);
    }
    if (result.host.empty()) throw std::runtime_error("server endpoint requires an IPv4 address");
    return result;
}
nf::net::Packet info_query(std::uint32_t id) {
    nf::net::Packet packet;
    packet.header.message = nf::net::Message::ServerQuery;
    packet.payload = nf::net::encode_server_query(id);
    return packet;
}
void append_info(nf::net::UdpSocket& socket, const std::string& endpoint, std::uint32_t id,
                 Clock::time_point sent, std::vector<ServerBrowserEntry>& entries,
                 const std::unordered_map<std::uint32_t, std::string>& endpoints) {
    for (const auto& incoming : socket.receive()) {
        if (incoming.packet.header.message != nf::net::Message::ServerInfo) continue;
        nf::net::ServerInfo info;
        if (!nf::net::decode_server_info(incoming.packet.payload, info) || info.query_id != id) continue;
        const std::string address = endpoint.empty() ? incoming.address + ":" + std::to_string(incoming.port) : endpoint;
        if (!endpoint.empty()) {
            const auto it = endpoints.find(info.query_id);
            if (it == endpoints.end() || it->second != address || incoming.address != parse_endpoint(address, 27500).host ||
                incoming.port != parse_endpoint(address, 27500).port)
                continue;
        }
        const std::uint32_t ping = std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count());
        const auto existing = std::find_if(entries.begin(), entries.end(), [&](const ServerBrowserEntry& e) { return e.endpoint == address; });
        ServerBrowserEntry entry{address, std::move(info.name), std::move(info.map), info.mode, info.players,
                                 info.max_players, ping, info.password_required, info.match_revision, info.bots,
                                 info.slot_count, info.modified_rules};
        if (existing == entries.end()) entries.push_back(std::move(entry));
        else *existing = std::move(entry);
    }
}
}

ServerBrowser::ServerBrowser() {
    if (!socket_.bind(0, &error_)) throw std::runtime_error("server browser socket: " + error_);
}

std::vector<ServerBrowserEntry> ServerBrowser::lan(std::uint16_t game_port, std::uint32_t timeout_ms) {
    if (!socket_.enable_broadcast(&error_)) throw std::runtime_error("LAN discovery broadcast: " + error_);
    const std::uint32_t id = next_query_id_++;
    const auto sent = Clock::now();
    if (!socket_.send("255.255.255.255", game_port, info_query(id)))
        throw std::runtime_error("LAN discovery query send failed");
    std::vector<ServerBrowserEntry> entries;
    const auto deadline = sent + std::chrono::milliseconds(std::min<std::uint32_t>(timeout_ms, 5000));
    while (Clock::now() < deadline) {
        append_info(socket_, {}, id, sent, entries, {});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return entries;
}

std::vector<ServerBrowserEntry> ServerBrowser::master(const std::string& host, std::uint16_t port,
                                                       std::uint32_t timeout_ms) {
    nf::net::Packet request;
    request.payload.assign(kMasterMagic.begin(), kMasterMagic.end());
    request.payload.push_back(1);
    request.payload.push_back(3);
    if (!socket_.send(host, port, request)) throw std::runtime_error("master list query send failed");
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::min<std::uint32_t>(timeout_ms, 5000));
    std::vector<std::pair<Endpoint, std::string>> advertised;
    while (Clock::now() < deadline && advertised.empty()) {
        for (const auto& incoming : socket_.receive()) {
            const auto& bytes = incoming.packet.payload;
            if (incoming.address != host || incoming.port != port || bytes.size() < 7 ||
                !std::equal(kMasterMagic.begin(), kMasterMagic.end(), bytes.begin()) || bytes[4] != 1 || bytes[5] != 4)
                continue;
            std::size_t at = 7;
            const std::size_t count = bytes[6];
            for (std::size_t i = 0; i < count; ++i) {
                if (at >= bytes.size()) break;
                const std::size_t host_size = bytes[at++];
                if (!host_size || at + host_size + 3 > bytes.size()) break;
                Endpoint endpoint;
                endpoint.host.assign(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + host_size));
                at += host_size;
                endpoint.port = get16(bytes, at);
                const std::size_t name_size = bytes[at++];
                if (at + name_size > bytes.size()) break;
                std::string name(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + name_size));
                at += name_size;
                advertised.emplace_back(std::move(endpoint), std::move(name));
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
        }
    if (advertised.empty()) return {};
    std::unordered_map<std::uint32_t, std::string> endpoints;
    std::unordered_map<std::uint32_t, Clock::time_point> sent_at;
    for (const auto& [endpoint, name] : advertised) {
        (void)name;
        const std::uint32_t id = next_query_id_++;
        const std::string address = endpoint.host + ":" + std::to_string(endpoint.port);
        endpoints.emplace(id, address);
        sent_at.emplace(id, Clock::now());
        socket_.send(endpoint.host, endpoint.port, info_query(id));
    }
    std::vector<ServerBrowserEntry> entries;
    const auto info_deadline = Clock::now() + std::chrono::milliseconds(std::min<std::uint32_t>(timeout_ms, 5000));
    while (Clock::now() < info_deadline) {
        for (const auto& incoming : socket_.receive()) {
            if (incoming.packet.header.message != nf::net::Message::ServerInfo) continue;
            nf::net::ServerInfo info;
            if (!nf::net::decode_server_info(incoming.packet.payload, info)) continue;
            const auto endpoint = endpoints.find(info.query_id);
            const auto sent = sent_at.find(info.query_id);
            if (endpoint == endpoints.end() || sent == sent_at.end()) continue;
            const Endpoint expected = parse_endpoint(endpoint->second, 27500);
            if (incoming.address != expected.host || incoming.port != expected.port) continue;
            const auto ping = std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent->second).count());
            entries.push_back({endpoint->second, std::move(info.name), std::move(info.map), info.mode, info.players,
                               info.max_players, ping, info.password_required, info.match_revision, info.bots});
            endpoints.erase(endpoint);
            sent_at.erase(sent);
        }
        if (endpoints.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return entries;
}

std::vector<ServerBrowserEntry> ServerBrowser::direct(const std::string& text, std::uint32_t timeout_ms) {
    const Endpoint endpoint = parse_endpoint(text, 27500);
    const std::string address = endpoint.host + ":" + std::to_string(endpoint.port);
    const std::uint32_t id = next_query_id_++;
    const auto sent = Clock::now();
    if (!socket_.send(endpoint.host, endpoint.port, info_query(id))) throw std::runtime_error("server query send failed");
    const auto deadline = sent + std::chrono::milliseconds(std::min<std::uint32_t>(timeout_ms, 5000));
    std::vector<ServerBrowserEntry> entries;
    const std::unordered_map<std::uint32_t, std::string> endpoints{{id, address}};
    while (Clock::now() < deadline) {
        append_info(socket_, address, id, sent, entries, endpoints);
        if (!entries.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return entries;
}

}  // namespace nf::app
