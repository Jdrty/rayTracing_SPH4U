#pragma once
struct Vec3 {
    float x, y, z;
};

enum MaterialType {
    DIFFUSE,
    METAL,
    DIELECTRIC
};

struct Material {
    MaterialType type;
    Vec3 color;            
    float refractiveIndex; 
    float reflectivity;    
};

struct Sphere {
    Vec3 center;
    float radius;
    Material material;
};

bool intersectSphere(Vec3 ro, Vec3 rd, Sphere s, float& t);