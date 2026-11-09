// Font handles, loaded lazily from romfs and kept for the life of the app.
#pragma once

#include <SDL_ttf.h>

#include "ui/canvas.h"

namespace ui {

// Returns the TTF handle for a face at a point size, or nullptr if romfs lacks the file.
TTF_Font* GetFont(FontFace face, int size);

// Closes every cached font. Call once before TTF_Quit().
void CloseFonts();

}  // namespace ui
