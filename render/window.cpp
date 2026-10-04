#include "window.h"
#include <functional>
#include <iostream>

namespace browser {

std::string findDocTitle(const std::shared_ptr<Node>& root) {
    std::string title;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                if (c->tag == "title" && title.empty()) {
                    for (auto& t : c->children)
                        if (t->tag == "text") title += t->text;
                } else {
                    walk(c);
                }
            }
        };
    walk(root);
    size_t a = title.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "Minimal Browser";
    size_t b = title.find_last_not_of(" \t\r\n");
    return title.substr(a, b - a + 1);
}

SDL_Window* createWindow(const std::string& title, int w, int h) {
    SDL_Window* win = SDL_CreateWindow(title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
        SDL_WINDOW_SHOWN);
    if (!win) std::cerr << "SDL_CreateWindow error: " << SDL_GetError() << "\n";
    return win;
}

void destroyWindow(SDL_Window* w) {
    if (w) SDL_DestroyWindow(w);
}

} // namespace browser
