// Legacy (Pascal) layer-name encoding. Photoshop, GIMP and friends read the Unicode 'luni' name;
// the Pascal name is only kept sensible: its code page is guessed from the names already in the
// file (the one that decodes them to their 'luni' text), falling back to the system code page.
#pragma once

#include <string>
#include <vector>

namespace psd {

struct LayerRecord;

int guessCodepage(const std::vector<LayerRecord>& layers);
std::u16string decodeLegacy(const std::string& bytes, int codepage);
// Encodes `name` in `codepage` ('?' for what it can't represent), at most 255 bytes, never
// splitting a character.
std::string encodeLegacy(const std::u16string& name, int codepage);

}  // namespace psd
