#include "app/browser.h"
#include "css/style.h"
#include "html/parser.h"
#include "js/jsengine.h"
#include "layout/font_loader.h"
#include "layout/layout.h"
#include "layout/resource.h"
#include "media/mediaplayer.h"
#include "render/renderer.h"
#include "render/window.h"
#include "tests/selftest.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace browser;

// Fail fast: print the SDL error and exit non-zero. The old code
// swallowed SDL_Init failures silently and crashed later in a
// confusing way.
static void sdlDie(const char* where) {
    std::cerr << "[fatal] " << where << ": " << SDL_GetError() << "\n";
    SDL_Quit();
    std::exit(1);
}

// One-frame offscreen render: `browser --screenshot out.bmp URL` loads
// the page and saves the rendered window as a BMP. Handled here with
// SDL_VIDEODRIVER=dummy so it also works without a display (CI).
static int runScreenshot(const std::string& outFile, const std::string& url) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) sdlDie("SDL_Init");
    if (TTF_Init() != 0)                sdlDie("TTF_Init");
    TTF_Font* font = loadFont();
    if (!font)                          sdlDie("loadFont");

    // Window size overridable via env for testing tall pages.
    const int W = getenv("MB_W") ? atoi(getenv("MB_W")) : 1024;
    const int H = getenv("MB_H") ? atoi(getenv("MB_H")) : 800;
    SDL_Window* win = createWindow("screenshot", W, H);
    if (!win)                           sdlDie("createWindow");
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren)                           sdlDie("SDL_CreateRenderer");

    Browser b(win, ren, font);
    // Screenshot = one deterministic shot: block inside navigate() until
    // the document is fetched and the image queue has drained (the
    // interactive build loads both asynchronously).
    b.setSynchronousNavigation(true);
    b.navigate(url.empty() ? "about:home" : url);
    // First paint without JS, then run the deferred scripts and repaint,
    // matching what the interactive loop does (and keeping JS-driven
    // content in headless screenshots).
    b.tick();
    b.paint();
    b.tick();
    b.paint();
    // Give late image jobs one bounded extra beat, then repaint so the
    // shot contains as much of the page as the network allowed.
    ResourceLoader::instance().waitForPendingImages(5000);
    // Media players: wait for the first decoded frame, then pump a few
    // paints so autoplay videos advance and the shot shows real video
    // content (not the loading placeholder). Bridged YouTube URLs spend
    // the first seconds resolving (yt-dlp + Piped fallback), so allow
    // a generous window here.
    browser::media::waitForFirstFrames(9000);
    for (int i = 0; i < 8; ++i) {
        b.tick();
        b.paint();
        SDL_Delay(125);   // ~1 s of wall-clock playback for autoplay clips
    }
    b.tick();
    b.paint();

    SDL_Surface* shot =
        SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
    if (shot) {
        if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32,
                                 shot->pixels, shot->pitch) == 0) {
            SDL_SaveBMP(shot, outFile.c_str());
            std::cout << "[screenshot] wrote " << outFile << "\n";
        } else {
            std::cerr << "[screenshot] readpixels failed: "
                      << SDL_GetError() << "\n";
        }
        SDL_FreeSurface(shot);
    }

    clearImageTextureCache();
    clearTextTextureCache();
    browser::media::stopAllPlayers();
    SDL_DestroyRenderer(ren);
    destroyWindow(win);
    shutdownFonts();
    shutdownImages();
    TTF_Quit();
    SDL_Quit();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        return browser::runSelfTest();
    }
    if (argc > 1 && std::string(argv[1]) == "--screenshot") {
        std::string out = (argc > 2) ? argv[2] : "screenshot.bmp";
        std::string url = (argc > 3) ? argv[3] : "about:home";
        return runScreenshot(out, url);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) sdlDie("SDL_Init");
    if (TTF_Init() != 0)                sdlDie("TTF_Init");

    // Build identity banner: lets the user verify at a glance that the
    // binary they are running contains the latest work (element geometry
    // in JS, parser implied end tags, selection, expanded JS DOM API).
    // Any stale copy of the executable prints the old date here.
    std::fprintf(stderr,
                 "[mini-browser] 2.6-round11 built " __DATE__ " " __TIME__
                 " (internal <video>/<audio> player + image viewer lightbox)\n");

    TTF_Font* font = loadFont();
    if (!font)                          sdlDie("loadFont");

    const int WIN_W = 960, WIN_H = 720;
    SDL_Window* win = createWindow("MiniBrowser", WIN_W, WIN_H);
    if (!win)                           sdlDie("createWindow");

    // Prefer the GPU: on anything with a working GL/ES driver, blitting
    // the (cached) page textures is orders of magnitude cheaper than the
    // software renderer memcpy-ing every sprite on the CPU — that was a
    // major cost on the target weak device. No GPU? Fall back cleanly.
    // Nearest-neighbour scaling: text textures are blitted 1:1, and
    // linear filtering costs fill rate for zero benefit here.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren)                           sdlDie("SDL_CreateRenderer");
    {
        SDL_RendererInfo ri{};
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            std::cerr << "[render] using " << ri.name << " renderer\n";
    }

    Browser b(win, ren, font);
    // Load the initial page: either the user-supplied argument (a URL or
    // a file) or the built-in home page.
    std::string startPage = (argc > 1) ? argv[1] : "about:home";
    b.navigate(startPage);

    // Event-driven loop: paint only when the frame is stale, sleep in
    // between (nextWakeupMs returns the JS-timer / caret-blink deadline,
    // or -1 to block until the next OS event). Idle CPU: zero — the old
    // loop repainted the whole window 60x per second forever, which is
    // exactly the wrong thing on a weak device.
    bool running = true;
    while (running) {
        SDL_Event e;
        int timeout = b.nextWakeupMs();
        if (SDL_WaitEventTimeout(&e, timeout)) {
            do {
                if (!b.handleEvent(e)) { running = false; break; }
            } while (SDL_PollEvent(&e));
        }
        if (!running) break;
        b.tick();                     // JS timers + deferred page scripts
        if (b.needsRepaint()) b.paint();
    }

    // Tear-down order: textures first (they depend on the renderer), then
    // the renderer, then the surface cache, then SDL itself.
    clearImageTextureCache();
    clearTextTextureCache();
    browser::media::stopAllPlayers();
    SDL_DestroyRenderer(ren);
    destroyWindow(win);
    shutdownFonts();
    shutdownImages();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
