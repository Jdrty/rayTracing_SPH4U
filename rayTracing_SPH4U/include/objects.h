#pragma once
#include <cstdint>

struct Vec3 {
    float x, y, z;
};

enum MaterialType {
    DIFFUSE,
    METAL,
    DIELECTRIC,
    SHARD,
    DIELECTRIC_SHARD,
    METAL_SHARD
};

struct Material {
    Vec3 color;
    int32_t type;
    /** Refractive index n (outside air → solid) for DIELECTRIC / DIELECTRIC_SHARD. */
    float refractiveIndex;
    /** RMS microfacet roughness in [0,1] (~GGX-ish response via cone reflection). */
    float roughness;
    /** Metallic weight in Disney-style PBS: drives spec tint vs tinted mirror vs diffuse weight. */
    float metallic;
    /** Polyurethane-clearcoat thickness proxy [0,1]; uses ~n ≈ 1.45 Fresnel overlay. */
    float clearcoat;
    /** Lambert wrap + warm scatter proxy [0,1]; common for porous clay/brick/paper. */
    float subsurfaceStrength;
    /** Rough coefficient of restitution for shard floor contacts (bounce). */
    float coefficientOfRestitution;
    /** Retained horizontal slip speed per bounce (fractional loss from friction/drag). */
    float tangentialDampingFactor;
};

struct Sphere {
    Vec3 center;
    float radius;
    Material material;
};

bool intersectSphere(Vec3 ro, Vec3 rd, Sphere s, float& t);