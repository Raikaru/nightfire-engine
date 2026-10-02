#pragma once

// Host file layer for the EE harness: serves disc files from an extracted data
// directory through the games' own file entry points. Implemented at the
// synchronous POSIX/Sony boundary both games share:
//
//   open(path, flags) / read(fd, buf, n) / write(fd, buf, n) / lseek(fd, off, wh)
//   sceOpen(path, flag) / sceRead / sceLseek / sceLseek64 / sceClose
//
// Semantics: single fd table (fd >= 3; Sony and libc namespaces unified —
// documented simplification, the games never mix them on one descriptor),
// read-only (writes to fds 1/2 go to host stdout; opening/creating for write
// traps), missing files return -1 with a loud stderr note (some code probes
// optional files). Paths are normalized (device prefix up to ':' stripped,
// ";1" version suffix stripped, backslash to slash) and resolved under the
// fs root. Async EA FILESYS/CDVD paths are NOT covered (they need IOP); hook
// only fires where the game actually calls these symbols. See docs/ee.md.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ee/cpu.hpp"
#include "ee/types.hpp"

namespace nf::ee {

class HostFs {
public:
    explicit HostFs(std::string root) : root_(std::move(root)) {}

    // Installs hooks for whichever of the entry points exist in this ELF
    // (missing ones are skipped; dir ops get a clear not-implemented trap).
    // Must be called after Machine construction; safe to call once.
    void install(class Machine& m);

    // Resolve an EE path as the hooks would (for tests/tools).
    std::string resolve(const std::string& ee_path) const;

private:
    int open_host(const std::string& ee_path, int flags, bool& write_mode);
    long read_host(int fd, u32 dst, u32 n, class Machine& m);
    long seek_host(int fd, long off, int whence);
    int close_host(int fd);

    struct OpenFile {
        std::FILE* f = nullptr;
        std::string path;
    };
    std::string root_;
    std::map<int, OpenFile> files_;
    int next_fd_ = 3;
};

}  // namespace nf::ee
