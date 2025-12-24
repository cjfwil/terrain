#pragma comment(lib, "SDL3.lib")

#include "src/metadata.h"

#include <SDL3/SDL.h>



int main(void)
{
    
    if (!SetExtendedMetadata())
        return 1;

    SDL_Log("Hello World!");
    return (0);
}