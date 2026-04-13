#include "../include/renderer.h"

Vec3 normalize(Vec3 v) {
	float len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return { v.x / len, v.y / len, v.z / len };
}

void render(uint32_t* pixels, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {

            float u = (float)x / (float)w;
            float v = (float)y / (float)h;

            // map to screen space (-1 to 1)
            float sx = 2.0f * u - 1.0f;
            float sy = 1.0f - 2.0f * v;

            Vec3 dir = normalize({ sx, sy, -1.0f });

            // direction as color
            // TEMPORARY!!!!
            uint8_t r = (dir.x * 0.5f + 0.5f) * 255;
            uint8_t g = (dir.y * 0.5f + 0.5f) * 255;
            uint8_t b = (dir.z * 0.5f + 0.5f) * 255;

            pixels[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
}