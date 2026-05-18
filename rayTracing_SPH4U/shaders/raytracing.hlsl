struct Material {
    float3 color;
    int type;
    float refractiveIndex;
    float roughness;
    float metallic;
    float clearcoat;
    float subsurfaceStrength;
    float coefficientOfRestitution;
    float tangentialDampingFactor;
};

struct Sphere {
    float3 center;
    float radius;
    Material material;
};

struct RayPayload {
    float3 color;
    uint depth;
    uint hit;
    uint rayType;
};

struct SphereAttributes {
    float3 normal;
};

RWTexture2D<float4> Output : register(u0);
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Sphere> Spheres : register(t1);
StructuredBuffer<uint> WeaponIndices : register(t2);
StructuredBuffer<float3> WeaponNormals : register(t3);

cbuffer FrameConstants : register(b0) {
    uint Width;
    uint Height;
    uint FrameIndex;
    uint SphereCount;
    float3 CameraPosition;
    float CameraFovY;
    float3 CameraForward;
    float CameraPad0;
    float3 CameraRight;
    float CameraPad1;
    float3 CameraUp;
    float CameraPad2;
};

static const int DIFFUSE = 0;
static const int METAL = 1;
static const int DIELECTRIC = 2;
static const int SHARD = 3;
static const int DIELECTRIC_SHARD = 4;
static const int METAL_SHARD = 5;
static const uint RAY_TYPE_RADIANCE = 0;
static const uint RAY_TYPE_SHADOW = 1;
static const uint MAX_DEPTH = 5;
static const float EPSILON = 0.002f;
static const float CLEARCOAT_IOR = 1.45f;
static const float3 NEUTRAL_ENV = float3(0.52f, 0.548f, 0.582f);

float3 sky(float3 rd) {
    float t = 0.5f * (rd.y + 1.0f);
    return lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
}

bool refractDir(float3 d, float3 n, float eta, out float3 refracted) {
    float cosi = -dot(d, n);
    float cost2 = 1.0f - eta * eta * (1.0f - cosi * cosi);
    if (cost2 < 0.0f) {
        refracted = 0.0f;
        return false;
    }

    refracted = normalize(d * eta + n * (eta * cosi - sqrt(cost2)));
    return true;
}

float fresnel(float cosi, float ior) {
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 *= r0;
    return r0 + (1.0f - r0) * pow(1.0f - cosi, 5.0f);
}

float3 traceRadiance(float3 origin, float3 direction, uint depth) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = EPSILON;
    ray.TMax = 1.0e30f;

    RayPayload payload;
    payload.color = 0.0f;
    payload.depth = depth;
    payload.hit = 0;
    payload.rayType = RAY_TYPE_RADIANCE;

    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
    return payload.color;
}

bool traceShadow(float3 origin, float3 direction) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = EPSILON;
    ray.TMax = 1.0e30f;

    RayPayload payload;
    payload.color = 0.0f;
    payload.depth = MAX_DEPTH;
    payload.hit = 0;
    payload.rayType = RAY_TYPE_SHADOW;

    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 1, 0, ray, payload);
    return payload.hit != 0;
}

// Fast opaque stack: Lambert + clay wrap + analytic sky reflections (no extra TraceRay recursion).
float3 shadeOpaquePhysical(Material material, float3 hit, float3 normal, float3 rd, bool rimShard) {
    float3 lightDir = normalize(float3(1.0f, 1.0f, 1.0f));
    bool shadowed = traceShadow(hit + normal * EPSILON, lightDir);
    float NdL = saturate(dot(normal, lightDir));
    if (shadowed) {
        NdL *= 0.22f;
    }

    float3 albedo = saturate(material.color);
    float rough = saturate(material.roughness);
    float metallic = saturate(material.metallic);
    float coat = saturate(material.clearcoat);
    float subs = saturate(material.subsurfaceStrength);

    float NdLw = saturate((dot(normal, lightDir) + 0.45f) / 1.45f);
    NdL = lerp(NdL, NdLw, subs * 0.55f);

    float NdV = saturate(abs(dot(normal, normalize(-rd))));
    float3 F0_spec = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 kD_albedo = albedo * (1.0f - metallic);

    float ambient = 0.11f;
    float3 diffuse = kD_albedo * (ambient + (1.0f - ambient) * NdL);
    diffuse += subs * saturate(NdLw * 1.02f + 0.045f) * albedo * float3(0.2f, 0.084f, 0.054f);

    float3 reflDir = normalize(reflect(rd, normal));
    float blur = saturate(rough * rough);
    float3 glossEnv = lerp(sky(reflDir), NEUTRAL_ENV, blur);
    float specMask = saturate((1.0f - rough * 0.88f) * (0.08f + metallic * 1.02f));

    float3 baseShaded = saturate(diffuse + glossEnv * F0_spec * specMask);

    float3 coatDir = normalize(reflect(rd, normal));
    float kcoat = saturate(fresnel(NdV, CLEARCOAT_IOR));
    float3 shaded = saturate(baseShaded + coat * sky(coatDir) * (0.065f + 0.935f * kcoat));

    if (rimShard) {
        float rim = 0.14f * pow(1.0f - saturate(abs(dot(normalize(-rd), normal))), 2.0f);
        shaded += albedo * rim;
    }

    return shaded;
}

