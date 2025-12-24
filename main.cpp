#pragma comment(lib, "SDL3.lib")

#include "src/metadata.h"
#include "src/error.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>

static struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    Uint64 ticksElapsed;
    bool isRunning;
} programState;

int main(void)
{
    if (!SetExtendedMetadata())
        return 1;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        err("SDL_Init failed");
        return 1;
    }

    if (!SDL_CreateWindowAndRenderer(APP_WINDOW_TITLE, 1920, 1080, SDL_WINDOW_BORDERLESS, &programState.window, &programState.renderer))
    {
        err("SDL_CreateWindowAndRenderer failed");
        return 1;
    }

    programState.isRunning = true;
    while (programState.isRunning)
    {
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            switch (sdlEvent.type)
            {
            case SDL_EVENT_QUIT:
                programState.isRunning = false;
                break;
            }
        }
        programState.ticksElapsed = SDL_GetTicks();
    }
    return (0);
}