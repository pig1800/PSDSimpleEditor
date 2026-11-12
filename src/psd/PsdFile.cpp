#include "PsdFile.h"

#include "Codepage.h"

#include <algorithm>
#include <cstring>

namespace psd {

namespace {

bool sigIs(const char s[4], const char* lit) { return std::memcmp(s, lit, 4) == 0; }
bool isBlockSig(const char s[4]) { return sigIs(s, "8BIM") || sigIs(s, "8B64"); }
uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

std::wstring at(uint64_t offset) { return L" (at byte " + std::to_wstring(offset) + L")"; }

class Parser {
public:
    Parser(PsdFile& p, Reader& r, std::wstring& error) : p_(p), r_(r), error_(error) {}

    bool parse() {
        if (!header() || !resources() || !layerAndMask()) return false;
        p_.codepage = guessCodepage(p_.layers);
        return true;
    }

private:
    bool fail(const std::wstring& msg) {
        error_ = msg;
        return false;
    }
    bool truncated(uint64_t offset) { return fail(L"File is truncated or corrupt" + at(offset)); }

    // True if a tagged block signature starts at `pos` (and fits before `end`).
    bool blockAt(uint64_t pos, uint64_t end) {
        char s[4];
        return pos + 12 <= end && r_.peek4(pos, s) && isBlockSig(s);
    }

    bool header() {
        char sig[4] = {};
        r_.bytes(sig, 4);
        if (!r_.ok() || !sigIs(sig, "8BPS")) return fail(L"Not a PSD/PSB file");
        const uint16_t version = r_.u16();
        if (version != 1 && version != 2) return fail(L"Unknown PSD version " + std::to_wstring(version));
        p_.psb = version == 2;
        r_.skip(6);
        p_.channels = r_.u16();
        p_.height = r_.u32();
        p_.width = r_.u32();
        p_.depth = r_.u16();
        p_.mode = ColorMode(r_.u16());
        if (!r_.ok()) return truncated(0);
        const uint32_t maxDim = p_.psb ? 300000 : 30000;
        if (p_.channels < 1 || p_.channels > 56 || p_.width < 1 || p_.height < 1 || p_.width > maxDim ||
            p_.height > maxDim)
            return fail(L"Invalid PSD header");
        if (p_.depth != 1 && p_.depth != 8 && p_.depth != 16 && p_.depth != 32)
            return fail(L"Invalid bit depth " + std::to_wstring(p_.depth));
        const uint32_t colorModeLen = r_.u32();
        r_.skip(colorModeLen);
        return true;
    }

    bool resources() {
        const uint32_t len = r_.u32();
        const uint64_t start = r_.tell(), end = start + len;
        if (!r_.ok() || end > p_.fileSize) return truncated(start);
        while (r_.tell() + 12 <= end) {
            char sig[4];
            r_.bytes(sig, 4);
            if (!sigIs(sig, "8BIM") && !sigIs(sig, "MeSa") && !sigIs(sig, "AgHg") && !sigIs(sig, "PHUT") &&
                !sigIs(sig, "DCSR"))
                break;  // unknown: the rest of the section is skipped as a whole anyway
            const uint16_t id = r_.u16();
            const uint8_t nameLen = r_.u8();
            r_.skip(nameLen + ((1u + nameLen) & 1u));  // Pascal name padded to even
            const uint32_t dataLen = r_.u32();
            const uint64_t data = r_.tell();
            if (!r_.ok() || data + dataLen > end) break;
            p_.resources.push_back({id, data, dataLen});
            if (id == 0x0421 && dataLen >= 5) {  // version info: version, hasRealMergedData
                r_.u32();
                p_.hasRealMergedData = r_.u8();
            }
            r_.seek(data + dataLen + (dataLen & 1u));
        }
        r_.seek(end);
        return true;
    }