float shardHash(uint shardIndex, uint planeIndex) {
    return frac(sin((float)shardIndex * 12.9898f + (float)planeIndex * 78.233f) * 43758.5453f);
}

bool intersectShard(float3 origin, float3 direction, float3 center, float radius, uint shardIndex, out float hitT, out float3 hitNormal) {
    float3 localOrigin = origin - center;
    float tNear = RayTMin();
    float tFar = RayTCurrent();
    hitNormal = -direction;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        float3 n = float3(
            (i & 1) ? 1.0f : -1.0f,
            (i & 2) ? 1.0f : -1.0f,
            (i & 4) ? 1.0f : -1.0f
        );
        n = normalize(n);

        float crackBias = shardHash(shardIndex, i);
        float planeRadius = radius * lerp(0.58f, 1.55f, crackBias);
        if ((shardIndex + i) % 5 == 0) {
            planeRadius *= 0.48f;
        }

        float signedDistance = dot(n, localOrigin) - planeRadius;
        float denom = dot(n, direction);

        if (abs(denom) < 1.0e-5f) {
            if (signedDistance > 0.0f) {
                hitT = 0.0f;
                return false;
            }
            continue;
        }

        float t = -signedDistance / denom;
        if (denom < 0.0f) {
            if (t > tNear) {
                tNear = t;
                hitNormal = normalize(n);
            }
        }
        else {
            tFar = min(tFar, t);
        }

        if (tNear > tFar) {
            hitT = 0.0f;
            return false;
        }
    }

    hitT = tNear;
    return hitT > RayTMin() && hitT < RayTCurrent();
}

