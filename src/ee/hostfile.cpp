#include "ee/hostfile.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "ee/machine.hpp"

namespace nf::ee {

namespace {

// newlib/Sony flag bits we accept for reading.
bool wants_write(int flags) {
    const int acc = flags & 3;
    if (acc == 1 || acc == 2) return true;      // O_WRONLY / O_RDWR
    if (flags & 0x40) return true;              // O_CREAT (either ABI)
    if (flags & 0x200) return true;             // O_CREAT (newlib)
    if (flags & 0x2000) return true;            // Sony SCE_CREAT-ish
    return false;
}

[[noreturn]] void trap_ro(Cpu& c, const std::string& what) {
    throw Trap(TrapKind::Other, "read-only host FS: " + what, c.cur_pc, c.cur_inst, 0);
}

}  // namespace

std::string HostFs::resolve(const std::string& ee_path) const {
    std::string p = ee_path;
    if (const auto c = p.find(':'); c != std::string::npos) p = p.substr(c + 1);
    if (p.size() > 2 && p.substr(p.size() - 2) == ";1") p.resize(p.size() - 2);
    while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
    for (char& ch : p)
        if (ch == '\\') ch = '/';
    const std::string direct = root_ + "/" + p;
    if (FILE* f = std::fopen(direct.c_str(), "rb")) {
        std::fclose(f);
        return direct;
    }
    // Bare filename fallback: some code passes section names without dirs.
    if (const auto s = p.find_last_of('/'); s != std::string::npos) {
        for (const char* sub : {"", "DRIVING/", "PS2/"}) {
            const std::string cand = root_ + "/" + sub + p.substr(s + 1);
            if (FILE* f = std::fopen(cand.c_str(), "rb")) {
                std::fclose(f);
                return cand;
            }
        }
    }
    return "";
}

int HostFs::open_host(const std::string& ee_path, int flags, bool& write_mode) {
    write_mode = wants_write(flags);
    const std::string host = resolve(ee_path);
    std::fprintf(stderr, "hostfs: open \"%s\" flags=0x%x -> %s\n", ee_path.c_str(), flags,
                 host.empty() ? "<missing>" : host.c_str());
    if (host.empty()) return -1;
    if (files_.size() > 60) return -1;
    FILE* f = std::fopen(host.c_str(), "rb");
    if (!f) return -1;
    const int fd = next_fd_++;
    files_[fd] = {f, host};
    return fd;
}

long HostFs::read_host(int fd, u32 dst, u32 n, Machine& m) {
    const auto it = files_.find(fd);
    if (it == files_.end() || !it->second.f) return -1;
    static std::vector<u8> tmp;
    tmp.resize(n);
    const size_t got = std::fread(tmp.data(), 1, n, it->second.f);
    if (got) m.mem.write_block(dst, tmp.data(), got);
    return long(got);
}

long HostFs::seek_host(int fd, long off, int whence) {
    const auto it = files_.find(fd);
    if (it == files_.end() || !it->second.f) return -1;
    if (std::fseek(it->second.f, off, whence)) return -1;
    const long pos = std::ftell(it->second.f);
    return pos < 0 ? -1 : pos;
}

int HostFs::close_host(int fd) {
    const auto it = files_.find(fd);
    if (it == files_.end()) return -1;
    if (it->second.f) std::fclose(it->second.f);
    files_.erase(it);
    return 0;
}

void HostFs::install(Machine& m) {
    auto need = [&](const char* name) { return m.symbol(name); };
    // POSIX/newlib level.
    if (need("open"))
        m.hook("open", [this](Cpu& c) {
            const u32 path = c.r[4].w[0];
            const int flags = int(c.r[5].w[0]);
            if (wants_write(flags)) trap_ro(c, "open for write: " + c.mem.read_cstr(path));
            bool wmb = false;
            const int fd = open_host(c.mem.read_cstr(path), flags, wmb);
            c.r[2].d[0] = u64(s64(s32(fd)));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("read"))
        m.hook("read", [this, &m](Cpu& c) {
            const long r = read_host(int(c.r[4].w[0]), c.r[5].w[0], c.r[6].w[0], m);
            c.r[2].d[0] = u64(s64(r));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("lseek"))
        m.hook("lseek", [this](Cpu& c) {
            const long r = seek_host(int(c.r[4].w[0]), long(s32(c.r[5].w[0])), int(c.r[6].w[0]));
            c.r[2].d[0] = u64(s64(r));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("close"))
        m.hook("close", [this](Cpu& c) {
            c.r[2].d[0] = u64(s64(close_host(int(c.r[4].w[0]))));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("write"))
        m.hook("write", [](Cpu& c) {
            const int fd = int(c.r[4].w[0]);
            const u32 n = c.r[6].w[0];
            if (fd == 1 || fd == 2) {
                std::string s(n, '?');
                c.mem.read_block(c.r[5].w[0], s.data(), n);
                std::fwrite(s.data(), 1, n, fd == 1 ? stdout : stderr);
                c.r[2].d[0] = n;
                c.r[2].d[1] = 0;
                return true;
            }
            trap_ro(c, "write to fd " + std::to_string(fd));
        });
    // Sony EE kernel level (same table).
    if (need("sceOpen"))
        m.hook("sceOpen", [this](Cpu& c) {
            const int flags = int(c.r[5].w[0]);
            if (wants_write(flags)) trap_ro(c, "sceOpen for write: " + c.mem.read_cstr(c.r[4].w[0]));
            bool wmb = false;
            const int fd = open_host(c.mem.read_cstr(c.r[4].w[0]), flags, wmb);
            c.r[2].d[0] = u64(s64(s32(fd)));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("sceRead"))
        m.hook("sceRead", [this, &m](Cpu& c) {
            const long r = read_host(int(c.r[4].w[0]), c.r[5].w[0], c.r[6].w[0], m);
            c.r[2].d[0] = u64(s64(r));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("sceLseek"))
        m.hook("sceLseek", [this](Cpu& c) {
            const long r = seek_host(int(c.r[4].w[0]), long(s32(c.r[5].w[0])), int(c.r[6].w[0]));
            c.r[2].d[0] = u64(s64(r));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("sceLseek64"))
        m.hook("sceLseek64", [this](Cpu& c) {
            const s64 off = s64(c.r[5].d[0]);  // low word carries offsets < 2 GB
            const long r = seek_host(int(c.r[4].w[0]), long(off), int(c.r[6].w[0]));
            c.r[2].d[0] = u64(s64(r));
            c.r[2].d[1] = 0;
            return true;
        });
    if (need("sceClose"))
        m.hook("sceClose", [this](Cpu& c) {
            c.r[2].d[0] = u64(s64(close_host(int(c.r[4].w[0]))));
            c.r[2].d[1] = 0;
            return true;
        });
    for (const char* s : {"sceDopen", "sceDread", "sceDclose"}) {
        if (need(s))
            m.hook(s, [s](Cpu&) {
                throw Trap(TrapKind::Unsupported, std::string(s) + " (directory scan) has no host binding");
                return true;
            });
    }
}

}  // namespace nf::ee
