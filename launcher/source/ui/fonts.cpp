#include "ui/fonts.h"

#include <cstdio>
#include <map>
#include <utility>

namespace ui {
namespace {

const char* PathFor(FontFace face) {
    switch (face) {
        case FontFace::Display:  return "romfs:/fonts/Nunito-ExtraBold.ttf";
        case FontFace::BodySemi: return "romfs:/fonts/Nunito-Bold.ttf";
        case FontFace::Body:     return "romfs:/fonts/Nunito-SemiBold.ttf";
        case FontFace::Icon:     return "romfs:/fonts/MaterialIconsRound.otf";
    }
    return "romfs:/fonts/Nunito-SemiBold.ttf";
}

std::map<std::pair<int, int>, TTF_Font*>& Cache() {
    static std::map<std::pair<int, int>, TTF_Font*> cache;
    return cache;
}

}  // namespace

TTF_Font* GetFont(FontFace face, int size) {
    auto& cache = Cache();
    const auto key = std::make_pair(static_cast<int>(face), size);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    TTF_Font* font = TTF_OpenFont(PathFor(face), size);
    if (!font) {
        std::fprintf(stderr, "[launcher] font missing: %s (%s)\n", PathFor(face), TTF_GetError());
    }
    cache.emplace(key, font);
    return font;
}

void CloseFonts() {
    for (auto& entry : Cache()) {
        if (entry.second) TTF_CloseFont(entry.second);
    }
    Cache().clear();
}

}  // namespace ui
