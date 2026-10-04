#pragma once
#include "../html/parser.h"
#include <SDL2/SDL.h>
#include <string>

namespace browser {

SDL_Window* createWindow(const std::string& title, int w, int h);
std::string findDocTitle(const std::shared_ptr<Node>& root);
void destroyWindow(SDL_Window* w);

} // namespace browser