    bool layerAndMask() {
        p_.lmLenOffset = r_.tell();
        p_.lmLen = r_.len(p_.psb);
        const uint64_t lmStart = r_.tell(), lmEnd = lmStart + p_.lmLen;
        if (!r_.ok() || lmEnd > p_.fileSize) return truncated(p_.lmLenOffset);
        p_.imageDataOffset = lmEnd;
        if (p_.lmLen == 0) return true;  // flat file: no layers

        const uint64_t liLenOffset = r_.tell();
        const uint64_t liLen = r_.len(p_.psb);
        const uint64_t liStart = r_.tell();
        if (!r_.ok() || liStart + liLen > lmEnd) return truncated(liLenOffset);
        if (liLen > 0) {
            p_.containerLenOffset = liLenOffset;
            p_.containerLen = liLen;
            return records(liStart, liStart + liLen);
        }
        // 16/32-bit documents: the layer info lives in an 'Lr16'/'Lr32' block after the global
        // layer mask info.
        r_.seek(liStart + liLen);
        const uint32_t globalMaskLen = r_.u32();
        uint64_t pos = r_.tell() + globalMaskLen;
        while (r_.ok() && blockAt(pos, lmEnd)) {
            char key[4];
            r_.seek(pos + 4);
            r_.bytes(key, 4);
            const uint64_t lenOffset = r_.tell();
            const uint64_t len = r_.len(p_.psb && isWideKey(key));
            const uint64_t data = r_.tell();
            if (!r_.ok() || data + len > lmEnd) break;
            if (sigIs(key, "Lr16") || sigIs(key, "Lr32")) {
                p_.layersInTaggedBlock = true;
                p_.containerLenOffset = lenOffset;
                p_.containerLen = len;
                return records(data, data + len);
            }
            uint64_t next = data + len;
            if (next != lmEnd && !blockAt(next, lmEnd)) {  // lengths not padded the way we assumed
                for (uint64_t a : {2u, 4u}) {
                    const uint64_t n = data + alignUp(len, a);
                    if (n == lmEnd || blockAt(n, lmEnd)) {
                        next = n;
                        break;
                    }
                }
            }
            pos = next;
        }
        return true;  // no layer records
    }

    bool records(uint64_t start, uint64_t end) {
        r_.seek(start);
        p_.layerCountRaw = r_.i16();
        const int count = std::abs(int(p_.layerCountRaw));
        p_.recordsStart = r_.tell();
        p_.layers.reserve(count);
        for (int i = 0; i < count; ++i) {
            LayerRecord L;
            if (!record(L, end)) return false;
            p_.layers.push_back(std::move(L));
        }
        p_.recordsEnd = r_.tell();
        uint64_t data = p_.recordsEnd;
        for (LayerRecord& L : p_.layers) {
            L.dataStart = data;
            for (ChannelInfo& c : L.channels) {
                c.dataOffset = data;
                data += c.length;
            }
            L.dataEnd = data;
        }
        if (data > end) return fail(L"Layer channel data runs past its section (truncated or corrupt file)");
        p_.channelDataEnd = data;
        p_.hasLayerInfo = true;
        return true;
    }

