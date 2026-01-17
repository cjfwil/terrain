#pragma once

#include <SDL3/SDL.h>

struct v3
{
    float x;
    float y;
    float z;

    v3 operator+(const v3 &v) const { return {x + v.x, y + v.y, z + v.z}; }
    v3 operator-(const v3 &v) const { return {x - v.x, y - v.y, z - v.z}; }
    v3 operator*(float s) const { return {x * s, y * s, z * s}; }

    static v3 cross(const v3 &a, const v3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

    static v3 normalised(const v3 &a)
    {
        float mag = SDL_sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
        return {a.x / mag, a.y / mag, a.z / mag};
    }
};
