#include "PsdWriter.h"

#include "Codepage.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace psd {

namespace {

constexpr size_t kCopyBuffer = 8 * 1024 * 1024;
constexpr size_t kCompareBytes = 1024 * 1024;
constexpr uint64_t kBlockCompare = 32 * 1024;  // per layer: head and tail of its channel data

void putBE(std::vector<uint8_t>& v, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) v.push_back(uint8_t(value >> (8 * i)));
}

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The rebuilt bytes of one renamed record.
bool buildRecord(const PsdFile& src, const File& in, size_t index, const std::u16string& name,
                 std::vector<uint8_t>& out, std::wstring& error) {
    const LayerRecord& L = src.layers[index];
    std::vector<uint8_t> rec(size_t(L.end - L.start));
    if (!in.readAt(L.start, rec.data(), rec.size())) {
        error = L"Read error in layer record " + std::to_wstring(index);
        return false;
    }
    auto part = [&](uint64_t a, uint64_t b) { out.insert(out.end(), rec.begin() + (a - L.start), rec.begin() + (b - L.start)); };

    // Pascal name, padded the way the original was (normally to 4).
    const std::string legacy = encodeLegacy(name, src.codepage);
    const uint64_t oldField = L.nameEnd - L.nameStart, oldRaw = 1 + L.pascalName.size();
    const uint64_t padTo = oldField == alignUp(oldRaw, 4) ? 4 : oldField == alignUp(oldRaw, 2) ? 2 : 1;
    std::vector<uint8_t> pascalField;
    pascalField.push_back(uint8_t(legacy.size()));
    pascalField.insert(pascalField.end(), legacy.begin(), legacy.end());
    pascalField.resize(size_t(alignUp(pascalField.size(), padTo)), 0);

    const uint64_t rest = L.hasLuni ? (uint64_t(L.luniStart) - L.nameEnd) + (L.end - uint64_t(L.luniEnd)) : L.end - L.nameEnd;
    const uint64_t fixed = (L.nameStart - L.start) + rest + pascalField.size();
    const uint64_t luniBase = 12 + 4 + 2 * uint64_t(name.size());  // header + count + chars
    const uint64_t oldSize = L.end - L.start;
    // Pad the 'luni' data with 0 or 2 bytes so the record grows or shrinks by a multiple of 4.
    uint64_t luniPad = 0;
    if (int64_t(fixed + luniBase - oldSize) % 4 != 0 && int64_t(fixed + luniBase + 2 - oldSize) % 4 == 0) luniPad = 2;
    std::vector<uint8_t> luni{'8', 'B', 'I', 'M', 'l', 'u', 'n', 'i'};
    putBE(luni, 4 + 2 * uint64_t(name.size()) + luniPad, 4);
    putBE(luni, name.size(), 4);
    for (char16_t c : name) putBE(luni, c, 2);
    luni.resize(luni.size() + size_t(luniPad), 0);

    const uint64_t newSize = fixed + luni.size();
    const uint64_t oldExtra = uint64_t(rec[L.extraLenOffset - L.start]) << 24 | uint64_t(rec[L.extraLenOffset - L.start + 1]) << 16 |
                              uint64_t(rec[L.extraLenOffset - L.start + 2]) << 8 | rec[L.extraLenOffset - L.start + 3];
    const int64_t newExtra = int64_t(oldExtra) + int64_t(newSize) - int64_t(oldSize);
    if (newExtra < 0 || newExtra > 0xFFFFFFFFll) {
        error = L"Layer record too large after rename";
        return false;
    }

    out.clear();
    out.reserve(size_t(newSize));
    part(L.start, L.extraLenOffset);              // rect, channels, blend mode, flags
    putBE(out, uint64_t(newExtra), 4);            // extra data length
    part(L.extraLenOffset + 4, L.nameStart);      // mask data, blending ranges
    out.insert(out.end(), pascalField.begin(), pascalField.end());
    if (L.hasLuni) {  // replace in place
        part(L.nameEnd, uint64_t(L.luniStart));
        out.insert(out.end(), luni.begin(), luni.end());
        part(uint64_t(L.luniEnd), L.end);
    } else {  // insert right after the name, where every reader will find it
        out.insert(out.end(), luni.begin(), luni.end());
        part(L.nameEnd, L.end);
    }
    return true;
}

