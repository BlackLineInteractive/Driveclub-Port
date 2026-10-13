// libkernel file system. Guest file descriptors are host descriptors; paths
// are translated from PS4 mount points to host directories.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

namespace fs = std::filesystem;

namespace {

fs::path g_game_root;
fs::path g_user_root;

struct Mount {
    std::string_view guest;
    fs::path host;
};

std::vector<Mount>& Mounts() {
    static std::vector<Mount> mounts;
    return mounts;
}

// Guest path -> host path, or empty if the guest path is not mounted.
fs::path Translate(const char* guest) {
    std::string_view p(guest);
    if (p.empty()) {
        return {};
    }
    std::string absolute;
    if (p.front() != '/') {
        absolute = "/app0/";
        absolute.append(p);
        p = absolute;
    }
    for (const Mount& m : Mounts()) {
        if (p.starts_with(m.guest) &&
            (p.size() == m.guest.size() || p[m.guest.size()] == '/')) {
            std::string_view rest = p.substr(m.guest.size());
            while (!rest.empty() && rest.front() == '/') {
                rest.remove_prefix(1);
            }
            return rest.empty() ? m.host : m.host / fs::path(rest);
        }
    }
    DC_WARN("unmounted guest path: %s", guest);
    return {};
}

// FreeBSD open(2) flags -> host flags. Most values coincide.
int HostOpenFlags(int f) {
    constexpr int kGuestDirect = 0x10000;
    constexpr int kGuestDirectory = 0x20000;
    int host = f & (O_ACCMODE | O_NONBLOCK | O_APPEND | O_CREAT | O_TRUNC | O_EXCL | O_NOFOLLOW);
    if (f & kGuestDirectory) {
        host |= O_DIRECTORY;
    }
    (void)kGuestDirect; // no O_DIRECT on macOS; the page cache is fine for us
    return host;
}

struct GuestTimespec {
    s64 sec;
    s64 nsec;
};

// FreeBSD amd64 struct stat, as the PS4 SDK defines it.
struct GuestStat {
    u32 st_dev;
    u32 st_ino;
    u16 st_mode;
    u16 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u32 st_rdev;
    GuestTimespec st_atim;
    GuestTimespec st_mtim;
    GuestTimespec st_ctim;
    s64 st_size;
    s64 st_blocks;
    u32 st_blksize;
    u32 st_flags;
    u32 st_gen;
    s32 st_lspare;
    GuestTimespec st_birthtim;
};
static_assert(sizeof(GuestStat) == 120);

void ToGuest(const struct stat& h, GuestStat* g) {
    std::memset(g, 0, sizeof(*g));
    g->st_dev = static_cast<u32>(h.st_dev);
    g->st_ino = static_cast<u32>(h.st_ino);
    g->st_mode = h.st_mode;
    g->st_nlink = h.st_nlink;
    // The PS4 reports files as owned by everyone and fully accessible.
    g->st_mode |= (S_ISDIR(h.st_mode) ? 0777 : 0666);
    g->st_atim = {h.st_atimespec.tv_sec, h.st_atimespec.tv_nsec};
    g->st_mtim = {h.st_mtimespec.tv_sec, h.st_mtimespec.tv_nsec};
    g->st_ctim = {h.st_ctimespec.tv_sec, h.st_ctimespec.tv_nsec};
    g->st_birthtim = {h.st_birthtimespec.tv_sec, h.st_birthtimespec.tv_nsec};
    g->st_size = h.st_size;
    g->st_blocks = h.st_blocks;
    g->st_blksize = 0x10000;
}

// sce* calls return -errno style Orbis errors; posix ones set guest errno.
s32 Sce(int r) {
    return r < 0 ? OrbisError(errno) : r;
}

s64 Sce64(s64 r) {
    return r < 0 ? OrbisError(errno) : r;
}

s32 sceKernelOpen(const char* path, int flags, u16 mode) {
    const fs::path host = Translate(path);
    if (host.empty()) {
        return OrbisError(ENOENT);
    }
    const int fd = open(host.c_str(), HostOpenFlags(flags), mode ? mode : 0644);
    if (fd < 0) {
        const int err = errno;
        if (err != ENOENT) {
            DC_WARN("open(%s) failed: %d", path, err);
        }
        return OrbisError(err);
    }
    return fd;
}

int posix_open(const char* path, int flags, u16 mode) {
    const s32 r = sceKernelOpen(path, flags, mode);
    return r < 0 ? FailErrno(r & 0xFFFF) : r;
}

std::mutex g_dir_lock;
struct DirState {
    std::vector<std::pair<std::string, u8>> entries;
    size_t next = 0;
    bool loaded = false;
};
std::unordered_map<int, DirState> g_dirs;

s32 sceKernelClose(int fd) {
    {
        std::scoped_lock lk(g_dir_lock);
        g_dirs.erase(fd);
    }
    return Sce(close(fd));
}

s64 sceKernelRead(int fd, void* buf, u64 n) { return Sce64(read(fd, buf, n)); }
s64 sceKernelWrite(int fd, const void* buf, u64 n) { return Sce64(write(fd, buf, n)); }
s64 sceKernelPread(int fd, void* buf, u64 n, s64 off) { return Sce64(pread(fd, buf, n, off)); }
s64 sceKernelPwrite(int fd, const void* buf, u64 n, s64 off) { return Sce64(pwrite(fd, buf, n, off)); }
s64 sceKernelLseek(int fd, s64 off, int whence) { return Sce64(lseek(fd, off, whence)); }
s32 sceKernelFsync(int fd) { return Sce(fsync(fd)); }
s32 sceKernelFtruncate(int fd, s64 len) { return Sce(ftruncate(fd, len)); }
s32 sceKernelFutimes(int, const void*) { return ORBIS_OK; }

s32 sceKernelFstat(int fd, GuestStat* out) {
    struct stat h;
    if (fstat(fd, &h) != 0) {
        return OrbisError(errno);
    }
    ToGuest(h, out);
    return ORBIS_OK;
}

int posix_fstat(int fd, GuestStat* out) {
    const s32 r = sceKernelFstat(fd, out);
    return r < 0 ? FailErrno(r & 0xFFFF) : 0;
}

s32 sceKernelStat(const char* path, GuestStat* out) {
    const fs::path host = Translate(path);
    struct stat h;
    if (host.empty() || stat(host.c_str(), &h) != 0) {
        return OrbisError(host.empty() ? ENOENT : errno);
    }
    ToGuest(h, out);
    return ORBIS_OK;
}

s32 PathOp(const char* path, int (*op)(const char*)) {
    const fs::path host = Translate(path);
    return host.empty() ? OrbisError(ENOENT) : Sce(op(host.c_str()));
}

s32 sceKernelMkdir(const char* path, u16 mode) {
    const fs::path host = Translate(path);
    return host.empty() ? OrbisError(ENOENT) : Sce(mkdir(host.c_str(), mode ? mode : 0755));
}

s32 sceKernelRmdir(const char* path) { return PathOp(path, rmdir); }
s32 sceKernelUnlink(const char* path) { return PathOp(path, unlink); }

s32 sceKernelTruncate(const char* path, s64 len) {
    const fs::path host = Translate(path);
    return host.empty() ? OrbisError(ENOENT) : Sce(truncate(host.c_str(), len));
}

s32 sceKernelRename(const char* from, const char* to) {
    const fs::path a = Translate(from), b = Translate(to);
    if (a.empty() || b.empty()) {
        return OrbisError(ENOENT);
    }
    return Sce(rename(a.c_str(), b.c_str()));
}

// FreeBSD (PS4) dirent: fixed 8-byte header, name padded to 4 bytes.
s64 FillDirents(int fd, u8* buf, u64 nbytes) {
    std::scoped_lock lk(g_dir_lock);
    DirState& st = g_dirs[fd];
    if (!st.loaded) {
        st.loaded = true;
        const int dup_fd = dup(fd);
        if (DIR* d = fdopendir(dup_fd)) {
            while (dirent* e = readdir(d)) {
                st.entries.emplace_back(e->d_name, e->d_type);
            }
            closedir(d);
        } else {
            close(dup_fd);
            return OrbisError(ENOTDIR);
        }
    }

    u64 written = 0;
    while (st.next < st.entries.size()) {
        const auto& [name, type] = st.entries[st.next];
        const u16 namlen = static_cast<u16>(std::min<size_t>(name.size(), 255));
        const u16 reclen = static_cast<u16>(8 + AlignUp(namlen + 1, 4));
        if (written + reclen > nbytes) {
            break;
        }
        u8* rec = buf + written;
        std::memset(rec, 0, reclen);
        const u32 fileno = static_cast<u32>(st.next + 1);
        std::memcpy(rec, &fileno, 4);
        std::memcpy(rec + 4, &reclen, 2);
        rec[6] = type;
        rec[7] = static_cast<u8>(namlen);
        std::memcpy(rec + 8, name.data(), namlen);
        written += reclen;
        ++st.next;
    }
    return static_cast<s64>(written);
}

s64 sceKernelGetdents(int fd, u8* buf, u64 nbytes) {
    return FillDirents(fd, buf, nbytes);
}

s64 sceKernelGetdirentries(int fd, u8* buf, u64 nbytes, s64* basep) {
    const s64 r = FillDirents(fd, buf, nbytes);
    if (basep) {
        *basep = 0;
    }
    return r;
}

} // namespace