    bool record(LayerRecord& L, uint64_t sectionEnd) {
        L.start = r_.tell();
        L.top = r_.i32();
        L.left = r_.i32();
        L.bottom = r_.i32();
        L.right = r_.i32();
        const uint16_t nch = r_.u16();
        if (!r_.ok()) return truncated(L.start);
        if (nch > 56) return fail(L"Corrupt layer record" + at(L.start));
        L.channels.resize(nch);
        for (ChannelInfo& c : L.channels) {
            c.id = r_.i16();
            c.length = r_.len(p_.psb);
        }
        char sig[4] = {};
        r_.bytes(sig, 4);
        r_.bytes(L.blendKey, 4);
        L.opacity = r_.u8();
        L.clipping = r_.u8();
        L.flags = r_.u8();
        r_.u8();  // filler
        if (!r_.ok()) return truncated(L.start);
        if (!sigIs(sig, "8BIM") && !sigIs(sig, "8B64")) return fail(L"Corrupt layer record" + at(L.start));

        L.extraLenOffset = r_.tell();
        const uint32_t extraLen = r_.u32();
        const uint64_t extraEnd = r_.tell() + extraLen;
        if (!r_.ok() || extraEnd > sectionEnd) return fail(L"Corrupt layer record" + at(L.start));
        const uint32_t maskLen = r_.u32();
        r_.skip(maskLen);
        const uint32_t blendRangesLen = r_.u32();
        r_.skip(blendRangesLen);
        L.nameStart = r_.tell();
        const uint8_t nameLen = r_.u8();
        L.pascalName.resize(nameLen);
        r_.bytes(L.pascalName.data(), nameLen);
        if (!r_.ok() || L.nameStart + 1 + nameLen > extraEnd) return fail(L"Corrupt layer record" + at(L.start));

        // The name is padded to a multiple of 4; tolerate writers that pad to 2 or not at all.
        const uint64_t raw = 1u + nameLen;
        L.nameEnd = 0;
        for (uint64_t cand : {alignUp(raw, 4), alignUp(raw, 2), raw}) {
            const uint64_t e = L.nameStart + cand;
            if (e == extraEnd || blockAt(e, extraEnd)) {
                L.nameEnd = e;
                break;
            }
        }
        if (L.nameEnd == 0) L.nameEnd = std::min(L.nameStart + alignUp(raw, 4), extraEnd);

        uint64_t pos = L.nameEnd;
        while (blockAt(pos, extraEnd)) {
            char key[4];
            r_.seek(pos + 4);
            r_.bytes(key, 4);
            const uint64_t len = r_.len(p_.psb && isWideKey(key));
            const uint64_t data = r_.tell();
            if (!r_.ok() || data + len > extraEnd) break;
            uint64_t next = data + len;
            if (next != extraEnd && !blockAt(next, extraEnd)) {
                for (uint64_t a : {2u, 4u}) {
                    const uint64_t n = data + alignUp(len, a);
                    if (n == extraEnd || blockAt(n, extraEnd)) {
                        next = n;
                        break;
                    }
                }
            }
            if (sigIs(key, "luni") && len >= 4) {
                const uint32_t n = r_.u32();
                const uint64_t chars = std::min<uint64_t>(n, (len - 4) / 2);
                L.unicodeName.resize(size_t(chars));
                for (char16_t& ch : L.unicodeName) ch = char16_t(r_.u16());
                while (!L.unicodeName.empty() && L.unicodeName.back() == 0) L.unicodeName.pop_back();
                L.hasLuni = true;
                L.luniStart = int64_t(pos);
                L.luniEnd = int64_t(next);
            } else if ((sigIs(key, "lsct") || sigIs(key, "lsdk")) && len >= 4) {
                switch (r_.u32()) {
                    case 1: L.kind = LayerKind::GroupOpen; break;
                    case 2: L.kind = LayerKind::GroupClosed; break;
                    case 3: L.kind = LayerKind::GroupEnd; break;
                    default: break;
                }
            }
            if (!r_.ok()) return truncated(pos);
            pos = next;
        }
        r_.seek(extraEnd);
        L.end = extraEnd;
        return true;
    }

    PsdFile& p_;
    Reader& r_;
    std::wstring& error_;
};

}  // namespace

bool isWideKey(const char key[4]) {
    static const char* const kWide[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                        "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    for (const char* k : kWide)
        if (std::memcmp(key, k, 4) == 0) return true;
    return false;
}

bool PsdFile::open(const std::wstring& p, std::wstring& error) {
    *this = PsdFile{};
    path = p;
    File f;
    if (!f.openRead(path, error)) return false;
    if (!getFileStamp(path, stamp)) {
        error = L"Cannot read file attributes of " + path;
        return false;
    }
    fileSize = f.size();
    Reader r(f);
    Parser parser(*this, r, error);
    return parser.parse();
}

std::u16string PsdFile::displayName(size_t i) const {
    const LayerRecord& L = layers[i];
    return L.hasLuni ? L.unicodeName : decodeLegacy(L.pascalName, codepage);
}

}  // namespace psd
