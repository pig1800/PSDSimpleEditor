#include "FileIO.h"

#include <algorithm>
#include <cstring>

namespace psd {

namespace {
constexpr size_t kReaderBuffer = 256 * 1024;
constexpr DWORD kMaxIo = 1u << 30;  // one ReadFile/WriteFile call at most 1 GB
}  // namespace

std::wstring win32Message(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring s = n ? std::wstring(text, n) : L"error " + std::to_wstring(code);
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

bool getFileStamp(const std::wstring& path, FileStamp& out) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    out.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    out.writeTime = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_;
        o.h_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}

bool File::openRead(const std::wstring& path, std::wstring& error) {
    close();
    h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
        error = L"Cannot open " + path + L": " + win32Message(GetLastError());
        return false;
    }
    return true;
}

bool File::create(const std::wstring& path, std::wstring& error) {
    close();
    h_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
        error = L"Cannot create " + path + L": " + win32Message(GetLastError());
        return false;
    }
    return true;
}

void File::close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

uint64_t File::size() const {
    LARGE_INTEGER s{};
    return GetFileSizeEx(h_, &s) ? uint64_t(s.QuadPart) : 0;
}

bool File::readAt(uint64_t offset, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    while (n > 0) {
        const DWORD chunk = DWORD(std::min<size_t>(n, kMaxIo));
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, p, chunk, &got, &ov) || got != chunk) return false;
        p += got;
        offset += got;
        n -= got;
    }
    return true;
}

bool File::write(const void* src, size_t n) {
    auto* p = static_cast<const uint8_t*>(src);
    while (n > 0) {
        const DWORD chunk = DWORD(std::min<size_t>(n, kMaxIo));
        DWORD put = 0;
        if (!WriteFile(h_, p, chunk, &put, nullptr) || put != chunk) return false;
        p += put;
        n -= put;
    }
    return true;
}

bool Reader::fill(uint64_t pos, size_t n) {
    if (pos >= bufPos_ && pos + n <= bufPos_ + bufLen_) return true;
    const uint64_t fileSize = f_.size();
    if (pos + n > fileSize) return false;
    const size_t want = size_t(std::min<uint64_t>(std::max(n, kReaderBuffer), fileSize - pos));
    buf_.resize(want);
    if (!f_.readAt(pos, buf_.data(), want)) {
        bufLen_ = 0;
        return false;
    }
    bufPos_ = pos;
    bufLen_ = want;
    return true;
}

bool Reader::bytes(void* dst, size_t n) {
    if (!ok_) return false;
    if (n == 0) return true;
    if (n > kReaderBuffer) {  // large: straight from the file
        if (!f_.readAt(pos_, dst, n)) return ok_ = false;
        pos_ += n;
        return true;
    }
    if (!fill(pos_, n)) return ok_ = false;
    std::memcpy(dst, buf_.data() + (pos_ - bufPos_), n);
    pos_ += n;
    return true;
}

bool Reader::peek4(uint64_t pos, char out[4]) {
    if (!fill(pos, 4)) return false;
    std::memcpy(out, buf_.data() + (pos - bufPos_), 4);
    return true;
}

uint8_t Reader::u8() {
    uint8_t b = 0;
    bytes(&b, 1);
    return b;
}

uint16_t Reader::u16() {
    uint8_t b[2] = {};
    if (!bytes(b, 2)) return 0;
    return uint16_t(b[0] << 8 | b[1]);
}

uint32_t Reader::u32() {
    uint8_t b[4] = {};
    if (!bytes(b, 4)) return 0;
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}

uint64_t Reader::u64() {
    const uint64_t hi = u32();
    return hi << 32 | u32();
}

}  // namespace psd
