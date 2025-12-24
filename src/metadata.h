#pragma once

#include <SDL3/SDL.h>

#define APP_NAME "terrain"
#define APP_VERSION "0.1.0"
#define APP_IDENTIFIER "online.cjfwil.terrain"
#define APP_AUTHOR "cjfwil"
#define APP_COPYRIGHT "Copyright 2026 cjfwil. All rights reserved."
#define APP_URL "https://github.com/cjfwil/terrain"
#define APP_TYPE "game"
#define APP_DESCRIPTION "A test bench for terrain rendering"
#define APP_WINDOW_TITLE "Terrain"

static const struct
{
    const char *key;
    const char *value;
} extended_metadata[] = {
    {SDL_PROP_APP_METADATA_URL_STRING, APP_URL},
    {SDL_PROP_APP_METADATA_IDENTIFIER_STRING, APP_IDENTIFIER},
    {SDL_PROP_APP_METADATA_VERSION_STRING, APP_VERSION},
    {SDL_PROP_APP_METADATA_CREATOR_STRING, APP_AUTHOR},
    {SDL_PROP_APP_METADATA_COPYRIGHT_STRING, APP_COPYRIGHT},
    {SDL_PROP_APP_METADATA_TYPE_STRING, APP_TYPE}};

static bool SetExtendedMetadata(void)
{
    bool result = true;
    if (!SDL_SetAppMetadata(APP_NAME, APP_VERSION, APP_IDENTIFIER))
    {
        result = false;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetAppMetadata failed: %s", SDL_GetError());
    }
    else
    {
        unsigned int i;
        for (i = 0; i < SDL_arraysize(extended_metadata); i++)
        {
            if (!SDL_SetAppMetadataProperty(extended_metadata[i].key, extended_metadata[i].value))
            {
                result = false;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetAppMetadataProperty(%s) failed: %s", extended_metadata[i].key, SDL_GetError());
            }
        }
    }
    return (result);
}