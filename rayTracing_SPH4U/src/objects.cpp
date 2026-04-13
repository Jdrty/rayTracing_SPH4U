#include "../include/objects.h"
#include <cmath>

bool intersectSphere(Vec3 ro, Vec3 rd, Sphere s, float& t) {
    Vec3 oc = { ro.x - s.center.x, ro.y - s.center.y, ro.z - s.center.z };

    float a = rd.x * rd.x + rd.y * rd.y + rd.z * rd.z;
    float b = 2.0f * (oc.x * rd.x + oc.y * rd.y + oc.z * rd.z);
    float c = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - s.radius * s.radius;

    float discriminant = b * b - 4 * a * c;
    if (discriminant < 0) return false;

    t = (-b - sqrtf(discriminant)) / (2.0f * a);
    return t > 0;
}