// Thin Win32 file wrappers: positional reads (thread-safe on one handle), sequential writes, and
// a buffered big-endian reader for parsing.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace psd {

// Size + last-write time, used to detect that a file changed behind our back.
struct FileStamp {
    uint64_t size = 0;
    uint64_t writeTime = 0;
    bool operator==(const FileStamp&) const = default;
};

bool getFileStamp(const std::wstring& path, FileStamp& out);
std::wstring win32Message(DWORD code);

class File {
public:
    File() = default;
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    File& operator=(File&& o) noexcept;

    // Read-only, sharing read/write/delete so we never block other applications.
    bool openRead(const std::wstring& path, std::wstring& error);
    // Write-only, creates or truncates.
    bool create(const std::wstring& path, std::wstring& error);
    void close();
    bool isOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    uint64_t size() const;
    // Reads exactly n bytes at `offset`; false on error or end of file.
    bool readAt(uint64_t offset, void* dst, size_t n) const;
    // Appends at the current position (sequential writer).
    bool write(const void* src, size_t n);
    bool flush() { return FlushFileBuffers(h_) != 0; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Buffered big-endian reader over File::readAt. Failure (read error, end of file) is sticky:
// after it, every read returns 0 and ok() is false.
class Reader {
public:
    explicit Reader(const File& f, uint64_t pos = 0) : f_(f), pos_(pos) {}

    uint64_t tell() const { return pos_; }
    void seek(uint64_t pos) { pos_ = pos; }
    void skip(uint64_t n) { pos_ += n; }
    bool ok() const { return ok_; }
    void fail() { ok_ = false; }

    uint8_t u8();
    uint16_t u16();
    int16_t i16() { return int16_t(u16()); }
    uint32_t u32();
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64();
    uint64_t len(bool wide) { return wide ? u64() : u32(); }
    bool bytes(void* dst, size_t n);
    // 4 bytes at an absolute position, without moving; false if unreadable.
    bool peek4(uint64_t pos, char out[4]);

private:
    bool fill(uint64_t pos, size_t n);  // makes [pos, pos + n) available in buf_

    const File& f_;
    std::vector<uint8_t> buf_;
    uint64_t bufPos_ = 0;
    size_t bufLen_ = 0;
    uint64_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace psd
