#include "ui/canvas.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <SDL_image.h>

#include "ui/fonts.h"

namespace ui {
namespace {

constexpr std::size_t kMaxCachedText = 512;

}  // namespace

Canvas::Canvas(SDL_Renderer* renderer) : renderer_(renderer) {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

Canvas::~Canvas() {
    ClearTextCache();
    for (auto& entry : roundTex_) {
        if (entry.second) SDL_DestroyTexture(entry.second);
    }
    for (auto& entry : images_) {
        if (entry.second) SDL_DestroyTexture(entry.second);
    }
}

void Canvas::FillRect(const Rect& r, Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_FRect f{r.x, r.y, r.w, r.h};
    SDL_RenderFillRectF(renderer_, &f);
}

SDL_Texture* Canvas::RoundTexture(int radius, int thickness) {
    const long key = (static_cast<long>(radius) << 16) | thickness;
    auto it = roundTex_.find(key);
    if (it != roundTex_.end()) return it->second;

    // A (2r+2)-pixel square: four quarter-circle corners and a 2-pixel solid middle that is stretched.
    // Coverage is 4x4 supersampled, so edges are anti-aliased at any scale near 1:1.
    const int size = 2 * radius + 2;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    const float r = static_cast<float>(radius);
    const float lo = r, hi = static_cast<float>(size) - r;
    const float ri = r - static_cast<float>(thickness);
    auto inside = [&](float x, float y, float rad, float inset) {
        const float l = lo, h = hi;
        const float cx = std::clamp(x, l, h), cy = std::clamp(y, l, h);
        const float dx = x - cx, dy = y - cy;
        if (x < inset || y < inset || x > size - inset || y > size - inset) return false;
        return dx * dx + dy * dy <= rad * rad;
    };
    auto* px = static_cast<std::uint32_t*>(surf->pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float fx = x + (sx + 0.5f) / 4.0f, fy = y + (sy + 0.5f) / 4.0f;
                    bool in = inside(fx, fy, r, 0.0f);
                    if (in && thickness > 0) in = !inside(fx, fy, std::max(0.0f, ri), static_cast<float>(thickness));
                    hits += in ? 1 : 0;
                }
            }
            const std::uint8_t a = static_cast<std::uint8_t>(hits * 255 / 16);
            std::uint8_t* p = reinterpret_cast<std::uint8_t*>(&px[y * (surf->pitch / 4) + x]);
            p[0] = p[1] = p[2] = 255;
            p[3] = a;
        }
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    roundTex_.emplace(key, tex);
    return tex;
}

void Canvas::RoundShape(const Rect& r, float radius, int thickness, Color top, Color bottom) {
    if (r.w <= 0.5f || r.h <= 0.5f) return;
    const int rad = std::max(1, static_cast<int>(std::lround(std::min({radius, r.w * 0.5f, r.h * 0.5f}))));
    const int t = thickness > 0 ? std::min(thickness, rad) : 0;
    SDL_Texture* tex = RoundTexture(rad, t);
    if (!tex) return;
    const float size = static_cast<float>(2 * rad + 2);
    const float xs[4] = {r.x, r.x + rad, r.x + r.w - rad, r.x + r.w};
    const float ys[4] = {r.y, r.y + rad, r.y + r.h - rad, r.y + r.h};
    const float us[4] = {0.0f, rad / size, (size - rad) / size, 1.0f};
    SDL_Vertex v[16];
    for (int j = 0; j < 4; ++j) {
        const float f = r.h > 0 ? (ys[j] - r.y) / r.h : 0.0f;
        const Color c = Mix(top, bottom, f);
        for (int i = 0; i < 4; ++i) {
            v[j * 4 + i].position = {xs[i], ys[j]};
            v[j * 4 + i].color = {c.r, c.g, c.b, c.a};
            v[j * 4 + i].tex_coord = {us[i], us[j]};
        }
    }
    int idx[54];
    int n = 0;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const int a = j * 4 + i, b = a + 1, c = a + 4, d = a + 5;
            idx[n++] = a; idx[n++] = b; idx[n++] = c;
            idx[n++] = b; idx[n++] = d; idx[n++] = c;
        }
    }
    SDL_RenderGeometry(renderer_, tex, v, 16, idx, n);
}

void Canvas::FillRoundRect(const Rect& r, float radius, Color c) { RoundShape(r, radius, 0, c, c); }

