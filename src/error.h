#pragma once

#include <windows.h>

#define err(msg)                                                                   \
    do                                                                             \
    {                                                                              \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", msg, SDL_GetError()); \
    } while (0)

#define errhr(msg, hr)                                                                                        \
    do                                                                                                    \
    {                                                                                                     \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: HR=0x%08X (%s)", msg, (hr), HRESULT_Message(hr)); \
    } while (0)

const char *HRESULT_Message(HRESULT hr)
{
    static char buffer[512];
    DWORD chars = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD)hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer, sizeof(buffer), NULL);

    if (chars == 0)
    {
        sprintf_s(buffer, "Unknown error HRESULT = 0x%08X", (UINT)hr);
    }
    return buffer;
}
