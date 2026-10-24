#ifndef LSM_IO_H
#define LSM_IO_H

// Small portable filesystem helpers used to make on-disk state crash-safe:
//  - SyncFile:      force a file's data to stable storage (fsync / _commit)
//  - SyncDirectory: persist directory entries (new / renamed / removed files)
//  - AtomicReplace: rename a fully-written temp file over its final name
//
// Without these, std::ofstream::flush() only hands data to the OS page cache,
// so a power loss can still lose "durable" writes or expose half-written files.

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lsm {
namespace io {

// Flush a file's contents to stable storage. Returns false on any error.
inline bool SyncFile(const std::string& path) {
#ifdef _WIN32
    int fd = _open(path.c_str(), _O_RDWR | _O_BINARY);
    if (fd < 0) return false;
    bool ok = (_commit(fd) == 0);
    _close(fd);
    return ok;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
#endif
}

// Persist directory metadata so that file creations, renames and deletions
// survive a crash. Windows has no equivalent (NTFS journals metadata), so no-op.
inline bool SyncDirectory(const std::string& dir) {
#ifdef _WIN32
    (void)dir;
    return true;
#else
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
#endif
}

// Atomically replace `to` with `from` (both in the same directory).
inline bool AtomicReplace(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// A raw file descriptor kept open next to an std::ofstream so the stream's
// buffered-then-flushed bytes can be fsync'd without reopening the file.
class SyncHandle {
public:
    SyncHandle() : fd_(-1) {}
    ~SyncHandle() { Close(); }
    SyncHandle(const SyncHandle&) = delete;
    SyncHandle& operator=(const SyncHandle&) = delete;

    bool Open(const std::string& path) {
        Close();
#ifdef _WIN32
        fd_ = _open(path.c_str(), _O_RDWR | _O_BINARY);
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
#endif
        return fd_ >= 0;
    }

    bool Sync() const {
        if (fd_ < 0) return false;
#ifdef _WIN32
        return _commit(fd_) == 0;
#elif defined(__APPLE__)
        return ::fsync(fd_) == 0;
#else
        return ::fdatasync(fd_) == 0;
#endif
    }

    void Close() {
        if (fd_ >= 0) {
#ifdef _WIN32
            _close(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }

private:
    int fd_;
};

} // namespace io
} // namespace lsm

#endif // LSM_IO_H