void SetFileRoots(const fs::path& game_root, const fs::path& user_root) {
    g_game_root = fs::absolute(game_root);
    g_user_root = user_root;
    for (const char* dir : {"data", "download0", "temp0", "savedata"}) {
        std::error_code ec;
        fs::create_directories(g_user_root / dir, ec);
    }
    auto& m = Mounts();
    m.clear();
    m.push_back({"/app0", g_game_root});
    m.push_back({"/hostapp", g_game_root});
    m.push_back({"/data", g_user_root / "data"});
    m.push_back({"/download0", g_user_root / "download0"});
    m.push_back({"/temp0", g_user_root / "temp0"});
    m.push_back({"/temp", g_user_root / "temp0"});
    m.push_back({"/savedata0", g_user_root / "savedata"});
    DC_INFO("/app0 -> %s, user data -> %s", g_game_root.c_str(), g_user_root.c_str());
}

void RegisterFile() {
    constexpr const char* k = "libkernel";
    DC_HLE(k, sceKernelOpen);
    DC_HLE(k, sceKernelClose);
    DC_HLE(k, sceKernelRead);
    DC_HLE(k, sceKernelWrite);
    DC_HLE(k, sceKernelPread);
    DC_HLE(k, sceKernelPwrite);
    DC_HLE(k, sceKernelLseek);
    DC_HLE(k, sceKernelFsync);
    DC_HLE(k, sceKernelFtruncate);
    DC_HLE(k, sceKernelFutimes);
    DC_HLE(k, sceKernelFstat);
    DC_HLE(k, sceKernelStat);
    DC_HLE(k, sceKernelMkdir);
    DC_HLE(k, sceKernelRmdir);
    DC_HLE(k, sceKernelUnlink);
    DC_HLE(k, sceKernelTruncate);
    DC_HLE(k, sceKernelRename);
    DC_HLE(k, sceKernelGetdents);
    DC_HLE(k, sceKernelGetdirentries);
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "open", posix_open);
        DC_HLE_AS(lib, "fstat", posix_fstat);
    }
}

} // namespace Hle::Kernel