// Sequential writer copying ranges of the source; contiguous ranges are merged into one copy.
class Copier {
public:
    Copier(const File& in, File& out) : in_(in), out_(out), buf_(new uint8_t[kCopyBuffer]) {}
    bool copy(uint64_t a, uint64_t b) {
        if (a >= b) return true;
        if (pendingEnd_ == a && pendingStart_ != pendingEnd_) {
            pendingEnd_ = b;
            return true;
        }
        if (!flush()) return false;
        pendingStart_ = a;
        pendingEnd_ = b;
        return true;
    }
    bool write(const std::vector<uint8_t>& v) {
        return flush() && (out_.write(v.data(), v.size()) || fail(L"Write error: " + win32Message(GetLastError())));
    }
    bool writeLen(uint64_t value, int bytes) {
        std::vector<uint8_t> v;
        putBE(v, value, bytes);
        return write(v);
    }
    bool flush() {
        for (uint64_t a = pendingStart_; a < pendingEnd_;) {
            const size_t n = size_t(std::min<uint64_t>(pendingEnd_ - a, kCopyBuffer));
            if (!in_.readAt(a, buf_.get(), n)) return fail(L"Read error at byte " + std::to_wstring(a));
            if (!out_.write(buf_.get(), n)) return fail(L"Write error: " + win32Message(GetLastError()));
            a += n;
        }
        pendingStart_ = pendingEnd_ = 0;
        return true;
    }
    std::wstring error;

private:
    bool fail(const std::wstring& e) {
        error = e;
        return false;
    }
    const File& in_;
    File& out_;
    std::unique_ptr<uint8_t[]> buf_;
    uint64_t pendingStart_ = 0, pendingEnd_ = 0;
};

struct Patch {
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
};

bool validOrder(const std::vector<size_t>& order, size_t n) {
    if (order.size() != n) return false;
    std::vector<bool> seen(n);
    for (size_t i : order) {
        if (i >= n || seen[i]) return false;
        seen[i] = true;
    }
    return true;
}

// Image resources that refer to layers by position, rewritten (same size) for the new order.
bool resourcePatches(const PsdFile& src, const File& in, const std::vector<size_t>& order, std::vector<Patch>& out,
                     std::wstring& error) {
    out.clear();
    if (order.empty()) return true;
    const size_t n = src.layers.size();
    std::vector<size_t> newIndex(n);
    for (size_t j = 0; j < n; ++j) newIndex[order[j]] = j;
    auto read = [&](const ImageResource& r, std::vector<uint8_t>& v) {
        v.resize(r.size);
        if (in.readAt(r.offset, v.data(), v.size())) return true;
        error = L"Read error in image resources";
        return false;
    };
    std::vector<uint8_t> v;
    if (const ImageResource* r = src.resource(1024); r && r->size == 2) {  // target layer index
        if (!read(*r, v)) return false;
        const size_t old = size_t(v[0] << 8 | v[1]);
        if (old < n) out.push_back({r->offset, {uint8_t(newIndex[old] >> 8), uint8_t(newIndex[old])}});
    }
    struct PerLayer {
        uint16_t id;
        size_t entry;  // bytes per layer
    };
    for (const auto& [id, entry] : {PerLayer{1026, 2}, PerLayer{1072, 1}}) {
        const ImageResource* r = src.resource(id);
        if (!r || r->size != n * entry) continue;
        if (!read(*r, v)) return false;
        Patch p{r->offset, std::vector<uint8_t>(v.size())};
        for (size_t j = 0; j < n; ++j) std::memcpy(p.bytes.data() + j * entry, v.data() + order[j] * entry, entry);
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Patch& a, const Patch& b) { return a.offset < b.offset; });
    return true;
}

