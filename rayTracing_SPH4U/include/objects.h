#pragma once
struct Vec3 {
    float x, y, z;
};

struct Sphere {
    Vec3 center;
    float radius;
};

bool intersectSphere(Vec3 ro, Vec3 rd, Sphere s, float& t);