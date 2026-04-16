#include "../include/renderer.h"
#include <cmath>

Vec3 normalize(Vec3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return { v.x / len, v.y / len, v.z / len };
}

// how aligned two vectors are
// tells us how concentrated light should be
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
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

                // compute hit point
                Vec3 hitPoint = {
                    origin.x + dir.x * t,
                    origin.y + dir.y * t,
                    origin.z + dir.z * t
                };
                
                //surface normal
                Vec3 normal = normalize({
                    hitPoint.x - sphere.center.x,
                    hitPoint.y - sphere.center.y,
                    hitPoint.z - sphere.center.z
                    });

                Vec3 lightDir = normalize({ -1.0f, -1.0f, -1.0f });

                // make it not look terrible
                float ambient = 0.1f;

                float brightness = dot(normal, lightDir);
                if (brightness < 0) brightness = 0;

                brightness = ambient + brightness * (1.0f - ambient);

                uint8_t c = (uint8_t)(brightness * 255);
                pixels[y * w + x] = (c << 16) | (c << 8) | c;
            }
        }
    }
}