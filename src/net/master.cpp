#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "net/net.hpp"

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array<std::uint8_t, 4> kMagic{'N', 'F', 'M', 'R'};
constexpr std::uint8_t kVersion = 1;
enum class Type : std::uint8_t { Register = 1, Heartbeat = 2, Query = 3, List = 4, Unregister = 5 };
struct Entry {
    std::string host;
    std::uint16_t port = 0;
    std::string name;
    Clock::time_point seen{};
};
void put16(std::vector<std::uint8_t>& bytes, std::uint16_t n) {
    bytes.push_back(std::uint8_t(n)); bytes.push_back(std::uint8_t(n >> 8));
}
bool get16(const std::vector<std::uint8_t>& bytes, std::size_t& at, std::uint16_t& n) {
    if (at + 2 > bytes.size()) return false;
    n = std::uint16_t(bytes[at]) | (std::uint16_t(bytes[at + 1]) << 8);
    at += 2;
    return true;
}
std::vector<std::uint8_t> message(Type type) {
    std::vector<std::uint8_t> bytes(kMagic.begin(), kMagic.end());
    bytes.push_back(kVersion);
    bytes.push_back(std::uint8_t(type));
    return bytes;
}
bool valid(const nf::net::Received& packet, Type& type) {
    const auto& b = packet.packet.payload;
    if (b.size() < 6 || !std::equal(kMagic.begin(), kMagic.end(), b.begin()) || b[4] != kVersion || b[5] < 1 || b[5] > 5)
        return false;
    type = Type(b[5]); return true;
}
void usage() { std::puts("usage: nfmaster [--port 27501] | nfmaster --list <master:port>"); }
}

int main(int argc, char** argv) {
    int port = 27501;
    std::string list_host;
    std::uint16_t list_port = 27501;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") { usage(); return 0; }
        if (arg == "--list" && i + 1 < argc) {
            const std::string endpoint = argv[++i];
            const std::size_t colon = endpoint.rfind(':');
            list_host = endpoint.substr(0, colon);
            if (colon != std::string::npos) {
                char* end = nullptr; const long value = std::strtol(endpoint.c_str() + colon + 1, &end, 10);
                if (!end || *end || value < 1 || value > 65535) { std::fprintf(stderr, "nfmaster: invalid master port\n"); return 2; }
                list_port = std::uint16_t(value);
            }
            if (list_host.empty()) { std::fprintf(stderr, "nfmaster: --list requires a host\n"); return 2; }
            continue;
        }
        if (arg != "--port" || ++i >= argc) { usage(); return 2; }
        char* end = nullptr; const long value = std::strtol(argv[i], &end, 10);
        if (!end || *end || value < 1 || value > 65535) { std::fprintf(stderr, "nfmaster: invalid port\n"); return 2; }
        port = int(value);
    }
    nf::net::UdpSocket socket;
    std::string error;
    if (!socket.bind(std::uint16_t(list_host.empty() ? port : 0), &error)) { std::fprintf(stderr, "nfmaster: %s\n", error.c_str()); return 1; }
    if (!list_host.empty()) {
        nf::net::Packet query;
        query.payload = message(Type::Query);
        if (!socket.send(list_host, list_port, query)) { std::fprintf(stderr, "nfmaster: list query send failed\n"); return 1; }
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while (Clock::now() < deadline) {
            for (const auto& packet : socket.receive()) {
                Type type{};
                if (!valid(packet, type) || type != Type::List || packet.port != list_port) continue;
                const auto& b = packet.packet.payload;
                if (b.size() < 7) continue;
                std::size_t at = 7;
                const std::size_t count = b[6];
                std::printf("nfmaster: %zu server(s)\n", count);
                for (std::size_t i = 0; i < count; ++i) {
                    if (at >= b.size()) return 1;
                    const std::size_t host_size = b[at++];
                    if (host_size == 0 || at + host_size > b.size()) return 1;
                    const std::string host(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + host_size));
                    at += host_size;
                    std::uint16_t game_port = 0;
                    if (!get16(b, at, game_port) || at >= b.size()) return 1;
                    const std::size_t name_size = b[at++];
                    if (at + name_size > b.size()) return 1;
                    const std::string name(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + name_size));
                    at += name_size;
                    std::printf("%s:%u %s\n", host.c_str(), unsigned(game_port), name.c_str());
                }
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::fprintf(stderr, "nfmaster: list query timed out\n");
        return 1;
    }
    std::vector<Entry> entries;
    std::printf("nfmaster: UDP %d; registrations expire after 60 seconds\n", port);
    for (;;) {
        const auto now = Clock::now();
        for (const auto& packet : socket.receive()) {
            Type type{};
            if (!valid(packet, type)) continue;
            const auto& b = packet.packet.payload;
            std::size_t at = 6; std::uint16_t game_port = 0;
            if (type == Type::Register || type == Type::Heartbeat || type == Type::Unregister) {
                if (!get16(b, at, game_port) || game_port == 0) continue;
                auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.host == packet.address && e.port == game_port; });
                if (type == Type::Unregister) { if (it != entries.end()) entries.erase(it); continue; }
                if (type == Type::Register) {
                    if (at >= b.size()) continue;
                    const std::size_t size = b[at++];
                    if (!size || size > 64 || at + size != b.size()) continue;
                    const std::string name(b.begin() + std::ptrdiff_t(at), b.end());
                    if (it == entries.end()) entries.push_back({packet.address, game_port, name, now});
                    else { it->name = name; it->seen = now; }
                } else if (it != entries.end()) it->seen = now;
            } else if (type == Type::Query && b.size() == 6) {
                entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return now - e.seen > std::chrono::seconds(60); }), entries.end());
                auto reply = message(Type::List);
                reply.push_back(0);
                for (const Entry& e : entries) {
                    const std::size_t n = std::min<std::size_t>(e.name.size(), 64);
                    if (e.host.empty() || e.host.size() > 255 || reply.size() + 1 + e.host.size() + 2 + 1 + n > nf::net::kMaxDatagramBytes - 20)
                        break;
                    reply.push_back(std::uint8_t(e.host.size()));
                    reply.insert(reply.end(), e.host.begin(), e.host.end());
                    put16(reply, e.port);
                    reply.push_back(std::uint8_t(n));
                    reply.insert(reply.end(), e.name.begin(), e.name.begin() + std::ptrdiff_t(n));
                    ++reply[6];
                }
                nf::net::Packet response; response.payload = std::move(reply);
                socket.send(packet.address, packet.port, response);
            }
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return now - e.seen > std::chrono::seconds(60); }), entries.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
