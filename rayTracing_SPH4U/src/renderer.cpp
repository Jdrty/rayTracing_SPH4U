#include "../include/renderer.h"
#include <cmath>

Vec3 normalize(Vec3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return { v.x / len, v.y / len, v.z / len };
}

void render(uint32_t* pixels, int w, int h) {
    Sphere sphere = { {0.0f, 0.0f, -3.0f}, 1.0f };
    Vec3 origin = { 0.0f, 0.0f, 0.0f };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {

            float u = (float)x / (float)w;
            float v = (float)y / (float)h;

            float sx = 2.0f * u - 1.0f;
            float sy = 1.0f - 2.0f * v;

            Vec3 dir = normalize({ sx, sy, -1.0f });

            float t;
            if (intersectSphere(origin, dir, sphere, t)) {
                // hit → white
                pixels[y * w + x] = 0x00FFFFFF;
            }
            else {
                // miss → black
                pixels[y * w + x] = 0x00000000;
            }
        }
    }
}