#pragma once

struct SDL_Window;

namespace rocket::widescreen {

// Called only from the SDL/window-owner thread. The guest culling hook reads
// the published aspect atomically and never touches SDL itself.
void update_window_aspect(SDL_Window* window);

} // namespace rocket::widescreen