[shader("raygeneration")]
void RayGen() {
    uint2 pixel = DispatchRaysIndex().xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(Width, Height);
    float aspect = (float)Width / (float)Height;
    float scale = tan(CameraFovY * 0.5f);

    float2 screen;
    screen.x = 2.0f * uv.x - 1.0f;
    screen.y = 1.0f - 2.0f * uv.y;

    float3 origin = CameraPosition;
    float3 direction = normalize(CameraForward + CameraRight * (screen.x * aspect * scale) + CameraUp * (screen.y * scale));
    float3 color = traceRadiance(origin, direction, 0);

    int2 center = int2(Width / 2, Height / 2);
    int2 delta = abs(int2(pixel) - center);
    bool horizontal = delta.y <= 0 && delta.x >= 3 && delta.x <= 5;
    bool vertical = delta.x <= 0 && delta.y >= 3 && delta.y <= 5;
    if (horizontal || vertical) {
        color = float3(1.0f, 1.0f, 1.0f);
    }

    Output[pixel] = float4(saturate(color), 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload) {
    payload.hit = 0;
    payload.color = payload.rayType == RAY_TYPE_SHADOW ? 0.0f : sky(WorldRayDirection());
}

[shader("intersection")]
void SphereIntersection() {
    uint sphereIndex = PrimitiveIndex();
    if (sphereIndex >= SphereCount) {
        return;
    }

    Sphere sphere = Spheres[sphereIndex];
    if (sphere.radius <= 0.0001f) {
        return;
    }

    if (sphere.material.type == SHARD || sphere.material.type == DIELECTRIC_SHARD ||
        sphere.material.type == METAL_SHARD) {
        float t;
        SphereAttributes attrs;
        if (intersectShard(ObjectRayOrigin(), ObjectRayDirection(), sphere.center, sphere.radius, sphereIndex, t, attrs.normal)) {
            ReportHit(t, 0, attrs);
        }
        return;
    }

    float3 oc = ObjectRayOrigin() - sphere.center;
    float b = dot(oc, ObjectRayDirection());
    float c = dot(oc, oc) - sphere.radius * sphere.radius;
    float disc = b * b - c;

    if (disc < 0.0f) {
        return;
    }

    float sqrtDisc = sqrt(disc);
    float t0 = -b - sqrtDisc;
    float t1 = -b + sqrtDisc;
    float t = t0 > RayTMin() ? t0 : t1;

    if (t > RayTMin() && t < RayTCurrent()) {
        SphereAttributes attrs;
        float3 hit = ObjectRayOrigin() + ObjectRayDirection() * t;
        attrs.normal = normalize(hit - sphere.center);
        ReportHit(t, 0, attrs);
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in SphereAttributes attrs) {
    payload.hit = 1;

    if (payload.rayType == RAY_TYPE_SHADOW) {
        payload.color = 0.0f;
        return;
    }

    if (payload.depth >= MAX_DEPTH) {
        payload.color = 0.0f;
        return;
    }

    uint sphereIndex = PrimitiveIndex();
    Sphere sphere = Spheres[sphereIndex];
    Material material = sphere.material;

    float3 rd = normalize(WorldRayDirection());
    float hitT = RayTCurrent();
    float3 hit = WorldRayOrigin() + rd * hitT;
    float3 normal = normalize(attrs.normal);

    if (material.type == DIFFUSE || material.type == SHARD || material.type == METAL || material.type == METAL_SHARD) {
        bool rim = (material.type == SHARD || material.type == METAL_SHARD);
        payload.color = shadeOpaquePhysical(material, hit, normal, rd, rim);
        return;
    }

    if (material.type == DIELECTRIC || material.type == DIELECTRIC_SHARD) {
        float ior = material.refractiveIndex;
        float cosi = dot(rd, normal);
        float etai = 1.0f;
        float etat = ior;
        float3 n = normal;

        if (cosi >= 0.0f) {
            n = -normal;
            etai = ior;
            etat = 1.0f;
        }

        float eta = etai / etat;
        float3 reflection = normalize(reflect(rd, normal));
        float kr = fresnel(abs(cosi), ior);

        float3 reflectionColor = sky(reflection);
        float3 refractionDir;
        if (refractDir(rd, n, eta, refractionDir)) {
            float3 refractionColor = sky(refractionDir);
            payload.color = (reflectionColor * kr + refractionColor * (1.0f - kr)) * material.color;
        }
        else {
            payload.color = reflectionColor * material.color;
        }
        return;
    }

    payload.color = 0.0f;
}

[shader("closesthit")]
void WeaponClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attrs) {
    payload.hit = 1;

    if (payload.rayType == RAY_TYPE_SHADOW) {
        payload.color = 0.0f;
        return;
    }

    uint tri = PrimitiveIndex() * 3;
    uint i0 = WeaponIndices[tri + 0];
    uint i1 = WeaponIndices[tri + 1];
    uint i2 = WeaponIndices[tri + 2];

    float3 bary;
    bary.yz = attrs.barycentrics;
    bary.x = 1.0f - bary.y - bary.z;

    float3 normal = normalize(
        WeaponNormals[i0] * bary.x +
        WeaponNormals[i1] * bary.y +
        WeaponNormals[i2] * bary.z
    );

    float3 lightDir = normalize(float3(1.0f, 1.0f, 1.0f));
    float diffuse = max(dot(normal, lightDir), 0.0f);
    float fresnelEdge = pow(1.0f - saturate(abs(dot(normal, -WorldRayDirection()))), 3.0f);
    float3 baseColor = float3(0.12f, 0.12f, 0.13f);
    float3 metalTint = float3(0.72f, 0.70f, 0.66f);

    payload.color = baseColor * (0.20f + diffuse * 0.65f) + metalTint * fresnelEdge * 0.25f;
}
