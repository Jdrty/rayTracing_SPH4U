#include "../include/objects.h"
#include <cmath>

static inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool intersectSphere(Vec3 ro, Vec3 rd, Sphere s, float& t) {
    Vec3 oc = {
        ro.x - s.center.x,
        ro.y - s.center.y,
        ro.z - s.center.z
    };

    float b = dot(oc, rd);
    float c = dot(oc, oc) - s.radius * s.radius;

    float disc = b * b - c;
    if (disc < 0.0f) return false;

    float sqrtD = sqrtf(disc);

    float t0 = -b - sqrtD;
    float t1 = -b + sqrtD;

    if (t0 > 0.001f) t = t0;
    else if (t1 > 0.001f) t = t1;
    else return false;

    return true;
}