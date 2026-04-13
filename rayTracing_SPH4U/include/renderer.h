#pragma once
#include <cstdint>
#include <cmath>
#include "../include/objects.h"

Vec3 normalize(Vec3 v);
void render(uint32_t* pixels, int w, int h); // pixels are 4 channels each 8 bit, therefore 32 bit