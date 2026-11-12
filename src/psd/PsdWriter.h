// Saving edits (renamed layers, layers moved in the tree) with minimal change. Every byte of the
// source is copied as is, except:
//   - renamed records: the Pascal name field, the 'luni' block (rewritten, or added if missing)
//     and the "extra data" length field;
//   - the length fields of the layer and mask section and of the block holding the records
//     (layer info, or 'Lr16'/'Lr32');
//   - moved layers: records and their channel data are written in the new order, each copied
//     unchanged; the image resources that list layers by position (1024 target layer, 1026 layer
//     group IDs, 1072 layer-group-enabled IDs) are permuted in place to follow them.
// A record's size change is kept a multiple of 4 (through the 'luni' padding) so every padding
// byte after it can be copied unchanged.
#pragma once

#include "PsdFile.h"

#include <map>
#include <string>
#include <vector>

namespace psd {

using RenameMap = std::map<size_t, std::u16string>;  // layer record index -> new name

struct Edits {
    RenameMap renames;
    // New file order (bottom-most first) as original record indices; empty: unchanged.
    std::vector<size_t> order;
    bool empty() const { return renames.empty() && order.empty(); }
};

// Writes `src` with `edits` applied to `outPath`. `delta` receives the change in file size.
bool writeEdited(const PsdFile& src, const Edits& edits, const std::wstring& outPath, int64_t& delta, std::wstring& error);

// Re-parses `outPath` and checks it is `src` with `edits` applied: same records in the new
// order, new names, the same channel data per record, identical leading and trailing data.
bool verifyEdited(const PsdFile& src, const Edits& edits, const std::wstring& outPath, int64_t delta, std::wstring& error);

// Writes to a temporary file next to `target`, verifies it, then atomically replaces `target`
// (or moves into place if it doesn't exist). `target` may be the source file itself.
bool saveEdited(const PsdFile& src, const Edits& edits, const std::wstring& target, std::wstring& error);

}  // namespace psd