void Canvas::FillGradientRoundRect(const Rect& r, float radius, Color top, Color bottom) {
    if (radius < 1.0f) {
        // Plain rectangles (the page background) need no corner texture.
        SDL_Vertex v[4] = {{{r.x, r.y}, {top.r, top.g, top.b, top.a}, {0, 0}},
                           {{r.x + r.w, r.y}, {top.r, top.g, top.b, top.a}, {0, 0}},
                           {{r.x, r.y + r.h}, {bottom.r, bottom.g, bottom.b, bottom.a}, {0, 0}},
                           {{r.x + r.w, r.y + r.h}, {bottom.r, bottom.g, bottom.b, bottom.a}, {0, 0}}};
        const int idx[6] = {0, 1, 2, 1, 3, 2};
        SDL_RenderGeometry(renderer_, nullptr, v, 4, idx, 6);
        return;
    }
    RoundShape(r, radius, 0, top, bottom);
}

void Canvas::StrokeRoundRect(const Rect& r, float radius, float thickness, Color c) {
    RoundShape(r, radius, std::max(1, static_cast<int>(std::lround(thickness))), c, c);
}

void Canvas::FillCircle(Vec2 center, float radius, Color c) {
    if (radius <= 0.0f) return;
    RoundShape(Rect{center.x - radius, center.y - radius, 2 * radius, 2 * radius}, radius, 0, c, c);
}

void Canvas::Shadow(const Rect& r, float radius, float spread, std::uint8_t alpha, float offsetY) {
    // The launcher uses a flat look: depth comes from borders and fills, not drop shadows.
    (void)r;
    (void)radius;
    (void)spread;
    (void)alpha;
    (void)offsetY;
}






void Canvas::Icon(char32_t cp, int size, float x, float y, Color c) {
    // UTF-8 encode the codepoint and draw it as text in the icon face.
    std::string s;
    if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    Text(TextStyle{FontFace::Icon, size}, s, x, y, c, Align::Center, VAlign::Middle);
}

SDL_Texture* Canvas::Texture(const std::string& path) {
    auto it = images_.find(path);
    if (it == images_.end()) {
        SDL_Texture* tex = IMG_LoadTexture(renderer_, path.c_str());
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        }
        it = images_.emplace(path, tex).first;
    }
    return it->second;
}

void Canvas::Image(const std::string& path, const Rect& r, std::uint8_t alpha) {
    SDL_Texture* tex = Texture(path);
    if (!tex) return;
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_FRect dst{r.x, r.y, r.w, r.h};
    SDL_RenderCopyF(renderer_, tex, nullptr, &dst);
}

const Canvas::CachedText& Canvas::Lookup(TextStyle style, const std::string& s) {
    const std::string key = std::to_string(static_cast<int>(style.face)) + ":" +
                            std::to_string(style.size) + ":" + s;
    auto it = textCache_.find(key);
    if (it != textCache_.end()) return it->second;

    if (textCache_.size() >= kMaxCachedText) ClearTextCache();

    CachedText entry;
    TTF_Font* font = GetFont(style.face, style.size);
    if (font && !s.empty()) {
        // White glyphs; the colour is applied as a texture tint at draw time.
        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, s.c_str(), SDL_Color{255, 255, 255, 255});
        if (surface) {
            entry.tex = SDL_CreateTextureFromSurface(renderer_, surface);
            entry.w = surface->w;
            entry.h = surface->h;
            SDL_FreeSurface(surface);
            if (entry.tex) SDL_SetTextureBlendMode(entry.tex, SDL_BLENDMODE_BLEND);
        }
    }
    return textCache_.emplace(key, entry).first->second;
}

void Canvas::Text(TextStyle style, const std::string& s, float x, float y, Color c, Align h, VAlign v) {
    const CachedText& t = Lookup(style, s);
    if (!t.tex) return;
    float dx = x;
    float dy = y;
    if (h == Align::Center) dx -= t.w * 0.5f;
    if (h == Align::Right) dx -= t.w;
    if (v == VAlign::Middle) dy -= t.h * 0.5f;
    if (v == VAlign::Bottom) dy -= t.h;
    SDL_SetTextureColorMod(t.tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t.tex, c.a);
    SDL_FRect dst{std::round(dx), std::round(dy), static_cast<float>(t.w), static_cast<float>(t.h)};
    SDL_RenderCopyF(renderer_, t.tex, nullptr, &dst);
}

float Canvas::TextWidth(TextStyle style, const std::string& s) {
    return static_cast<float>(Lookup(style, s).w);
}

float Canvas::TextHeight(TextStyle style) {
    TTF_Font* font = GetFont(style.face, style.size);
    return font ? static_cast<float>(TTF_FontHeight(font)) : static_cast<float>(style.size);
}

void Canvas::ClearTextCache() {
    for (auto& entry : textCache_) {
        if (entry.second.tex) SDL_DestroyTexture(entry.second.tex);
    }
    textCache_.clear();
}

}  // namespace ui