bool sameBytes(const File& a, uint64_t offA, const File& b, uint64_t offB, uint64_t n) {
    std::vector<uint8_t> x(static_cast<size_t>(n)), y(static_cast<size_t>(n));
    return a.readAt(offA, x.data(), x.size()) && b.readAt(offB, y.data(), y.size()) && x == y;
}


}  // namespace

bool writeEdited(const PsdFile& src, const Edits& edits, const std::wstring& outPath, int64_t& delta,
                 std::wstring& error) {
    delta = 0;
    const size_t n = src.layers.size();
    if (!edits.order.empty() && !validOrder(edits.order, n)) {
        error = L"Invalid layer order";
        return false;
    }
    File in;
    if (!in.openRead(src.path, error)) return false;
    FileStamp now;
    if (!getFileStamp(src.path, now) || now != src.stamp || in.size() != src.fileSize) {
        error = L"The file was changed on disk after it was opened; reopen it before saving.";
        return false;
    }

    std::map<size_t, std::vector<uint8_t>> rebuilt;
    for (const auto& [index, name] : edits.renames) {
        if (index >= n) {
            error = L"Invalid layer index";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!buildRecord(src, in, index, name, bytes, error)) return false;
        const LayerRecord& L = src.layers[index];
        delta += int64_t(bytes.size()) - int64_t(L.end - L.start);
        rebuilt.emplace(index, std::move(bytes));
    }
    std::vector<Patch> patches;
    if (!resourcePatches(src, in, edits.order, patches, error)) return false;

    File out;
    if (!out.create(outPath, error)) return false;
    Copier c(in, out);
    bool ok = true;
    // Header, colour mode data, image resources (with the patched ones).
    uint64_t pos = 0;
    for (const Patch& p : patches) {
        ok = ok && c.copy(pos, p.offset) && c.write(p.bytes);
        pos = p.offset + p.bytes.size();
    }
    if (!src.hasLayerInfo || (rebuilt.empty() && edits.order.empty())) {
        ok = ok && c.copy(pos, src.fileSize);
    } else {
        const int lenSize = src.lenSize();
        const int containerLenSize = src.layersInTaggedBlock ? (src.psb ? 8 : 4) : lenSize;  // Lr16/Lr32 are wide keys
        const uint64_t maxLen = src.psb ? UINT64_MAX : 0xFFFFFFFFull;
        if (int64_t(src.lmLen) + delta < 0 || uint64_t(int64_t(src.lmLen) + delta) > maxLen) {
            error = L"Layer section too large after rename";
            return false;
        }
        ok = ok && c.copy(pos, src.lmLenOffset) && c.writeLen(uint64_t(int64_t(src.lmLen) + delta), lenSize) &&
             c.copy(src.lmLenOffset + lenSize, src.containerLenOffset) &&
             c.writeLen(uint64_t(int64_t(src.containerLen) + delta), containerLenSize) &&
             c.copy(src.containerLenOffset + containerLenSize, src.recordsStart);
        auto original = [&](size_t j) { return edits.order.empty() ? j : edits.order[j]; };
        for (size_t j = 0; j < n && ok; ++j) {  // records, in the new order
            const size_t i = original(j);
            const auto it = rebuilt.find(i);
            ok = it != rebuilt.end() ? c.write(it->second) : c.copy(src.layers[i].start, src.layers[i].end);
        }
        for (size_t j = 0; j < n && ok; ++j)  // their channel data, in the same order
            ok = c.copy(src.layers[original(j)].dataStart, src.layers[original(j)].dataEnd);
        ok = ok && c.copy(src.channelDataEnd, src.fileSize);  // global layer info, composite
    }
    ok = ok && c.flush();
    if (ok && !out.flush()) {
        c.error = L"Flush failed: " + win32Message(GetLastError());
        ok = false;
    }
    if (!ok) error = c.error;
    return ok;
}

