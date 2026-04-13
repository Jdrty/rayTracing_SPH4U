#pragma once
#include <cstdint>
#include <cmath>

struct Vec3 {
    float x, y, z;
};

void render(uint32_t* pixels, int w, int h); // pixels are 4 channels each 8 bit, therefore 32 bit