// Unpacks mod downloads dropped into the mods folder as .zip files.
#pragma once

#include <string>

namespace mkw::zipx {

// Extracts `zip` (stored or deflated entries; no Zip64 or encryption) into `dest`, which must not
// exist yet. A single folder wrapping everything is unwrapped, so "Mod.zip/Mod/riivolution" lands
// as "Mod/riivolution". Entries with absolute or ".." paths fail the whole extraction. On failure
// nothing is left at `dest` and `error` says why.
bool Extract(const std::string& zip, const std::string& dest, std::string& error);

}  // namespace mkw::zipx
