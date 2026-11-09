// Host stand-ins for code the tests link but do not exercise: zip unpacking (needs zlib) and the
// disc extractor (needs a real disc image).
#include <string>

#include "data/zip_extract.h"
#include "disc_extractor.h"

namespace mkw::zipx {
bool Extract(const std::string&, const std::string&, std::string& error) {
    error = "zip support is not built into the host tests";
    return false;
}
}  // namespace mkw::zipx

namespace melee_nx::disc {
bool isExtractionComplete(const std::string&) { return false; }
bool looksLikeMeleeDisc(const std::string&, std::string& reason) {
    reason = "host test";
    return false;
}
}  // namespace melee_nx::disc