bool verifyEdited(const PsdFile& src, const Edits& edits, const std::wstring& outPath, int64_t delta,
                  std::wstring& error) {
    PsdFile out;
    std::wstring parseError;
    if (!out.open(outPath, parseError)) {
        error = L"Verification failed: the written file doesn't parse: " + parseError;
        return false;
    }
    auto bad = [&](const std::wstring& what) {
        error = L"Verification failed: " + what;
        return false;
    };
    if (out.fileSize != uint64_t(int64_t(src.fileSize) + delta)) return bad(L"file size");
    if (out.psb != src.psb || out.width != src.width || out.height != src.height || out.depth != src.depth ||
        out.mode != src.mode || out.channels != src.channels)
        return bad(L"header");
    if (out.layers.size() != src.layers.size() || out.layerCountRaw != src.layerCountRaw) return bad(L"layer count");
    if (out.imageDataOffset != uint64_t(int64_t(src.imageDataOffset) + delta)) return bad(L"composite offset");
    if (out.layersInTaggedBlock != src.layersInTaggedBlock) return bad(L"layer container");

    File fa, fb;
    if (!fa.openRead(src.path, error) || !fb.openRead(outPath, error)) return false;
    for (size_t j = 0; j < src.layers.size(); ++j) {
        const size_t i = edits.order.empty() ? j : edits.order[j];
        const LayerRecord &a = src.layers[i], &b = out.layers[j];
        const std::wstring which = L"layer " + std::to_wstring(i);
        if (a.top != b.top || a.left != b.left || a.bottom != b.bottom || a.right != b.right ||
            a.opacity != b.opacity || a.clipping != b.clipping || a.flags != b.flags || a.kind != b.kind ||
            std::memcmp(a.blendKey, b.blendKey, 4) != 0 || a.channels.size() != b.channels.size())
            return bad(which + L" record");
        for (size_t k = 0; k < a.channels.size(); ++k)
            if (a.channels[k].id != b.channels[k].id || a.channels[k].length != b.channels[k].length)
                return bad(which + L" channel table");
        const auto it = edits.renames.find(i);
        const std::u16string expected = it != edits.renames.end() ? it->second : src.displayName(i);
        if (out.displayName(j) != expected) return bad(which + L" name");
        if (it == edits.renames.end() && (b.end - b.start) != (a.end - a.start)) return bad(which + L" record size");
        // Its channel data followed it: compare the head and tail of the block.
        const uint64_t len = a.dataEnd - a.dataStart, head = std::min(len, kBlockCompare);
        if (b.dataEnd - b.dataStart != len || !sameBytes(fa, a.dataStart, fb, b.dataStart, head) ||
            !sameBytes(fa, a.dataEnd - head, fb, b.dataEnd - head, head))
            return bad(which + L" channel data");
    }
    // Everything after the channel data, and before the layer section (resources patched).
    const uint64_t tail = std::min<uint64_t>(kCompareBytes, src.fileSize - src.imageDataOffset);
    if (!sameBytes(fa, src.fileSize - tail, fb, out.fileSize - tail, tail)) return bad(L"composite data");
    std::vector<uint8_t> expect(size_t(src.lmLenOffset)), got(size_t(src.lmLenOffset));
    std::vector<Patch> patches;
    if (!fa.readAt(0, expect.data(), expect.size()) || !fb.readAt(0, got.data(), got.size()) ||
        !resourcePatches(src, fa, edits.order, patches, error))
        return bad(L"read error");
    for (const Patch& p : patches) std::memcpy(expect.data() + p.offset, p.bytes.data(), p.bytes.size());
    if (expect != got) return bad(L"header and resources");
    return true;
}

bool saveEdited(const PsdFile& src, const Edits& edits, const std::wstring& target, std::wstring& error) {
    std::wstring tmp = target + L".saving~";
    int64_t delta = 0;
    if (!writeEdited(src, edits, tmp, delta, error) || !verifyEdited(src, edits, tmp, delta, error)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    const bool exists = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    const BOOL moved = exists ? ReplaceFileW(target.c_str(), tmp.c_str(), nullptr,
                                             REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)
                              : MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!moved) {
        error = L"Cannot replace " + target + L": " + win32Message(GetLastError()) + L" (the new data is in " + tmp + L")";
        return false;
    }
    return true;
}

}  // namespace psd
