// Console harness for the PSD core (no GUI):
//   psdcheck dump <file>                          header + layer list
//   psdcheck edit <in> <out> [--order i,j,...] [<index>=<name>...]
//       write an edited copy (renames; new record order, bottom first, as original indices) and
//       verify it ("rename" is the same command)
//   psdcheck save <file> [--order i,j,...] [<index>=<name>...]   the same, in place (temp file + atomic replace)
#include "psd/Codepage.h"
#include "psd/PsdFile.h"
#include "psd/PsdWriter.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

std::string utf8(const std::u16string& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string utf8(const std::wstring& s) { return utf8(std::u16string(s.begin(), s.end())); }

int fail(const std::wstring& e) {
    std::printf("error: %s\n", utf8(e).c_str());
    return 1;
}

const char* kindName(psd::LayerKind k) {
    switch (k) {
        case psd::LayerKind::GroupOpen: return "group(open)";
        case psd::LayerKind::GroupClosed: return "group(closed)";
        case psd::LayerKind::GroupEnd: return "group-end";
        default: return "layer";
    }
}

int dump(const std::wstring& path) {
    psd::PsdFile f;
    std::wstring e;
    if (!f.open(path, e)) return fail(e);
    std::printf("%s %ux%u ch=%u depth=%u mode=%u merged=%d codepage=%d\n", f.psb ? "PSB" : "PSD", f.width, f.height,
                f.channels, f.depth, unsigned(f.mode), f.hasRealMergedData, f.codepage);
    std::printf("layers=%d (raw %d) in %s, records [%llu, %llu), composite at %llu, file %llu bytes\n",
                int(f.layers.size()), f.layerCountRaw, f.layersInTaggedBlock ? "Lr16/Lr32" : "layer info",
                (unsigned long long)f.recordsStart, (unsigned long long)f.recordsEnd,
                (unsigned long long)f.imageDataOffset, (unsigned long long)f.fileSize);
    for (size_t i = 0; i < f.layers.size(); ++i) {
        const psd::LayerRecord& L = f.layers[i];
        std::printf("%4zu %-13s %s%s rect(%d,%d,%d,%d) ch=%zu luni=%d \"%s\"\n", i, kindName(L.kind),
                    L.hidden() ? "hidden " : "       ", L.clipped() ? "clip" : "    ", L.left, L.top, L.right,
                    L.bottom, L.channels.size(), L.hasLuni ? 1 : 0, utf8(f.displayName(i)).c_str());
    }
    return 0;
}

bool parseEdits(int argc, wchar_t** argv, int first, psd::Edits& edits) {
    for (int i = first; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--order" && i + 1 < argc) {
            const std::wstring list = argv[++i];
            for (size_t pos = 0; pos <= list.size();) {
                const size_t comma = std::min(list.find(L',', pos), list.size());
                edits.order.push_back(size_t(std::stoul(list.substr(pos, comma - pos))));
                pos = comma + 1;
            }
            continue;
        }
        const size_t eq = a.find(L'=');
        if (eq == std::wstring::npos) return false;
        edits.renames[size_t(std::stoul(a.substr(0, eq)))] = std::u16string(a.begin() + eq + 1, a.end());
    }
    return !edits.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"dump" && argc == 3) return dump(argv[2]);
    if ((cmd == L"edit" || cmd == L"rename") && argc >= 5) {
        psd::PsdFile f;
        std::wstring e;
        psd::Edits edits;
        if (!parseEdits(argc, argv, 4, edits)) return fail(L"bad edit arguments");
        if (!f.open(argv[2], e)) return fail(e);
        int64_t delta = 0;
        if (!psd::writeEdited(f, edits, argv[3], delta, e)) return fail(e);
        if (!psd::verifyEdited(f, edits, argv[3], delta, e)) return fail(e);
        std::printf("ok, size delta %lld\n", (long long)delta);
        return 0;
    }
    if (cmd == L"save" && argc >= 4) {
        psd::PsdFile f;
        std::wstring e;
        psd::Edits edits;
        if (!parseEdits(argc, argv, 3, edits)) return fail(L"bad edit arguments");
        if (!f.open(argv[2], e)) return fail(e);
        if (!psd::saveEdited(f, edits, argv[2], e)) return fail(e);
        std::printf("ok\n");
        return 0;
    }
    std::printf("usage: psdcheck dump <file> | edit <in> <out> [--order i,j,...] [<i>=<name>...] | save <file> [--order i,j,...] [<i>=<name>...]\n");
    return 2;
}
