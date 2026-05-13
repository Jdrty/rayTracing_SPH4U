struct Material {
    float3 color;
    int type;
    float refractiveIndex;
    float reflectivity;
    float2 padding;
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

cbuffer FrameConstants : register(b0) {
    uint Width;
    uint Height;
    uint FrameIndex;
    uint SphereCount;
};

static const int DIFFUSE = 0;
static const int METAL = 1;
static const int DIELECTRIC = 2;
static const uint RAY_TYPE_RADIANCE = 0;
static const uint RAY_TYPE_SHADOW = 1;
static const uint MAX_DEPTH = 5;
static const float EPSILON = 0.002f;

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

[shader("raygeneration")]
void RayGen() {
    uint2 pixel = DispatchRaysIndex().xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(Width, Height);
    float aspect = (float)Width / (float)Height;
    float scale = tan(radians(90.0f) * 0.5f);

    float2 screen;
    screen.x = 2.0f * uv.x - 1.0f;
    screen.y = 1.0f - 2.0f * uv.y;

    float3 origin = float3(0.0f, 0.0f, 0.0f);
    float3 direction = normalize(float3(screen.x * aspect * scale, screen.y * scale, -1.0f));
    float3 color = traceRadiance(origin, direction, 0);

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

    if (material.type == DIFFUSE) {
        float3 lightDir = normalize(float3(1.0f, 1.0f, 1.0f));
        bool shadowed = traceShadow(hit + normal * EPSILON, lightDir);
        float diff = max(dot(normal, lightDir), 0.0f);
        if (shadowed) {
            diff *= 0.2f;
        }

        float ambient = 0.1f;
        payload.color = material.color * (ambient + diff * (1.0f - ambient));
        return;
    }

    if (material.type == METAL) {
        float3 reflection = normalize(reflect(rd, normal));
        float3 color = traceRadiance(hit + normal * EPSILON, reflection, payload.depth + 1);
        payload.color = color * material.color + material.color * 0.05f;
        return;
    }

    if (material.type == DIELECTRIC) {
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
        float3 reflectionColor = traceRadiance(hit + n * EPSILON, reflection, payload.depth + 1);

        float3 refraction;
        float kr = fresnel(abs(cosi), ior);
        if (refractDir(rd, n, eta, refraction)) {
            float3 refractionColor = traceRadiance(hit - n * EPSILON, refraction, payload.depth + 1);
            payload.color = reflectionColor * kr + refractionColor * (1.0f - kr);
        }
        else {
            payload.color = reflectionColor;
        }
        return;
    }

    payload.color = 0.0f;
}
