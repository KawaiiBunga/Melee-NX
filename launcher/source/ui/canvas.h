// Drawing helpers over SDL_Renderer in logical 1280x720 space.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <SDL.h>

#include "ui/geometry.h"

namespace ui {

enum class FontFace : std::uint8_t { Display, Body, BodySemi, Icon };

struct TextStyle {
    FontFace face;
    int size;
};

// Shared type scale (Nunito). Sizes are in points at logical resolution.
inline constexpr TextStyle kDisplayXL{FontFace::Display, 56};
inline constexpr TextStyle kDisplay{FontFace::Display, 34};
inline constexpr TextStyle kTitle{FontFace::Display, 26};
inline constexpr TextStyle kBody{FontFace::Body, 21};
inline constexpr TextStyle kBodyStrong{FontFace::BodySemi, 21};
inline constexpr TextStyle kLabel{FontFace::BodySemi, 19};
inline constexpr TextStyle kSmall{FontFace::Body, 17};

// Material Icons Round codepoints used by the launcher.
namespace icon {
inline constexpr char32_t kHome = 0xE88A, kSave = 0xE161, kBadge = 0xEA67, kFace = 0xE87C, kFolder = 0xE2C7,
                          kSettings = 0xE8B8, kUpdate = 0xE62A, kFlag = 0xF06E, kPerson = 0xE7FD,
                          kPlay = 0xE037, kCheck = 0xE86C, kWarning = 0xE002, kTune = 0xE429,
                          kChevron = 0xE5CF, kRestore = 0xE889, kExtension = 0xE87B, kGamepad = 0xEA28,
                          kPublic = 0xE80B, kDelete = 0xE872, kRefresh = 0xE5D5, kLogin = 0xEA77,
                          kLogout = 0xE9BA, kLayers = 0xE53B, kBolt = 0xEA0B, kAdd = 0xE145,
                          kWifi = 0xE63E, kInfo = 0xE88E, kArrowUp = 0xE5D8, kArrowDown = 0xE5DB;
}

enum class Align : std::uint8_t { Left, Center, Right };
enum class VAlign : std::uint8_t { Top, Middle, Bottom };

class Canvas {
public:
    explicit Canvas(SDL_Renderer* renderer);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    SDL_Renderer* sdl() const { return renderer_; }

    void FillRect(const Rect& r, Color c);
    void FillRoundRect(const Rect& r, float radius, Color c);
    void StrokeRoundRect(const Rect& r, float radius, float thickness, Color c);
    void FillCircle(Vec2 center, float radius, Color c);
    // Vertical gradient inside a rounded rectangle.
    void FillGradientRoundRect(const Rect& r, float radius, Color top, Color bottom);
    // Soft drop shadow under a rounded rectangle.
    void Shadow(const Rect& r, float radius, float spread, std::uint8_t alpha, float offsetY = 4.0f);
    // A Material Icons glyph centred on (x, y).
    void Icon(char32_t codepoint, int size, float x, float y, Color c);
    // A PNG from romfs, cached; drawn stretched to `r`.
    void Image(const std::string& path, const Rect& r, std::uint8_t alpha = 255);

    // Text is rendered once per (face, size, string) and tinted per draw.
    void Text(TextStyle style, const std::string& s, float x, float y, Color c,
              Align h = Align::Left, VAlign v = VAlign::Top);
    float TextWidth(TextStyle style, const std::string& s);
    float TextHeight(TextStyle style);

    // Drops cached text textures; called when the font set changes.
    void ClearTextCache();

private:
    struct CachedText {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
    };
    const CachedText& Lookup(TextStyle style, const std::string& s);
    SDL_Texture* Texture(const std::string& path);
    // Anti-aliased rounded shapes: a cached corner texture drawn as a 9-slice, coloured per vertex.
    SDL_Texture* RoundTexture(int radius, int thickness);
    void RoundShape(const Rect& r, float radius, int thickness, Color top, Color bottom);

    SDL_Renderer* renderer_;
    std::unordered_map<std::string, CachedText> textCache_;
    std::unordered_map<std::string, SDL_Texture*> images_;
    std::unordered_map<long, SDL_Texture*> roundTex_;
};

}  // namespace ui
