#include "../include/renderer.h"
#include <cmath>

static const int MAX_DEPTH = 5;
static const float EPSILON = 0.002f;

// math
static inline Vec3 add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline Vec3 sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline Vec3 mul(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline Vec3 mulV(Vec3 a, Vec3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
static inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline Vec3 normalize(Vec3 v) {
    float len = sqrtf(dot(v, v));
    return { v.x / len, v.y / len, v.z / len };
}
static inline Vec3 reflect(Vec3 d, Vec3 n) {
    return sub(d, mul(n, 2.0f * dot(d, n)));
}
static bool refract(Vec3 d, Vec3 n, float eta, Vec3& out) {
    float cosi = -dot(d, n);
    float cost2 = 1.0f - eta * eta * (1.0f - cosi * cosi);
    if (cost2 < 0.0f) return false;

    float cost = sqrtf(cost2);
    out = add(mul(d, eta), mul(n, eta * cosi - cost));
    return true;
}
static float fresnel(float cosi, float ior) {
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 *= r0;
    return r0 + (1.0f - r0) * powf(1.0f - cosi, 5.0f);
}

// sky actual energy source for reflections
static Vec3 sky(Vec3 rd) {
    float t = 0.5f * (rd.y + 1.0f);
    return {
        (1.0f - t) + t * 0.5f,
        (1.0f - t) + t * 0.7f,
        1.0f
    };
}

// trace - Added ior_offset parameter
static Vec3 trace(Vec3 ro, Vec3 rd, Sphere* spheres, int count, int depth, float ior_offset = 0.0f) {
    if (depth > MAX_DEPTH) return { 0,0,0 };

    float closest = 1e30f;
    Sphere* hitObj = nullptr;

    for (int i = 0; i < count; i++) {
        float t;
        if (intersectSphere(ro, rd, spheres[i], t)) {
            if (t < closest) {
                closest = t;
                hitObj = &spheres[i];
            }
        }
    }

    if (!hitObj) {
        return sky(rd);
    }

    Vec3 hit = add(ro, mul(rd, closest));
    Vec3 normal = normalize(sub(hit, hitObj->center));
    Material m = hitObj->material;

    // diffuse
    if (m.type == DIFFUSE) {
        Vec3 lightDir = normalize({ 1.0f, 1.0f, 1.0f });

        // shadow
        bool shadow = false;
        for (int i = 0; i < count; i++) {
            float t;
            if (&spheres[i] != hitObj &&
                intersectSphere(add(hit, mul(normal, EPSILON)), lightDir, spheres[i], t)) {
                shadow = true;
                break;
            }
        }

        float diff = dot(normal, lightDir);
        if (diff < 0) diff = 0;
        if (shadow) diff *= 0.2f;

        float ambient = 0.1f;

        return mul(m.color, ambient + diff * (1.0f - ambient));
    }

    // metal
    if (m.type == METAL) {
        Vec3 refl = reflect(rd, normal);

        // correct bias
        Vec3 origin = add(hit, mul(normal, EPSILON));

        // pass ior
        Vec3 col = trace(origin, refl, spheres, count, depth + 1, ior_offset);

        // energy heh
        return add(mulV(col, m.color), mul(m.color, 0.05f));
    }

    // glass
    if (m.type == DIELECTRIC) {
        // offset
        float ior = m.refractiveIndex + ior_offset;

        float cosi = dot(rd, normal);
        float etai = 1.0f, etat = ior;
        Vec3 n = normal;

        bool outside = cosi < 0;

        if (!outside) {
            n = mul(normal, -1.0f);
            etai = ior;
            etat = 1.0f;
        }

        float eta = etai / etat;

        // reflection
        Vec3 reflDir = reflect(rd, normal);
        Vec3 reflOrigin = add(hit, mul(n, EPSILON));

        // pass ior
        Vec3 reflCol = trace(reflOrigin, reflDir, spheres, count, depth + 1, ior_offset);

        // refraction
        Vec3 refrDir;
        float kr = fresnel(fabsf(cosi), ior);

        if (refract(rd, n, eta, refrDir)) {
            Vec3 refrOrigin = add(hit, mul(n, -EPSILON));

            // pass ior
            Vec3 refrCol = trace(refrOrigin, refrDir, spheres, count, depth + 1, ior_offset);

            return {
                reflCol.x * kr + refrCol.x * (1.0f - kr),
                reflCol.y * kr + refrCol.y * (1.0f - kr),
                reflCol.z * kr + refrCol.z * (1.0f - kr)
            };
        }

        return reflCol;
    }

    return { 0,0,0 };
}

// render
void render(uint32_t* pixels, int w, int h) {

    float aspect = (float)w / (float)h;
    float scale = tanf(90.0f * 0.5f * 3.14159265f / 180.0f);

    Vec3 origin = { 0,0,0 };

    Sphere spheres[] = {
        {{ 0, 0,-3}, 1.0f, {DIFFUSE,    {0.7f,0.7f,0.7f}, 1.0f, 0.0f}},
        {{ 2, 0,-4}, 1.0f, {METAL,      {0.9f,0.9f,0.9f}, 1.0f, 1.0f}},
        {{-2, 0,-4}, 1.0f, {DIELECTRIC, {1.0f,1.0f,1.0f}, 1.5f, 0.0f}},

        {{ 0, -1001, -3}, 1000.0f, {DIFFUSE, {0.2f,0.8f,0.2f}, 1.0f, 0.0f}}
    };

    int count = 4;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {

            float u = (x + 0.5f) / w;
            float v = (y + 0.5f) / h;

            float sx = 2.0f * u - 1.0f;
            float sy = 1.0f - 2.0f * v;

            Vec3 dir = normalize({
                sx * aspect * scale,
                sy * scale,
                -1.0f
                });

            // adjust spread if needed
            float spread = 0.03f;

            // trace 3 times
            Vec3 col = {
                trace(origin, dir, spheres, count, 0, -spread).x, // red pass
                trace(origin, dir, spheres, count, 0, 0.0f).y,    // green pass
                trace(origin, dir, spheres, count, 0, spread).z   // blue pass
            };

            if (col.x > 1) col.x = 1;
            if (col.y > 1) col.y = 1;
            if (col.z > 1) col.z = 1;

            uint8_t r = (uint8_t)(col.x * 255);
            uint8_t g = (uint8_t)(col.y * 255);
            uint8_t b = (uint8_t)(col.z * 255);

            pixels[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
}