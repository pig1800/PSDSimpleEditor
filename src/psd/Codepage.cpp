#include "Codepage.h"

#include "PsdFile.h"

#include <algorithm>

namespace psd {

namespace {

bool isAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x80; });
}

// Code pages whose conversion functions reject the best-fit / default-char arguments.
bool plainFlagsOnly(int cp) { return cp == CP_UTF8 || cp == 54936 || (cp >= 50220 && cp <= 50229) || cp == 42; }

bool decodeStrict(const std::string& bytes, int cp, std::u16string& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    const int n = MultiByteToWideChar(UINT(cp), MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(size_t(n));
    MultiByteToWideChar(UINT(cp), MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()),
                        reinterpret_cast<wchar_t*>(out.data()), n);
    return true;
}

}  // namespace

int guessCodepage(const std::vector<LayerRecord>& layers) {
    std::vector<int> unique;
    for (int cp : {int(GetACP()), int(CP_UTF8), 936, 932, 950, 949, 1252, 10000})
        if (std::find(unique.begin(), unique.end(), cp) == unique.end() && IsValidCodePage(UINT(cp)))
            unique.push_back(cp);

    // 1. Names that have a Unicode twin: the code page that decodes them to it.
    int best = int(GetACP()), bestScore = 0;
    for (int cp : unique) {
        int score = 0;
        for (const LayerRecord& L : layers) {
            if (!L.hasLuni || isAscii(L.pascalName)) continue;
            std::u16string d;
            if (!decodeStrict(L.pascalName, cp, d)) continue;
            // A name cut at 255 bytes may decode to a prefix of the Unicode name.
            if (d == L.unicodeName || (L.pascalName.size() >= 250 && L.unicodeName.starts_with(d))) ++score;
        }
        if (score > bestScore) {
            best = cp;
            bestScore = score;
        }
    }
    if (bestScore > 0) return best;

    // 2. No reference (no 'luni'): like Notepad++, UTF-8 if every name is valid UTF-8 (multi-byte
    //    text in another encoding almost never is), else the code page that decodes the most
    //    names without errors, the system code page winning ties.
    std::vector<const std::string*> names;
    for (const LayerRecord& L : layers)
        if (!isAscii(L.pascalName)) names.push_back(&L.pascalName);
    if (names.empty()) return int(GetACP());
    std::u16string d;
    if (std::all_of(names.begin(), names.end(), [&](const std::string* n) { return decodeStrict(*n, CP_UTF8, d); }))
        return CP_UTF8;
    best = int(GetACP());
    bestScore = -1;
    for (int cp : unique) {
        const int score = int(std::count_if(names.begin(), names.end(), [&](const std::string* n) { return decodeStrict(*n, cp, d); }));
        if (score > bestScore) {
            best = cp;
            bestScore = score;
        }
    }
    return best;
}

std::u16string decodeLegacy(const std::string& bytes, int cp) {
    if (bytes.empty()) return {};
    const int n = MultiByteToWideChar(UINT(cp), 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0) return std::u16string(bytes.begin(), bytes.end());
    std::u16string out(size_t(n), u'\0');
    MultiByteToWideChar(UINT(cp), 0, bytes.data(), int(bytes.size()), reinterpret_cast<wchar_t*>(out.data()), n);
    return out;
}

std::string encodeLegacy(const std::u16string& name, int cp) {
    std::string out;
    const bool plain = plainFlagsOnly(cp);
    for (size_t i = 0; i < name.size();) {
        size_t len = 1;
        if (name[i] >= 0xD800 && name[i] <= 0xDBFF && i + 1 < name.size() && name[i + 1] >= 0xDC00 &&
            name[i + 1] <= 0xDFFF)
            len = 2;
        char piece[16];
        int n = WideCharToMultiByte(UINT(cp), plain ? 0 : WC_NO_BEST_FIT_CHARS,
                                    reinterpret_cast<const wchar_t*>(name.data() + i), int(len), piece,
                                    int(sizeof piece), plain ? nullptr : "?", nullptr);
        if (n <= 0 || (len == 2 && n == 2 && piece[0] == '?' && piece[1] == '?')) {  // one character, one '?'
            piece[0] = '?';
            n = 1;
        }
        if (out.size() + size_t(n) > 255) break;
        out.append(piece, size_t(n));
        i += len;
    }
    return out;
}

}  // namespace psd
