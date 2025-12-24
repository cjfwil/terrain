#pragma once

#define err(msg)                                                                   \
    do                                                                             \
    {                                                                              \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", msg, SDL_GetError()); \
    } while (0)
