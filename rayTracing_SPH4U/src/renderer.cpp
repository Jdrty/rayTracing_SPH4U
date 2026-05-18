#include "../include/renderer.h"
#include "../include/objects.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <climits>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <wincodec.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT FrameCount = 2;
constexpr UINT DescriptorCount = 7;
constexpr UINT ShardsPerTarget = 12;
constexpr float TargetOrbRadius = 0.55f;
/// Seconds after a target orb is destroyed before it respawns.
constexpr float TargetOrbRegenSeconds = 2.0f;

/// Shard positions are orb center + offset (same table used on spawn and respawn).
constexpr std::array<Vec3, ShardsPerTarget> kShardLocalOffsets = {{
    { 0.31f, 0.05f, 0.02f },
    {-0.27f, 0.10f,-0.04f },
    { 0.08f, 0.34f, 0.03f },
    {-0.06f,-0.29f, 0.06f },
    { 0.04f, 0.03f, 0.34f },
    {-0.03f, 0.01f,-0.32f },
    { 0.22f, 0.25f, 0.12f },
    {-0.24f, 0.22f,-0.10f },
    { 0.19f,-0.20f,-0.14f },
    {-0.18f,-0.24f, 0.13f },
    { 0.02f, 0.18f,-0.25f },
    {-0.02f,-0.16f, 0.27f },
}};

/// Respawn volume loosely matches the original target layout (above floor plane y = 0).
constexpr float OrbRespawnMinX = -18.5f;
constexpr float OrbRespawnMaxX = 15.5f;
constexpr float OrbRespawnMinY = TargetOrbRadius;
constexpr float OrbRespawnMaxY = 6.35f;
constexpr float OrbRespawnMinZ = -31.5f;
constexpr float OrbRespawnMaxZ = -8.5f;

constexpr float WeaponInspectTotalSeconds = 5.5f;

constexpr float InspectUpushEnd = 0.22f;
constexpr float InspectUSpinFwdEnd = 0.48f;
constexpr float InspectUholdEnd = 0.62f;
constexpr float InspectUspinBackEnd = 0.82f;

constexpr float InspectReplayWarmBlendSeconds = 0.48f;

constexpr float WeaponInspectEyeToCentroidAlongView = 1.74f;

constexpr float InspectBoreThreatCentroidRelax = 0.82f;
constexpr float InspectBoreThreatPushAlongView = 0.42f;

constexpr float InspectHandoffOutwardAlongView = 0.17f;

constexpr float WeaponInspectAnchorDownAlongUp = 0.38f;

struct WeaponPose {
    Vec3 origin{};
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
};

static Vec3 v3lerp(Vec3 a, Vec3 b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

static Vec3 v3add(Vec3 a, Vec3 b) {
    return {
        a.x + b.x,
        a.y + b.y,
        a.z + b.z,
    };
}

static Vec3 v3scale(Vec3 v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}

static float v3len(Vec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static Vec3 v3norm(Vec3 v) {
    const float len = v3len(v);
    if (len < 1.0e-6f) {
        return { 1.0f, 0.0f, 0.0f };
    }

    const float inv = 1.0f / len;
    return { v.x * inv, v.y * inv, v.z * inv };
}

static Vec3 v3cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static float smoothstep01(float t) {
    if (t <= 0.0f) {
        return 0.0f;
    }

    if (t >= 1.0f) {
        return 1.0f;
    }

    return t * t * (3.0f - 2.0f * t);
}

/// Rotation ease ~ cos; zero slope at endpoints (heavy mid-spin acceleration like a eased wrist turn).
static float easeInspectSpinTurn01(float t) {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    return 0.5f - 0.5f * cosf(t * 3.1415926535f);
}

/** Held-out inspect: clearly visible gentle float along camera.right + camera.up (not jittery). */
static Vec3 weaponInspectHoldSwayOffset(const CameraState& camera, float uNormElapsed, float timeSec) {
    const Vec3 Rt = v3norm(camera.right);
    const Vec3 Up = v3norm(camera.up);
    // Short smooth ramps so motion is strong through almost the entire inspect.
    const float rampIn = smoothstep01(fminf(1.0f, uNormElapsed / 0.09f));
    const float rampOut = smoothstep01(fminf(1.0f, (1.0f - uNormElapsed) / 0.14f));
    const float envelope = rampIn * rampOut;

    // Periods multi-second (~2.35 s / ~3 s): several readable lobes per clip, derivative still tame.
    constexpr float omegaSlow = 2.0f * 3.1415926535f * (1.0f / 3.05f);
    constexpr float omegaMid = 2.0f * 3.1415926535f * (1.0f / 2.35f);

    constexpr float ampMain = 0.078f;
    constexpr float ampMix = 0.032f;

    const float vv =
        ampMain * sinf(omegaSlow * timeSec + 0.35f) + ampMix * sinf(omegaMid * timeSec + 2.08f);
    const float hh =
        ampMain * sinf(omegaMid * timeSec + 1.12f) + ampMix * sinf(omegaSlow * timeSec + 2.64f);

    const Vec3 d = {
        Rt.x * hh + Up.x * vv,
        Rt.y * hh + Up.y * vv,
        Rt.z * hh + Up.z * vv,
    };
    return {
        d.x * envelope,
        d.y * envelope,
        d.z * envelope,
    };
}

// Instance rotation rows: weapon +X -> pose.right, +Y -> pose.up, +Z (barrel) -> pose.forward.
static WeaponPose weaponPoseFromBarrel(Vec3 barrelUnit, Vec3 camUpNorm) {
    const Vec3 b = v3norm(barrelUnit);
    const Vec3 uRef = v3norm(camUpNorm);
    Vec3 r = v3cross(b, uRef);
    const float rl = v3len(r);
    if (rl < 1.0e-4f) {
        // Barrel almost parallel cam.up — use +X aux axis.
        Vec3 aux = v3cross(b, { 1.0f, 0.0f, 0.0f });
        if (v3len(aux) < 1.0e-4f) {
            aux = v3cross(b, { 0.0f, 1.0f, 0.0f });
        }
        r = v3norm(aux);
    }
    else {
        const float inv = 1.0f / rl;
        r = { r.x * inv, r.y * inv, r.z * inv };
    }

    WeaponPose pose = {};
    pose.forward = b;
    pose.right = r;
    pose.up = v3norm(v3cross(r, b));
    return pose;
}

/** Short transition from “cancel pose” toward new nominal inspect pose — lerp origins + tween barrel axis then rebuild RH basis. */
static WeaponPose inspectPoseBlendRebuild(const WeaponPose& fromPose, const WeaponPose& toPose, float kt01,
    Vec3 camUpNorm) {
    WeaponPose blended = weaponPoseFromBarrel(v3norm(v3lerp(fromPose.forward, toPose.forward, kt01)),
        camUpNorm);
    blended.origin = v3lerp(fromPose.origin, toPose.origin, kt01);
    return blended;
}

/** Replay warm-blend mixes last emitted inspect pose with the new nominal pose; rebasing avoids breaking when camera moved/yawed. */
static WeaponPose inspectWarmPoseRebasedToCamera(const WeaponPose& poseAtSnapWorld, Vec3 snapEye, Vec3 sR,
    Vec3 sU, Vec3 sF, Vec3 currEye, Vec3 cR, Vec3 cU, Vec3 cF, Vec3 camUpForBasis) {
    sR = v3norm(sR);
    sU = v3norm(sU);
    sF = v3norm(sF);
    cR = v3norm(cR);
    cU = v3norm(cU);
    cF = v3norm(cF);

    const Vec3 d = {
        poseAtSnapWorld.origin.x - snapEye.x,
        poseAtSnapWorld.origin.y - snapEye.y,
        poseAtSnapWorld.origin.z - snapEye.z,
    };

    const float lx = d.x * sR.x + d.y * sR.y + d.z * sR.z;
    const float ly = d.x * sU.x + d.y * sU.y + d.z * sU.z;
    const float lz = d.x * sF.x + d.y * sF.y + d.z * sF.z;

    const Vec3 origin = {
        currEye.x + lx * cR.x + ly * cU.x + lz * cF.x,
        currEye.y + lx * cR.y + ly * cU.y + lz * cF.y,
        currEye.z + lx * cR.z + ly * cU.z + lz * cF.z,
    };

    const Vec3& B0 = poseAtSnapWorld.forward;
    const float bx = B0.x * sR.x + B0.y * sR.y + B0.z * sR.z;
    const float by = B0.x * sU.x + B0.y * sU.y + B0.z * sU.z;
    const float bz = B0.x * sF.x + B0.y * sF.y + B0.z * sF.z;

    const Vec3 fwd = v3norm({
        bx * cR.x + by * cU.x + bz * cF.x,
        bx * cR.y + by * cU.y + bz * cF.y,
        bx * cR.z + by * cU.z + bz * cF.z,
    });

    WeaponPose rebuilt = weaponPoseFromBarrel(fwd, camUpForBasis);
    rebuilt.origin = origin;
    return rebuilt;
}

static Vec3 weaponCentroidWorldOffset(const Vec3& localCentroid, float scale,
    const Vec3& weaponRight, const Vec3& weaponUp, const Vec3& weaponForward) {
    return {
        scale
            * (localCentroid.x * weaponRight.x + localCentroid.y * weaponUp.x
               + localCentroid.z * weaponForward.x),
        scale
            * (localCentroid.x * weaponRight.y + localCentroid.y * weaponUp.y
               + localCentroid.z * weaponForward.y),
        scale
            * (localCentroid.x * weaponRight.z + localCentroid.y * weaponUp.z
               + localCentroid.z * weaponForward.z)
    };
}

static Vec3 barrelDirectionInspectYaw(const CameraState& camera, float yawRad) {
    // Barrel sweeps in the plane of camera.right and -camera.forward (horizontal inspection spin).
    const Vec3 Rw = v3norm(camera.right);
    const Vec3 negFwd = { -camera.forward.x, -camera.forward.y, -camera.forward.z };
    const Vec3 bf = v3norm(negFwd);
    const float c = cosf(yawRad);
    const float s = sinf(yawRad);
    return v3norm({
        Rw.x * c + bf.x * s,
        Rw.y * c + bf.y * s,
        Rw.z * c + bf.z * s
    });
}

/** Side inspect barrel axis: planar spin + slow yaw drift + subtle up-wise lean so profile never locks perfectly stiff. */
static Vec3 inspectBarrelFacingFloated(const CameraState& camera, float spinYawRad, float inspectTimeSec) {
    constexpr float planarRad = 0.16f;
    const float planar = planarRad
        * (sinf(1.71f * inspectTimeSec + 0.52f) + 0.5f * sinf(2.48f * inspectTimeSec + 1.88f));

    Vec3 barrel = barrelDirectionInspectYaw(camera, spinYawRad + planar);

    const Vec3 cu = v3norm(camera.up);
    constexpr float upKickScale = 0.115f;
    const float lu = upKickScale
        * (sinf(1.93f * inspectTimeSec + 0.74f)
            + 0.46f * sinf(3.02f * inspectTimeSec + 2.63f));

    barrel = {
        barrel.x + cu.x * lu,
        barrel.y + cu.y * lu,
        barrel.z + cu.z * lu,
    };
    return v3norm(barrel);
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path);
std::vector<std::filesystem::path> searchRoots();

struct FrameConstants {
    UINT width;
    UINT height;
    UINT frameIndex;
    UINT sphereCount;
    Vec3 cameraPosition;
    float fovYRadians;
    Vec3 cameraForward;
    float pad0;
    Vec3 cameraRight;
    float pad1;
    Vec3 cameraUp;
    float pad2;
};

struct MeshData {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<uint32_t> indices;
};

struct WeaponGpuMesh {
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> normalBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> blasScratch;
    ComPtr<ID3D12Resource> blasResult;
    UINT vertexCount = 0;
    UINT indexCount = 0;
    float scale = 1.0f;
    Vec3 meshCentroidLocal{};
};

int parseIntAfter(const std::string& text, size_t pos) {
    pos = text.find_first_of("-0123456789", pos);
    if (pos == std::string::npos) {
        throw std::runtime_error("Expected integer in GLB JSON");
    }
    return std::stoi(text.substr(pos));
}

std::string jsonObjectFromArray(const std::string& json, const char* arrayName, int index) {
    const std::string key = std::string("\"") + arrayName + "\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        throw std::runtime_error("GLB JSON array not found");
    }

    pos = json.find('[', pos);
    if (pos == std::string::npos) {
        throw std::runtime_error("Malformed GLB JSON array");
    }

    int current = -1;
    for (++pos; pos < json.size(); ++pos) {
        if (json[pos] != '{') {
            continue;
        }

        ++current;
        int depth = 0;
        const size_t start = pos;
        for (; pos < json.size(); ++pos) {
            if (json[pos] == '{') {
                ++depth;
            }
            else if (json[pos] == '}') {
                --depth;
                if (depth == 0) {
                    if (current == index) {
                        return json.substr(start, pos - start + 1);
                    }
                    break;
                }
            }
        }
    }

    throw std::runtime_error("GLB JSON array index out of range");
}

int jsonIntProperty(const std::string& object, const char* propertyName, int defaultValue = 0) {
    const std::string key = std::string("\"") + propertyName + "\"";
    size_t pos = object.find(key);
    if (pos == std::string::npos) {
        return defaultValue;
    }
    return parseIntAfter(object, object.find(':', pos));
}

int glbAccessorForAttribute(const std::string& json, const char* attributeName) {
    const std::string key = std::string("\"") + attributeName + "\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        throw std::runtime_error("Required GLB mesh attribute not found");
    }
    return parseIntAfter(json, json.find(':', pos));
}

std::filesystem::path findAssetPath(const wchar_t* fileName) {
    for (const auto& root : searchRoots()) {
        const std::filesystem::path candidates[] = {
            root / L"assets" / fileName,
            root / L"rayTracing_SPH4U" / L"assets" / fileName
        };

        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    throw std::runtime_error("Could not find requested model asset");
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

float readF32(const std::vector<uint8_t>& bytes, size_t offset) {
    float value = 0.0f;
    memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

MeshData loadGlbMesh(const std::filesystem::path& path, const char* label) {
    const auto glb = readFileBytes(path);
    if (glb.size() < 28 || readU32(glb, 0) != 0x46546C67 || readU32(glb, 4) != 2) {
        throw std::runtime_error((std::string(label) + " asset is not a GLB v2 file").c_str());
    }

    size_t offset = 12;
    const uint32_t jsonLength = readU32(glb, offset);
    const uint32_t jsonType = readU32(glb, offset + 4);
    if (jsonType != 0x4E4F534A) {
        throw std::runtime_error((std::string(label) + " GLB is missing its JSON chunk").c_str());
    }
    const std::string json(reinterpret_cast<const char*>(glb.data() + offset + 8), jsonLength);
    offset += 8 + jsonLength;

    const uint32_t binLength = readU32(glb, offset);
    const uint32_t binType = readU32(glb, offset + 4);
    if (binType != 0x004E4942) {
        throw std::runtime_error((std::string(label) + " GLB is missing its BIN chunk").c_str());
    }
    const size_t binStart = offset + 8;
    if (binStart + binLength > glb.size()) {
        throw std::runtime_error((std::string(label) + " GLB BIN chunk is truncated").c_str());
    }

    const int positionAccessorIndex = glbAccessorForAttribute(json, "POSITION");
    const int normalAccessorIndex = glbAccessorForAttribute(json, "NORMAL");
    size_t indicesPos = json.find("\"indices\"");
    if (indicesPos == std::string::npos) {
        throw std::runtime_error((std::string(label) + " GLB mesh has no indices").c_str());
    }
    const int indexAccessorIndex = parseIntAfter(json, json.find(':', indicesPos));

    const auto positionAccessor = jsonObjectFromArray(json, "accessors", positionAccessorIndex);
    const auto normalAccessor = jsonObjectFromArray(json, "accessors", normalAccessorIndex);
    const auto indexAccessor = jsonObjectFromArray(json, "accessors", indexAccessorIndex);

    const auto positionView = jsonObjectFromArray(json, "bufferViews", jsonIntProperty(positionAccessor, "bufferView"));
    const auto normalView = jsonObjectFromArray(json, "bufferViews", jsonIntProperty(normalAccessor, "bufferView"));
    const auto indexView = jsonObjectFromArray(json, "bufferViews", jsonIntProperty(indexAccessor, "bufferView"));

    const int vertexCount = jsonIntProperty(positionAccessor, "count");
    const int indexCount = jsonIntProperty(indexAccessor, "count");
    const int positionStride = jsonIntProperty(positionView, "byteStride", 12);
    const int normalStride = jsonIntProperty(normalView, "byteStride", 12);
    const size_t positionBase = binStart + jsonIntProperty(positionView, "byteOffset") + jsonIntProperty(positionAccessor, "byteOffset");
    const size_t normalBase = binStart + jsonIntProperty(normalView, "byteOffset") + jsonIntProperty(normalAccessor, "byteOffset");
    const size_t indexBase = binStart + jsonIntProperty(indexView, "byteOffset") + jsonIntProperty(indexAccessor, "byteOffset");
    const int indexComponentType = jsonIntProperty(indexAccessor, "componentType");

    MeshData mesh;
    mesh.positions.resize(vertexCount);
    mesh.normals.resize(vertexCount);
    mesh.indices.resize(indexCount);

    for (int i = 0; i < vertexCount; ++i) {
        const size_t posOffset = positionBase + static_cast<size_t>(i) * positionStride;
        const size_t normalOffset = normalBase + static_cast<size_t>(i) * normalStride;
        mesh.positions[i] = { readF32(glb, posOffset), readF32(glb, posOffset + 4), readF32(glb, posOffset + 8) };
        mesh.normals[i] = { readF32(glb, normalOffset), readF32(glb, normalOffset + 4), readF32(glb, normalOffset + 8) };
    }

    for (int i = 0; i < indexCount; ++i) {
        if (indexComponentType == 5125) {
            mesh.indices[i] = readU32(glb, indexBase + static_cast<size_t>(i) * 4);
        }
        else if (indexComponentType == 5123) {
            uint16_t value = 0;
            memcpy(&value, glb.data() + indexBase + static_cast<size_t>(i) * 2, sizeof(value));
            mesh.indices[i] = value;
        }
        else if (indexComponentType == 5121) {
            mesh.indices[i] = glb[indexBase + i];
        }
        else {
            throw std::runtime_error((std::string("Unsupported ") + label + " GLB index format").c_str());
        }
    }

    return mesh;
}

void throwIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

UINT64 alignTo(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_CPU_DESCRIPTOR_HANDLE offsetCpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT offset, UINT size) {
    handle.ptr += static_cast<SIZE_T>(offset) * size;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE offsetGpu(D3D12_GPU_DESCRIPTOR_HANDLE handle, UINT offset, UINT size) {
    handle.ptr += static_cast<UINT64>(offset) * size;
    return handle;
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

std::filesystem::path findShaderPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::vector<std::filesystem::path> roots = {
        std::filesystem::current_path(),
        std::filesystem::path(exePath).parent_path(),
        std::filesystem::path(exePath).parent_path().parent_path(),
        std::filesystem::path(exePath).parent_path().parent_path().parent_path()
    };

    for (const auto& root : roots) {
        const std::filesystem::path candidates[] = {
            root / L"shaders" / L"raytracing.hlsl",
            root / L"rayTracing_SPH4U" / L"shaders" / L"raytracing.hlsl"
        };

        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    throw std::runtime_error("Could not find shaders/raytracing.hlsl");
}

std::vector<std::filesystem::path> searchRoots() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::vector<std::filesystem::path> roots = {
        std::filesystem::current_path(),
        std::filesystem::path(exePath).parent_path()
    };

    std::filesystem::path root = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 8 && root.has_parent_path(); ++i) {
        root = root.parent_path();
        roots.push_back(root);
    }

    return roots;
}

std::filesystem::path findDxcCompilerPath() {
    for (const auto& root : searchRoots()) {
        const std::filesystem::path candidates[] = {
            root / L"bin" / L"x64" / L"dxcompiler.dll",
            root / L"dxc" / L"bin" / L"x64" / L"dxcompiler.dll",
            root / L"dxcompiler.dll"
        };

        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    return {};
}

ComPtr<IDxcBlob> compileRayTracingLibrary() {
    using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

    std::filesystem::path dxcPath = findDxcCompilerPath();
    if (!dxcPath.empty()) {
        SetDllDirectoryW(dxcPath.parent_path().c_str());
    }

    HMODULE dxcModule = LoadLibraryW(dxcPath.empty() ? L"dxcompiler.dll" : dxcPath.c_str());
    if (!dxcModule) {
        throw std::runtime_error("dxcompiler.dll was not found. Install the DirectX Shader Compiler runtime or place dxcompiler.dll beside the executable.");
    }

    auto createDxcInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance"));
    if (!createDxcInstance) {
        throw std::runtime_error("dxcompiler.dll does not export DxcCreateInstance");
    }

    const auto shaderBytes = readFileBytes(findShaderPath());
    if (shaderBytes.empty()) {
        throw std::runtime_error("raytracing.hlsl is empty or unreadable");
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    throwIfFailed(createDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Failed to create DXC utils");
    throwIfFailed(createDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Failed to create DXC compiler");
    throwIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler), "Failed to create DXC include handler");

    DxcBuffer source = {};
    source.Ptr = shaderBytes.data();
    source.Size = shaderBytes.size();
    source.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T", L"lib_6_3",
        L"-Zi",
        L"-Qembed_debug",
        L"-O3"
    };

    ComPtr<IDxcResult> result;
    throwIfFailed(compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)),
        "DXC failed to compile raytracing.hlsl");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        std::string message = "raytracing.hlsl compilation failed";
        if (errors && errors->GetStringLength() > 0) {
            message += ": ";
            message += errors->GetStringPointer();
        }
        throw std::runtime_error(message.c_str());
    }

    ComPtr<IDxcBlob> library;
    throwIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&library), nullptr),
        "DXC did not produce a DXIL library");
    return library;
}

std::filesystem::path findOverlayShaderPath() {
    for (const auto& root : searchRoots()) {
        const std::filesystem::path candidates[] = {
            root / L"shaders" / L"overlay.hlsl",
            root / L"rayTracing_SPH4U" / L"shaders" / L"overlay.hlsl"
        };
        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }
    throw std::runtime_error("Could not find shaders/overlay.hlsl");
}

ComPtr<IDxcBlob> compileOverlayComputeLibrary() {
    using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

    std::filesystem::path dxcPath = findDxcCompilerPath();
    if (!dxcPath.empty()) {
        SetDllDirectoryW(dxcPath.parent_path().c_str());
    }

    HMODULE dxcModule = LoadLibraryW(dxcPath.empty() ? L"dxcompiler.dll" : dxcPath.c_str());
    if (!dxcModule) {
        throw std::runtime_error("dxcompiler.dll was not found (overlay)");
    }

    auto createDxcInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance"));
    if (!createDxcInstance) {
        throw std::runtime_error("dxcompiler.dll missing DxcCreateInstance");
    }

    const auto shaderBytes = readFileBytes(findOverlayShaderPath());
    if (shaderBytes.empty()) {
        throw std::runtime_error("overlay.hlsl is empty or unreadable");
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    throwIfFailed(createDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Failed to create DXC utils");
    throwIfFailed(createDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Failed to create DXC compiler");
    throwIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler), "Failed to create DXC include handler");

    DxcBuffer source = {};
    source.Ptr = shaderBytes.data();
    source.Size = shaderBytes.size();
    source.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T", L"cs_6_0",
        L"-E", L"main",
        L"-O3"
    };

    ComPtr<IDxcResult> result;
    throwIfFailed(compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)),
        "DXC failed to compile overlay.hlsl");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        std::string message = "overlay.hlsl compilation failed";
        if (errors && errors->GetStringLength() > 0) {
            message += ": ";
            message += errors->GetStringPointer();
        }
        throw std::runtime_error(message.c_str());
    }

    ComPtr<IDxcBlob> blob;
    throwIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr), "overlay.hlsl DXIL missing");
    return blob;
}

std::filesystem::path findNoirShaderPath() {
    for (const auto& root : searchRoots()) {
        const std::filesystem::path candidates[] = {
            root / L"shaders" / L"noir.hlsl",
            root / L"rayTracing_SPH4U" / L"shaders" / L"noir.hlsl"
        };
        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }
    throw std::runtime_error("Could not find shaders/noir.hlsl");
}

ComPtr<IDxcBlob> compileNoirComputeLibrary() {
    using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

    std::filesystem::path dxcPath = findDxcCompilerPath();
    if (!dxcPath.empty()) {
        SetDllDirectoryW(dxcPath.parent_path().c_str());
    }

    HMODULE dxcModule = LoadLibraryW(dxcPath.empty() ? L"dxcompiler.dll" : dxcPath.c_str());
    if (!dxcModule) {
        throw std::runtime_error("dxcompiler.dll was not found (noir)");
    }

    auto createDxcInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance"));
    if (!createDxcInstance) {
        throw std::runtime_error("dxcompiler.dll missing DxcCreateInstance");
    }

    const auto shaderBytes = readFileBytes(findNoirShaderPath());
    if (shaderBytes.empty()) {
        throw std::runtime_error("noir.hlsl is empty or unreadable");
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    throwIfFailed(createDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Failed to create DXC utils");
    throwIfFailed(createDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Failed to create DXC compiler");
    throwIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler), "Failed to create DXC include handler");

    DxcBuffer source = {};
    source.Ptr = shaderBytes.data();
    source.Size = shaderBytes.size();
    source.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T", L"cs_6_0",
        L"-E", L"main",
        L"-O3"
    };

    ComPtr<IDxcResult> result;
    throwIfFailed(compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)),
        "DXC failed to compile noir.hlsl");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        std::string message = "noir.hlsl compilation failed";
        if (errors && errors->GetStringLength() > 0) {
            message += ": ";
            message += errors->GetStringPointer();
        }
        throw std::runtime_error(message.c_str());
    }

    ComPtr<IDxcBlob> blob;
    throwIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr), "noir.hlsl DXIL missing");
    return blob;
}

UINT alignTextureRowPitch(UINT rowBytes) {
    return static_cast<UINT>((static_cast<uint64_t>(rowBytes) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) /
        static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) *
        static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
}

struct CpuRgbaImage {
    std::vector<uint8_t> pixels;
    UINT pixelWidth = 0;
    UINT pixelHeight = 0;
};

std::wstring findBannerPngPath() {
    constexpr const wchar_t* names[] = { L"killbanner.png", L"banner.png" };
    for (const wchar_t* name : names) {
        for (const auto& root : searchRoots()) {
            const std::filesystem::path candidates[] = {
                root / L"assets" / name,
                root / L"rayTracing_SPH4U" / L"assets" / name,
            };
            for (const auto& p : candidates) {
                if (std::filesystem::exists(p)) {
                    return p.wstring();
                }
            }
        }
    }
    throw std::runtime_error("Kill banner PNG not found in assets/(killbanner.png or banner.png)");
}

CpuRgbaImage loadPngRgbaViaWic(const std::wstring& fsPathW) {
    ComPtr<IWICImagingFactory> factory;
    throwIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
        "Failed to create WIC factory");

    ComPtr<IWICBitmapDecoder> decoder;
    throwIfFailed(factory->CreateDecoderFromFilename(fsPathW.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf()), "WIC PNG open failed");

    ComPtr<IWICBitmapFrameDecode> frame;
    throwIfFailed(decoder->GetFrame(0, frame.GetAddressOf()), "WIC PNG frame failed");

    ComPtr<IWICFormatConverter> conv;
    throwIfFailed(factory->CreateFormatConverter(conv.GetAddressOf()), "WIC format converter failed");

    throwIfFailed(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeMedianCut), "WIC PNG convert failed");

    UINT w = 0;
    UINT h = 0;
    conv->GetSize(&w, &h);
    const UINT stride = w * 4u;
    const UINT pitched = alignTextureRowPitch(stride);

    CpuRgbaImage img;
    img.pixelWidth = w;
    img.pixelHeight = h;
    img.pixels.resize(static_cast<size_t>(pitched) * static_cast<size_t>(h));

    throwIfFailed(conv->CopyPixels(nullptr, pitched, static_cast<UINT>(img.pixels.size()), img.pixels.data()),
        "WIC PNG pixels failed");
    return img;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}
}

struct DxrRenderer::Impl {
    HWND hwnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT frameIndex = 0;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    std::wstring lastError = L"";

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    ComPtr<ID3D12CommandAllocator> commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList4> commandList;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12Resource> sphereBuffer;
    ComPtr<ID3D12Resource> aabbBuffer;
    WeaponGpuMesh weaponMesh;
    ComPtr<ID3D12Resource> blasScratch;
    ComPtr<ID3D12Resource> blasResult;
    ComPtr<ID3D12Resource> tlasScratch;
    ComPtr<ID3D12Resource> tlasResult;
    ComPtr<ID3D12Resource> instanceBuffer;
    ComPtr<ID3D12RootSignature> globalRootSignature;
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    ComPtr<ID3D12Resource> shaderTable;

    std::vector<Sphere> sceneSpheres;
    std::vector<UINT> targetSphereIndices;
    std::vector<Vec3> sphereRestCenters;
    std::vector<float> targetOrbRegenRemaining;
    std::vector<Vec3> shardVelocities;
    std::vector<float> shardActiveRadii;
    std::vector<uint8_t> activeShards;
    std::mt19937 orbRespawnRng;
    std::chrono::steady_clock::time_point lastSimulationTick = std::chrono::steady_clock::now();
    float weaponRecoilTime = 1.0f;
    bool weaponInspectPlaying = false;
    float weaponInspectT = 0.0f;
    WeaponPose inspectReplayWarmFrom{};
    float inspectWarmBlendElapsed = 0.0f;
    mutable bool inspectReplayWarmBlend = false;
    mutable WeaponPose inspectLastEmittedInspectPose{};
    mutable Vec3 inspectLastEmittedCamEye{};
    mutable Vec3 inspectLastEmittedCamRight{};
    mutable Vec3 inspectLastEmittedCamUp{};
    mutable Vec3 inspectLastEmittedCamForward{};
    mutable bool inspectLastEmittedCamKnown = false;
    Vec3 inspectReplayWarmSnapEye{};
    Vec3 inspectReplayWarmSnapRight{};
    Vec3 inspectReplayWarmSnapUp{};
    Vec3 inspectReplayWarmSnapForward{};
    bool inspectReplayWarmSnapValid = false;
    bool sceneDirty = false;

    UINT rtvDescriptorSize = 0;
    UINT srvUavDescriptorSize = 0;
    UINT shaderRecordSize = 0;
    UINT64 rayGenOffset = 0;
    UINT64 missOffset = 0;
    UINT64 hitGroupOffset = 0;
    UINT sphereCount = 0;

    /// Short burst where fade-in overlaps fade-out so the envelope is almost always transitioning.
    static constexpr float KillBannerTotalSeconds = 0.17f;

    /// Screen span vs min(width,height); multiplied per-frame by expand animation (subtle—short-lived).
    static constexpr float KillBannerSpriteFrac = 0.128f;
    /// Vertical banner offset (+Y is down): 0 centers on screen mid-height.
    static constexpr float KillOverlayCenterDownFracOfHeight = 0.0f;

    /// Eased fades (overlap total duration → few frames at steady peak).
    static constexpr float KillBannerFadeInSeconds = 0.10f;
    static constexpr float KillBannerFadeOutSeconds = 0.098f;

    /// Opacity multiplier range; capped so combined sprite never exceeds ~70% opaque (≥30% see-through).
    static constexpr float KillBannerToneStrengthMin = 0.02f;
    static constexpr float KillBannerToneStrengthMax = 0.70f;
    static constexpr float KillBannerAlphaCapMax = 0.70f;

    /// Quick scale swell (must fit inside TotalSeconds).
    static constexpr float KillBannerExpandRampSeconds = 0.11f;
    static constexpr float KillBannerExpandScaleStart = 0.90f;
    static constexpr float KillBannerExpandScalePeak = 1.04f;

    ComPtr<ID3D12Resource> killScratchTexture;
    ComPtr<ID3D12Resource> bannerTextureGpu;
    UINT bannerTextureW = 0;
    UINT bannerTextureH = 0;
    ComPtr<ID3D12RootSignature> killOverlayRootSignature;
    ComPtr<ID3D12PipelineState> killOverlayPso;

    ComPtr<ID3D12RootSignature> noirOverlayRootSignature;
    ComPtr<ID3D12PipelineState> noirOverlayPso;

    bool killBannerActive = false;
    float killBannerElapsedSeconds = 0.0f;
    void initialize(HWND window, int renderWidth, int renderHeight) {
        hwnd = window;
        width = static_cast<UINT>(renderWidth);
        height = static_cast<UINT>(renderHeight);

        createDevice();
        createSwapChain();
        createDescriptors();
        createOutputTexture();
        createScene();
        orbRespawnRng.seed(std::random_device{}());
        createAccelerationStructures();
        createGlobalRootSignature();
        createStateObject();
        createShaderTable();
        createKillBannerGpuTextures();
        createKillBannerOverlayCompute();
        createNoirOverlayCompute();
    }

    void createDevice() {
        throwIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Failed to create DXGI factory");

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;
            factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
            ++index) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)))) {
                break;
            }
        }

        if (!device) {
            throw std::runtime_error("No Direct3D 12 feature level 12.1 adapter was found");
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        throwIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)),
            "Failed to query DXR feature support");
        if (options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            throw std::runtime_error("This GPU/driver does not expose DirectX Raytracing support");
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        throwIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)), "Failed to create command queue");

        for (UINT i = 0; i < FrameCount; ++i) {
            throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])),
                "Failed to create command allocator");
        }

        throwIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0].Get(), nullptr,
            IID_PPV_ARGS(&commandList)), "Failed to create command list");
        commandList->Close();

        throwIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Failed to create fence");
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            throw std::runtime_error("Failed to create fence event");
        }
    }

    void createSwapChain() {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = FrameCount;
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        throwIfFailed(factory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1),
            "Failed to create swap chain");
        throwIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "Failed to configure window association");
        throwIfFailed(swapChain1.As(&swapChain), "Failed to query IDXGISwapChain3");
        frameIndex = swapChain->GetCurrentBackBufferIndex();
    }

    void createDescriptors() {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = FrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        throwIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "Failed to create RTV heap");
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (UINT i = 0; i < FrameCount; ++i) {
            throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])), "Failed to get swap-chain buffer");
            device->CreateRenderTargetView(renderTargets[i].Get(), nullptr,
                offsetCpu(rtvHeap->GetCPUDescriptorHandleForHeapStart(), i, rtvDescriptorSize));
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvUavDesc = {};
        srvUavDesc.NumDescriptors = DescriptorCount;
        srvUavDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvUavDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throwIfFailed(device->CreateDescriptorHeap(&srvUavDesc, IID_PPV_ARGS(&srvUavHeap)), "Failed to create SRV/UAV heap");
        srvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    ComPtr<ID3D12Resource> createBuffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = heapType;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        throwIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
            IID_PPV_ARGS(&resource)), "Failed to create buffer");
        return resource;
    }

    void createOutputTexture() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        throwIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputTexture)), "Failed to create output texture");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));
    }

    void uploadToResource(ID3D12Resource* resource, const void* data, size_t size) {
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        throwIfFailed(resource->Map(0, &readRange, &mapped), "Failed to map upload resource");
        memcpy(mapped, data, size);
        resource->Unmap(0, nullptr);
    }

    void createScene() {
        sceneSpheres = {
            {{ 0.0f,-1000.0f,  0.0f}, 1000.0f,
                {{0.18f,0.20f,0.22f}, DIFFUSE, 1.0f, 0.91f, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f}}
        };
        targetSphereIndices.clear();
        sphereRestCenters = { sceneSpheres[0].center };
        shardActiveRadii = { 0.0f };

        auto addTarget = [this](Vec3 center, const Material& sphereMaterial) {
            int32_t shardMaterialType = SHARD;
            if (sphereMaterial.type == DIELECTRIC) {
                shardMaterialType = DIELECTRIC_SHARD;
            } else if (sphereMaterial.type == METAL) {
                shardMaterialType = METAL_SHARD;
            }
            const Vec3 sphereColor = sphereMaterial.color;
            targetSphereIndices.push_back(static_cast<UINT>(sceneSpheres.size()));
            sceneSpheres.push_back({ center, TargetOrbRadius, sphereMaterial });
            sphereRestCenters.push_back(center);
            shardActiveRadii.push_back(0.0f);

            const float activeRadii[] = {
                0.145f, 0.060f, 0.115f, 0.045f,
                0.095f, 0.052f, 0.075f, 0.130f,
                0.040f, 0.105f, 0.055f, 0.085f
            };

            for (UINT shard = 0; shard < ShardsPerTarget; ++shard) {
                const Vec3 offset = kShardLocalOffsets[shard];
                Vec3 shardCenter = { center.x + offset.x, center.y + offset.y, center.z + offset.z };
                const float brightness = (shard % 4) == 0 ? 0.42f : 0.72f + 0.02f * static_cast<float>(shard % 5);
                Vec3 shardColor = {
                    sphereColor.x * brightness,
                    sphereColor.y * brightness,
                    sphereColor.z * brightness
                };
                sceneSpheres.push_back({
                    shardCenter,
                    0.0f,
                    {shardColor,
                     shardMaterialType,
                     sphereMaterial.refractiveIndex,
                     sphereMaterial.roughness,
                     sphereMaterial.metallic,
                     sphereMaterial.clearcoat,
                     sphereMaterial.subsurfaceStrength,
                     sphereMaterial.coefficientOfRestitution,
                     sphereMaterial.tangentialDampingFactor}
                });
                sphereRestCenters.push_back(shardCenter);
                shardActiveRadii.push_back(activeRadii[shard]);
            }
        };

        // Targets: optics (n where relevant), PBS roughness/metallic, polyurethane clearcoat (n_clear ≈ 1.45 simulated in shader).
        const Material mats[] = {
            // Pearl / silver tri-coat: metallic base flake + glossy clearcoat.
            {{0.76f, 0.77f, 0.81f}, METAL,   1.0f, 0.41f, 0.92f, 0.93f, 0.035f, 0.39f, 0.82f},
            // Racing red urethane enamel (flake-pigmented base under clearcoat; low metallic flake).
            {{0.86f, 0.115f, 0.086f}, DIFFUSE,   1.58f, 0.478f, 0.098f, 0.962f, 0.042f, 0.395f, 0.835f},
            // Deep solid black urethane gloss (pigment + clearcoat, no flake).
            {{0.045f, 0.045f, 0.048f}, DIFFUSE, 1.585f, 0.395f, 0.0f, 0.94f, 0.018f, 0.42f, 0.825f},
            // Chrome-ish trim lacquer (smooth metal + thinner clear).
            {{0.85f, 0.862f, 0.873f}, METAL,   1.0f, 0.036f, 1.0f, 0.58f, 0.015f, 0.52f, 0.82f},
            // Aluminum wheel satin + light factory clear.
            {{0.74f, 0.744f, 0.758f}, METAL,   1.0f, 0.29f, 1.0f, 0.11f, 0.022f, 0.51f, 0.82f},
            // Brushed galvanized / satin zinc look.
            {{0.588f, 0.627f, 0.663f}, METAL,   1.0f, 0.31f, 1.0f, 0.075f, 0.022f, 0.53f, 0.82f},
            // Sapphire window (few true-transparent targets; n ~ 1.77).
            {{0.36f, 0.55f, 0.91f}, DIELECTRIC, 1.77f, 0.02f, 0.f, 0.f, 0.f, 0.12f, 0.66f},
            // Gloss ABS plastic (semi-gloss molded body panel).
            {{0.93f, 0.28f, 0.065f}, DIFFUSE, 1.555f, 0.225f, 0.f, 0.065f, 0.012f, 0.62f, 0.91f},
            // Ceramic terracotta (porous diffuse + scattering proxy).
            {{0.66f, 0.352f, 0.226f}, DIFFUSE, 1.585f, 0.935f, 0.f, 0.012f, 0.595f, 0.065f, 0.71f},
            // Fire brick / kiln clay.
            {{0.505f, 0.258f, 0.206f}, DIFFUSE, 1.56f, 0.935f, 0.f, 0.f, 0.382f, 0.068f, 0.69f},
            // Acrylic lacquered wood (grain tint + varnish clearcoat).
            {{0.428f, 0.266f, 0.096f}, DIFFUSE, 1.56f, 0.442f, 0.f, 0.695f, 0.065f, 0.455f, 0.815f},
            // CFRP epoxy composite (flake metal + directional roughness approximation).
            {{0.086f, 0.090f, 0.097f}, METAL,   1.0f, 0.52f, 0.095f, 0.068f, 0.065f, 0.465f, 0.795f},
            // Porcelain stoneware biscuit (fine matte ceramics).
            {{0.942f, 0.931f, 0.912f}, DIFFUSE, 1.59f, 0.892f, 0.f, 0.068f, 0.068f, 0.085f, 0.795f},
            // Carbon-black filled nitrile rubber (very matte, hysteretic bounce proxy).
            {{0.069f, 0.065f, 0.068f}, DIFFUSE, 1.52f, 0.987f, 0.0f, 0.0f, 0.072f, 0.11f, 0.548f},
        };

        constexpr std::array<Vec3, 18> targetCenters = {{
            {-14.8f, 5.45f,-28.0f},{-9.90f, 3.92f,-11.40f},{5.600f, 4.76f,-24.55f},{14.150f, 2.06f,-19.20f},{-11.30f, 1.58f,-16.82f},{3.090f, 0.55f,-8.970f},{-6.090f, 4.93f,-21.10f},{10.400f, 5.62f,-13.50f},{-2.790f, 2.71f,-28.93f},{13.760f, 4.28f,-9.090f},{-17.06f, 0.93f,-12.64f},{8.090f, 3.43f,-26.71f},{1.670f, 5.19f,-15.18f},{-4.930f, 0.71f,-23.94f},{6.790f, 1.93f,-18.72f},{12.970f, 1.41f,-22.96f},{0.090f, 1.82f,-13.62f},{8.790f, 5.16f,-30.54f},
        }};

        for (size_t i = 0; i < targetCenters.size(); ++i) {
            addTarget(targetCenters[i], mats[i % std::size(mats)]);
        }

        sphereCount = static_cast<UINT>(sceneSpheres.size());
        shardVelocities.assign(sceneSpheres.size(), { 0.0f, 0.0f, 0.0f });
        activeShards.assign(sceneSpheres.size(), 0);
        targetOrbRegenRemaining.assign(targetSphereIndices.size(), 0.0f);

        const size_t sphereBytes = sizeof(Sphere) * sceneSpheres.size();
        sphereBuffer = createBuffer(sphereBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(sphereBuffer.Get(), sceneSpheres.data(), sphereBytes);

        std::vector<D3D12_RAYTRACING_AABB> aabbs;
        aabbs.reserve(sphereCount);
        for (const Sphere& sphere : sceneSpheres) {
            const float boundsRadius = sphere.radius > 0.0001f ? sphere.radius : 6.0f;
            aabbs.push_back({
                sphere.center.x - boundsRadius,
                sphere.center.y - boundsRadius,
                sphere.center.z - boundsRadius,
                sphere.center.x + boundsRadius,
                sphere.center.y + boundsRadius,
                sphere.center.z + boundsRadius
            });
        }

        aabbBuffer = createBuffer(sizeof(D3D12_RAYTRACING_AABB) * aabbs.size(),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(aabbBuffer.Get(), aabbs.data(), sizeof(D3D12_RAYTRACING_AABB) * aabbs.size());

        D3D12_SHADER_RESOURCE_VIEW_DESC sphereSrvDesc = {};
        sphereSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sphereSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        sphereSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sphereSrvDesc.Buffer.NumElements = sphereCount;
        sphereSrvDesc.Buffer.StructureByteStride = sizeof(Sphere);
        device->CreateShaderResourceView(sphereBuffer.Get(), &sphereSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 2, srvUavDescriptorSize));

        createWeaponMesh(loadGlbMesh(findAssetPath(L"pistol__desert_eagle_weapon_model_cs2.glb"), "Pistol"), 5.0f);
        updateWeaponDescriptors();
    }

    void createWeaponMesh(const MeshData& mesh, float scale) {
        weaponMesh.vertexCount = static_cast<UINT>(mesh.positions.size());
        weaponMesh.indexCount = static_cast<UINT>(mesh.indices.size());
        weaponMesh.scale = scale;
        weaponMesh.meshCentroidLocal = {};

        if (!mesh.positions.empty()) {
            float minX = mesh.positions[0].x;
            float maxX = minX;
            float minY = mesh.positions[0].y;
            float maxY = minY;
            float minZ = mesh.positions[0].z;
            float maxZ = minZ;
            for (size_t i = 1; i < mesh.positions.size(); ++i) {
                const Vec3 p = mesh.positions[i];
                minX = fminf(minX, p.x);
                maxX = fmaxf(maxX, p.x);
                minY = fminf(minY, p.y);
                maxY = fmaxf(maxY, p.y);
                minZ = fminf(minZ, p.z);
                maxZ = fmaxf(maxZ, p.z);
            }

            weaponMesh.meshCentroidLocal = {
                0.5f * (minX + maxX),
                0.5f * (minY + maxY),
                0.5f * (minZ + maxZ),
            };
        }

        weaponMesh.vertexBuffer = createBuffer(sizeof(Vec3) * mesh.positions.size(),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(weaponMesh.vertexBuffer.Get(), mesh.positions.data(), sizeof(Vec3) * mesh.positions.size());

        weaponMesh.normalBuffer = createBuffer(sizeof(Vec3) * mesh.normals.size(),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(weaponMesh.normalBuffer.Get(), mesh.normals.data(), sizeof(Vec3) * mesh.normals.size());

        weaponMesh.indexBuffer = createBuffer(sizeof(uint32_t) * mesh.indices.size(),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(weaponMesh.indexBuffer.Get(), mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size());
    }

    void updateWeaponDescriptors() {
        D3D12_SHADER_RESOURCE_VIEW_DESC weaponIndexSrvDesc = {};
        weaponIndexSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        weaponIndexSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        weaponIndexSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        weaponIndexSrvDesc.Buffer.NumElements = weaponMesh.indexCount;
        weaponIndexSrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(weaponMesh.indexBuffer.Get(), &weaponIndexSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 3, srvUavDescriptorSize));

        D3D12_SHADER_RESOURCE_VIEW_DESC weaponNormalSrvDesc = {};
        weaponNormalSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        weaponNormalSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        weaponNormalSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        weaponNormalSrvDesc.Buffer.NumElements = weaponMesh.vertexCount;
        weaponNormalSrvDesc.Buffer.StructureByteStride = sizeof(Vec3);
        device->CreateShaderResourceView(weaponMesh.normalBuffer.Get(), &weaponNormalSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 4, srvUavDescriptorSize));
    }

    D3D12_RAYTRACING_INSTANCE_DESC sphereInstanceDesc() const {
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = 1.0f;
        instanceDesc.Transform[1][1] = 1.0f;
        instanceDesc.Transform[2][2] = 1.0f;
        instanceDesc.InstanceID = 0;
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.InstanceContributionToHitGroupIndex = 0;
        instanceDesc.AccelerationStructure = blasResult->GetGPUVirtualAddress();
        return instanceDesc;
    }

    WeaponPose computeHandWeaponPose(const CameraState& camera) const {
        float recoil = 0.0f;
        if (weaponRecoilTime < 0.34f) {
            const float t = weaponRecoilTime / 0.34f;
            const float attack = sinf(t * 3.1415926535f);
            const float settle = expf(-4.0f * t);
            recoil = attack * settle;
        }

        const float recoilAngle = recoil * 0.36f;
        const float recoilCos = cosf(recoilAngle);
        const float recoilSin = sinf(recoilAngle);
        Vec3 weaponForward = {
            camera.forward.x * recoilCos + camera.up.x * recoilSin,
            camera.forward.y * recoilCos + camera.up.y * recoilSin,
            camera.forward.z * recoilCos + camera.up.z * recoilSin
        };
        Vec3 weaponUp = {
            camera.up.x * recoilCos - camera.forward.x * recoilSin,
            camera.up.y * recoilCos - camera.forward.y * recoilSin,
            camera.up.z * recoilCos - camera.forward.z * recoilSin
        };

        const float forwardOffset = 0.65f;
        const float rightOffset = 0.64f;
        const float downOffset = 0.72f - recoil * 0.12f;
        const float backOffset = recoil * 0.10f;
        const Vec3 origin = {
            camera.position.x + camera.forward.x * (forwardOffset - backOffset) + camera.right.x * rightOffset - camera.up.x * downOffset,
            camera.position.y + camera.forward.y * (forwardOffset - backOffset) + camera.right.y * rightOffset - camera.up.y * downOffset,
            camera.position.z + camera.forward.z * (forwardOffset - backOffset) + camera.right.z * rightOffset - camera.up.z * downOffset
        };

        WeaponPose pose = {};
        pose.origin = origin;
        pose.right = camera.right;
        pose.up = weaponUp;
        pose.forward = weaponForward;
        return pose;
    }

    D3D12_RAYTRACING_INSTANCE_DESC buildWeaponInstanceDesc(const WeaponPose& pose) const {
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = pose.right.x * weaponMesh.scale;
        instanceDesc.Transform[0][1] = pose.up.x * weaponMesh.scale;
        instanceDesc.Transform[0][2] = pose.forward.x * weaponMesh.scale;
        instanceDesc.Transform[0][3] = pose.origin.x;
        instanceDesc.Transform[1][0] = pose.right.y * weaponMesh.scale;
        instanceDesc.Transform[1][1] = pose.up.y * weaponMesh.scale;
        instanceDesc.Transform[1][2] = pose.forward.y * weaponMesh.scale;
        instanceDesc.Transform[1][3] = pose.origin.y;
        instanceDesc.Transform[2][0] = pose.right.z * weaponMesh.scale;
        instanceDesc.Transform[2][1] = pose.up.z * weaponMesh.scale;
        instanceDesc.Transform[2][2] = pose.forward.z * weaponMesh.scale;
        instanceDesc.Transform[2][3] = pose.origin.z;
        instanceDesc.InstanceID = 1;
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.InstanceContributionToHitGroupIndex = 1;
        instanceDesc.AccelerationStructure = weaponMesh.blasResult->GetGPUVirtualAddress();
        return instanceDesc;
    }

    /// Pivot translation so the mesh centroid tracks the view ray; eases off when the bore points at the viewer.
    Vec3 inspectOriginForCenteredCentroid(const CameraState& camera, const WeaponPose& pose) const {
        const Vec3 f = v3norm(camera.forward);
        const Vec3 u = v3norm(camera.up);
        const Vec3 centroidW = weaponCentroidWorldOffset(weaponMesh.meshCentroidLocal, weaponMesh.scale,
            pose.right, pose.up, pose.forward);
        Vec3 anchor = {
            camera.position.x + f.x * WeaponInspectEyeToCentroidAlongView - u.x * WeaponInspectAnchorDownAlongUp,
            camera.position.y + f.y * WeaponInspectEyeToCentroidAlongView - u.y * WeaponInspectAnchorDownAlongUp,
            camera.position.z + f.z * WeaponInspectEyeToCentroidAlongView - u.z * WeaponInspectAnchorDownAlongUp,
        };

        const Vec3 b = v3norm(pose.forward);
        float threat = -(b.x * f.x + b.y * f.y + b.z * f.z); // +1 when bore anti-parallel to view
        threat = fmaxf(0.0f, fminf(1.0f, threat));

        const float t2 = threat * threat;
        const float centroidKeep = 1.0f - InspectBoreThreatCentroidRelax * t2;
        const float pushForward = InspectBoreThreatPushAlongView * t2;

        return {
            anchor.x - centroidW.x * centroidKeep + f.x * pushForward,
            anchor.y - centroidW.y * centroidKeep + f.y * pushForward,
            anchor.z - centroidW.z * centroidKeep + f.z * pushForward,
        };
    }

    D3D12_RAYTRACING_INSTANCE_DESC weaponInstanceDesc(const CameraState& camera) const {
        const WeaponPose idle = computeHandWeaponPose(camera);

        if (!weaponInspectPlaying) {
            return buildWeaponInstanceDesc(idle);
        }

        const float u = fminf(1.0f, weaponInspectT / WeaponInspectTotalSeconds);
        constexpr float kPi = 3.1415926535f;

        WeaponPose pose = idle;

        if (u <= InspectUpushEnd) {
            const float kp = smoothstep01(u / InspectUpushEnd);
            // Translation reaches full extent a touch before barrel finishes rolling to profile.
            const float kPos = smoothstep01(fminf(1.0f, u / (InspectUpushEnd * 0.78f)));
            const float kpRot = easeInspectSpinTurn01(kp);
            const Vec3 profileAim = inspectBarrelFacingFloated(camera, 0.0f, weaponInspectT);
            const Vec3 aimFwd = v3norm(v3lerp(idle.forward, profileAim, kpRot));
            pose = weaponPoseFromBarrel(aimFwd, camera.up);
            const Vec3 outOrigin = inspectOriginForCenteredCentroid(camera, pose);
            pose.origin = v3lerp(idle.origin, outOrigin, kPos);
            const Vec3 fUv = v3norm(camera.forward);
            const float bump = InspectHandoffOutwardAlongView * sinf(kpRot * 3.1415926535f);
            pose.origin = v3add(pose.origin, v3scale(fUv, bump));
        }
        else if (u <= InspectUSpinFwdEnd) {
            // Linear yaw through the horizontal arc; centroid helper handles bore-at-camera depth (no barrel-first lunge).
            const float raw = (u - InspectUpushEnd) / fmaxf(1.0e-6f, InspectUSpinFwdEnd - InspectUpushEnd);
            const float rawClamped = fminf(1.0f, fmaxf(0.0f, raw));
            const float yaw = rawClamped * kPi;
            const Vec3 fwd = inspectBarrelFacingFloated(camera, yaw, weaponInspectT);
            pose = weaponPoseFromBarrel(fwd, camera.up);
            pose.origin = inspectOriginForCenteredCentroid(camera, pose);
        }
        else if (u <= InspectUholdEnd) {
            pose = weaponPoseFromBarrel(inspectBarrelFacingFloated(camera, kPi, weaponInspectT), camera.up);
            pose.origin = inspectOriginForCenteredCentroid(camera, pose);
        }
        else if (u <= InspectUspinBackEnd) {
            const float raw = (u - InspectUholdEnd) / fmaxf(1.0e-6f, InspectUspinBackEnd - InspectUholdEnd);
            const float rawClamped = fminf(1.0f, fmaxf(0.0f, raw));
            const float yaw = (1.0f - rawClamped) * kPi;
            const Vec3 fwd = inspectBarrelFacingFloated(camera, yaw, weaponInspectT);
            pose = weaponPoseFromBarrel(fwd, camera.up);
            pose.origin = inspectOriginForCenteredCentroid(camera, pose);
        }
        else {
            const float kr = smoothstep01((u - InspectUspinBackEnd)
                / fmaxf(1.0e-6f, 1.0f - InspectUspinBackEnd));
            const float krRot = easeInspectSpinTurn01(kr);
            const Vec3 profileFloated =
                inspectBarrelFacingFloated(camera, 0.0f, weaponInspectT);
            const Vec3 aimFwd = v3norm(v3lerp(profileFloated, idle.forward, krRot));
            pose = weaponPoseFromBarrel(aimFwd, camera.up);
            const Vec3 screened = inspectOriginForCenteredCentroid(camera, pose);
            const float kHand =
                smoothstep01(fmaxf(0.0f, kr - 0.12f) / fmaxf(1.0e-6f, 0.88f));
            pose.origin = v3lerp(screened, idle.origin, kHand);
            const Vec3 fUv = v3norm(camera.forward);
            const float bump = InspectHandoffOutwardAlongView * sinf(krRot * 3.1415926535f);
            pose.origin = v3add(pose.origin, v3scale(fUv, bump));
        }

        pose.origin = v3add(pose.origin, weaponInspectHoldSwayOffset(camera, u, weaponInspectT));

        if (inspectReplayWarmBlend) {
            const float denom = InspectReplayWarmBlendSeconds;
            float ww = denom > 1.0e-6f ? inspectWarmBlendElapsed / denom : 1.0f;
            ww = fminf(1.0f, ww);
            const float kt = smoothstep01(ww);
            WeaponPose warmFrom = inspectReplayWarmFrom;
            if (inspectReplayWarmSnapValid) {
                warmFrom = inspectWarmPoseRebasedToCamera(inspectReplayWarmFrom, inspectReplayWarmSnapEye,
                    inspectReplayWarmSnapRight, inspectReplayWarmSnapUp, inspectReplayWarmSnapForward,
                    camera.position, v3norm(camera.right), v3norm(camera.up), v3norm(camera.forward),
                    camera.up);
            }
            pose = inspectPoseBlendRebuild(warmFrom, pose, kt, camera.up);
            if (inspectWarmBlendElapsed >= InspectReplayWarmBlendSeconds) {
                inspectReplayWarmBlend = false;
            }
        }

        inspectLastEmittedInspectPose = pose;
        inspectLastEmittedCamEye = camera.position;
        inspectLastEmittedCamRight = v3norm(camera.right);
        inspectLastEmittedCamUp = v3norm(camera.up);
        inspectLastEmittedCamForward = v3norm(camera.forward);
        inspectLastEmittedCamKnown = true;

        return buildWeaponInstanceDesc(pose);
    }

    void writeInstanceBuffer(const CameraState& camera) {
        const D3D12_RAYTRACING_INSTANCE_DESC instances[] = {
            sphereInstanceDesc(),
            weaponInstanceDesc(camera)
        };
        uploadToResource(instanceBuffer.Get(), instances, sizeof(instances));
    }

    /// Rebuild sphere BLAS from `aabbBuffer` (must match current `sceneSpheres`). Called while `commandList` is recording.
    void rebuildSphereBlas() {
        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometryDesc.AABBs.AABBCount = sphereCount;
        geometryDesc.AABBs.AABBs.StartAddress = aabbBuffer->GetGPUVirtualAddress();
        geometryDesc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.NumDescs = 1;
        blasInputs.pGeometryDescs = &geometryDesc;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
        blasDesc.Inputs = blasInputs;
        blasDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
        blasDesc.DestAccelerationStructureData = blasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
        D3D12_RESOURCE_BARRIER blasBarrier = uavBarrier(blasResult.Get());
        commandList->ResourceBarrier(1, &blasBarrier);
    }

    void uploadSceneIfNeeded() {
        if (!sceneDirty) {
            return;
        }

        uploadToResource(sphereBuffer.Get(), sceneSpheres.data(), sizeof(Sphere) * sceneSpheres.size());

        std::vector<D3D12_RAYTRACING_AABB> aabbs;
        aabbs.reserve(sceneSpheres.size());
        for (const Sphere& sphere : sceneSpheres) {
            const float boundsRadius = sphere.radius > 0.0001f ? sphere.radius : 6.0f;
            aabbs.push_back({
                sphere.center.x - boundsRadius,
                sphere.center.y - boundsRadius,
                sphere.center.z - boundsRadius,
                sphere.center.x + boundsRadius,
                sphere.center.y + boundsRadius,
                sphere.center.z + boundsRadius
            });
        }
        uploadToResource(aabbBuffer.Get(), aabbs.data(), sizeof(D3D12_RAYTRACING_AABB) * aabbs.size());

        rebuildSphereBlas();

        sceneDirty = false;
    }

    float beginFrameSimulation() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastSimulationTick).count();
        lastSimulationTick = now;
        if (dt < 0.0f) {
            dt = 0.0f;
        }
        if (dt > 0.05f) {
            dt = 0.05f;
        }
        return dt;
    }

    void updateFragmentPhysics(float dt) {
        if (dt <= 0.0f) {
            return;
        }

        bool moved = false;
        for (size_t i = 0; i < sceneSpheres.size(); ++i) {
            if (!activeShards[i] || sceneSpheres[i].radius <= 0.0001f) {
                continue;
            }

            shardVelocities[i].y -= 9.8f * dt;
            sceneSpheres[i].center.x += shardVelocities[i].x * dt;
            sceneSpheres[i].center.y += shardVelocities[i].y * dt;
            sceneSpheres[i].center.z += shardVelocities[i].z * dt;

            const float floorY = sceneSpheres[i].radius;
            if (sceneSpheres[i].center.y < floorY) {
                // Despawn on floor contact — shards never rest on the ground.
                activeShards[i] = 0;
                sceneSpheres[i].radius = 0.0f;
                shardVelocities[i] = { 0.0f, 0.0f, 0.0f };
            }

            moved = true;
        }

        if (moved) {
            sceneDirty = true;
        }
    }

    Vec3 randomOrbSpawnPosition(size_t respawningTargetSlot) {
        constexpr float MinCenterSeparation = TargetOrbRadius * 2.0f + 0.45f;
        constexpr float MinCenterSeparationSq = MinCenterSeparation * MinCenterSeparation;

        std::uniform_real_distribution<float> distX(OrbRespawnMinX, OrbRespawnMaxX);
        std::uniform_real_distribution<float> distY(OrbRespawnMinY, OrbRespawnMaxY);
        std::uniform_real_distribution<float> distZ(OrbRespawnMinZ, OrbRespawnMaxZ);

        for (int attempt = 0; attempt < 56; ++attempt) {
            Vec3 candidate = { distX(orbRespawnRng), distY(orbRespawnRng), distZ(orbRespawnRng) };

            bool clear = true;
            for (size_t j = 0; j < targetSphereIndices.size(); ++j) {
                if (j == respawningTargetSlot) {
                    continue;
                }

                const UINT otherOrb = targetSphereIndices[j];
                if (sceneSpheres[otherOrb].radius <= 0.0001f) {
                    continue;
                }

                const float dx = candidate.x - sceneSpheres[otherOrb].center.x;
                const float dy = candidate.y - sceneSpheres[otherOrb].center.y;
                const float dz = candidate.z - sceneSpheres[otherOrb].center.z;
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq < MinCenterSeparationSq) {
                    clear = false;
                    break;
                }
            }

            if (clear) {
                return candidate;
            }
        }

        const UINT orbIndex = targetSphereIndices[respawningTargetSlot];
        return sphereRestCenters[orbIndex];
    }

    void updateTargetOrbRegen(float dt) {
        if (dt <= 0.0f) {
            return;
        }

        bool changed = false;
        for (size_t ti = 0; ti < targetSphereIndices.size(); ++ti) {
            float& remaining = targetOrbRegenRemaining[ti];
            if (remaining <= 0.0f) {
                continue;
            }

            remaining -= dt;
            if (remaining > 0.0f) {
                continue;
            }

            remaining = 0.0f;
            const UINT orbIndex = targetSphereIndices[ti];
            const Vec3 newCenter = randomOrbSpawnPosition(ti);

            sceneSpheres[orbIndex].center = newCenter;
            sphereRestCenters[orbIndex] = newCenter;
            sceneSpheres[orbIndex].radius = TargetOrbRadius;

            for (UINT i = 1; i <= ShardsPerTarget && orbIndex + i < sceneSpheres.size(); ++i) {
                const UINT shardIndex = orbIndex + i;
                const Vec3 offset = kShardLocalOffsets[i - 1];
                const Vec3 shardCenter = {
                    newCenter.x + offset.x,
                    newCenter.y + offset.y,
                    newCenter.z + offset.z
                };
                activeShards[shardIndex] = 0;
                sceneSpheres[shardIndex].radius = 0.0f;
                shardVelocities[shardIndex] = { 0.0f, 0.0f, 0.0f };
                sceneSpheres[shardIndex].center = shardCenter;
                sphereRestCenters[shardIndex] = shardCenter;
            }

            changed = true;
        }

        if (changed) {
            sceneDirty = true;
        }
    }

    void updateWeaponInspect(float dt) {
        if (!weaponInspectPlaying) {
            return;
        }

        weaponInspectT += dt;
        if (inspectReplayWarmBlend) {
            inspectWarmBlendElapsed += dt;
        }
        if (weaponInspectT >= WeaponInspectTotalSeconds) {
            weaponInspectPlaying = false;
            weaponInspectT = 0.0f;
            inspectReplayWarmBlend = false;
        }
    }

    void beginWeaponInspect() {
        if (weaponInspectPlaying) {
            inspectReplayWarmFrom = inspectLastEmittedInspectPose;
            inspectReplayWarmSnapEye = inspectLastEmittedCamEye;
            inspectReplayWarmSnapRight = inspectLastEmittedCamRight;
            inspectReplayWarmSnapUp = inspectLastEmittedCamUp;
            inspectReplayWarmSnapForward = inspectLastEmittedCamForward;
            inspectReplayWarmSnapValid =
                inspectLastEmittedCamKnown && v3len(inspectReplayWarmSnapForward) > 0.5f
                && v3len(inspectReplayWarmSnapRight) > 0.5f && v3len(inspectReplayWarmSnapUp) > 0.5f;
            inspectReplayWarmBlend = inspectReplayWarmSnapValid;
            inspectWarmBlendElapsed = 0.0f;
        }
        else {
            inspectReplayWarmBlend = false;
            inspectReplayWarmSnapValid = false;
        }

        weaponInspectPlaying = true;
        weaponInspectT = 0.0f;
    }

    void updateWeaponRecoil(float dt) {
        if (weaponRecoilTime >= 0.34f) {
            return;
        }

        weaponRecoilTime += dt;
    }

    static Vec3 sub(Vec3 a, Vec3 b) {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    static float dot(Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static bool rayHitsSphere(Vec3 origin, Vec3 direction, const Sphere& sphere, float& t) {
        if (sphere.radius <= 0.0001f) {
            return false;
        }

        Vec3 oc = sub(origin, sphere.center);
        float b = dot(oc, direction);
        float c = dot(oc, oc) - sphere.radius * sphere.radius;
        float disc = b * b - c;
        if (disc < 0.0f) {
            return false;
        }

        float sqrtDisc = sqrtf(disc);
        float t0 = -b - sqrtDisc;
        float t1 = -b + sqrtDisc;
        if (t0 > 0.001f) {
            t = t0;
            return true;
        }
        if (t1 > 0.001f) {
            t = t1;
            return true;
        }
        return false;
    }

    bool shoot(const CameraState& camera) {
        weaponInspectPlaying = false;
        weaponInspectT = 0.0f;
        inspectReplayWarmBlend = false;
        weaponRecoilTime = 0.0f;

        float closest = 1.0e30f;
        UINT hitTargetIndex = UINT_MAX;
        UINT hitShardIndex = UINT_MAX;

        for (UINT targetIndex : targetSphereIndices) {
            float t = 0.0f;
            if (rayHitsSphere(camera.position, camera.forward, sceneSpheres[targetIndex], t) && t < closest) {
                closest = t;
                hitTargetIndex = targetIndex;
                hitShardIndex = UINT_MAX;
            }
        }

        for (UINT shardIndex = 0; shardIndex < sceneSpheres.size(); ++shardIndex) {
            if (!activeShards[shardIndex]) {
                continue;
            }

            float t = 0.0f;
            if (rayHitsSphere(camera.position, camera.forward, sceneSpheres[shardIndex], t) && t < closest) {
                closest = t;
                hitTargetIndex = UINT_MAX;
                hitShardIndex = shardIndex;
            }
        }

        if (hitShardIndex != UINT_MAX) {
            shardVelocities[hitShardIndex].x += camera.forward.x * 3.2f + camera.up.x * 0.8f;
            shardVelocities[hitShardIndex].y += camera.forward.y * 3.2f + camera.up.y * 0.8f;
            shardVelocities[hitShardIndex].z += camera.forward.z * 3.2f + camera.up.z * 0.8f;
            sceneDirty = true;
            return true;
        }

        if (hitTargetIndex == UINT_MAX) {
            return false;
        }

        sceneSpheres[hitTargetIndex].radius = 0.0f;
        for (size_t ti = 0; ti < targetSphereIndices.size(); ++ti) {
            if (targetSphereIndices[ti] == hitTargetIndex) {
                targetOrbRegenRemaining[ti] = TargetOrbRegenSeconds;
                break;
            }
        }
        killBannerActive = true;
        killBannerElapsedSeconds = 0.0f;
        const Vec3 velocities[] = {
            { 4.7f, 2.6f, 0.7f },
            {-3.9f, 2.1f,-0.8f },
            { 0.9f, 5.2f, 0.3f },
            {-0.6f, 1.1f, 0.9f },
            { 1.2f, 3.3f, 4.1f },
            {-1.4f, 2.9f,-3.8f },
            { 3.1f, 4.0f, 2.0f },
            {-3.5f, 3.6f,-1.7f },
            { 2.8f, 1.8f,-2.5f },
            {-2.6f, 2.4f, 2.8f },
            { 0.4f, 4.8f,-3.2f },
            {-0.7f, 3.9f, 3.0f }
        };

        const float jitter = 0.85f + static_cast<float>((hitTargetIndex * 37u) % 31u) / 100.0f;
        for (UINT i = 1; i <= ShardsPerTarget && hitTargetIndex + i < sceneSpheres.size(); ++i) {
            const UINT shardIndex = hitTargetIndex + i;
            sceneSpheres[shardIndex].radius = shardActiveRadii[shardIndex];
            shardVelocities[shardIndex] = {
                velocities[i - 1].x * jitter,
                velocities[i - 1].y * (1.08f - jitter * 0.08f),
                velocities[i - 1].z * (1.18f - jitter * 0.12f)
            };
            activeShards[shardIndex] = 1;
        }
        sceneDirty = true;
        return true;
    }

    void createAccelerationStructures() {
        commandAllocators[frameIndex]->Reset();
        commandList->Reset(commandAllocators[frameIndex].Get(), nullptr);

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometryDesc.AABBs.AABBCount = sphereCount;
        geometryDesc.AABBs.AABBs.StartAddress = aabbBuffer->GetGPUVirtualAddress();
        geometryDesc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.NumDescs = 1;
        blasInputs.pGeometryDescs = &geometryDesc;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasInfo);
        blasScratch = createBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        blasResult = createBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        rebuildSphereBlas();

        buildWeaponBlas(weaponMesh);

        instanceBuffer = createBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 2,
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        const CameraState initialCamera = {
            {0.0f, 1.7f, 4.0f},
            {0.0f, 0.0f, -1.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            80.0f * 3.1415926535f / 180.0f
        };
        writeInstanceBuffer(initialCamera);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.NumDescs = 2;
        tlasInputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
        tlasScratch = createBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        tlasResult = createBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
        tlasDesc.Inputs = tlasInputs;
        tlasDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        tlasDesc.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
        auto tlasBarrier = uavBarrier(tlasResult.Get());
        commandList->ResourceBarrier(1, &tlasBarrier);

        commandList->Close();
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
        waitForGpu();

        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc = {};
        tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrvDesc.RaytracingAccelerationStructure.Location = tlasResult->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &tlasSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, srvUavDescriptorSize));
    }

    void buildWeaponBlas(WeaponGpuMesh& weapon) {
        D3D12_RAYTRACING_GEOMETRY_DESC weaponGeometryDesc = {};
        weaponGeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        weaponGeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        weaponGeometryDesc.Triangles.VertexBuffer.StartAddress = weapon.vertexBuffer->GetGPUVirtualAddress();
        weaponGeometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vec3);
        weaponGeometryDesc.Triangles.VertexCount = weapon.vertexCount;
        weaponGeometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        weaponGeometryDesc.Triangles.IndexBuffer = weapon.indexBuffer->GetGPUVirtualAddress();
        weaponGeometryDesc.Triangles.IndexCount = weapon.indexCount;
        weaponGeometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS weaponBlasInputs = {};
        weaponBlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        weaponBlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        weaponBlasInputs.NumDescs = 1;
        weaponBlasInputs.pGeometryDescs = &weaponGeometryDesc;
        weaponBlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO weaponBlasInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&weaponBlasInputs, &weaponBlasInfo);
        weapon.blasScratch = createBuffer(weaponBlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        weapon.blasResult = createBuffer(weaponBlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC weaponBlasDesc = {};
        weaponBlasDesc.Inputs = weaponBlasInputs;
        weaponBlasDesc.ScratchAccelerationStructureData = weapon.blasScratch->GetGPUVirtualAddress();
        weaponBlasDesc.DestAccelerationStructureData = weapon.blasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&weaponBlasDesc, 0, nullptr);
        auto weaponBlasBarrier = uavBarrier(weapon.blasResult.Get());
        commandList->ResourceBarrier(1, &weaponBlasBarrier);
    }

    void createGlobalRootSignature() {
        D3D12_DESCRIPTOR_RANGE ranges[5] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = 0;

        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[3].NumDescriptors = 1;
        ranges[3].BaseShaderRegister = 2;
        ranges[3].OffsetInDescriptorsFromTableStart = 0;

        ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[4].NumDescriptors = 1;
        ranges[4].BaseShaderRegister = 3;
        ranges[4].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[6] = {};
        for (UINT i = 0; i < 5; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        }
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[5].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(UINT);
        params[5].Constants.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(params);
        desc.pParameters = params;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> errors;
        throwIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors),
            "Failed to serialize root signature");
        throwIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&globalRootSignature)), "Failed to create root signature");
    }

    void createStateObject() {
        ComPtr<IDxcBlob> dxilLibrary = compileRayTracingLibrary();

        const wchar_t* rayGen = L"RayGen";
        const wchar_t* miss = L"Miss";
        const wchar_t* closestHit = L"ClosestHit";
        const wchar_t* weaponClosestHit = L"WeaponClosestHit";
        const wchar_t* intersection = L"SphereIntersection";
        const wchar_t* hitGroup = L"SphereHitGroup";
        const wchar_t* weaponHitGroup = L"WeaponHitGroup";

        D3D12_EXPORT_DESC exports[] = {
            { rayGen, nullptr, D3D12_EXPORT_FLAG_NONE },
            { miss, nullptr, D3D12_EXPORT_FLAG_NONE },
            { closestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
            { weaponClosestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
            { intersection, nullptr, D3D12_EXPORT_FLAG_NONE }
        };

        D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
        libraryDesc.DXILLibrary.BytecodeLength = dxilLibrary->GetBufferSize();
        libraryDesc.DXILLibrary.pShaderBytecode = dxilLibrary->GetBufferPointer();
        libraryDesc.NumExports = _countof(exports);
        libraryDesc.pExports = exports;

        D3D12_STATE_SUBOBJECT librarySubobject = {};
        librarySubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        librarySubobject.pDesc = &libraryDesc;

        D3D12_HIT_GROUP_DESC hitGroupDesc = {};
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hitGroupDesc.HitGroupExport = hitGroup;
        hitGroupDesc.ClosestHitShaderImport = closestHit;
        hitGroupDesc.IntersectionShaderImport = intersection;

        D3D12_STATE_SUBOBJECT hitGroupSubobject = {};
        hitGroupSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        hitGroupSubobject.pDesc = &hitGroupDesc;

        D3D12_HIT_GROUP_DESC weaponHitGroupDesc = {};
        weaponHitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        weaponHitGroupDesc.HitGroupExport = weaponHitGroup;
        weaponHitGroupDesc.ClosestHitShaderImport = weaponClosestHit;

        D3D12_STATE_SUBOBJECT weaponHitGroupSubobject = {};
        weaponHitGroupSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        weaponHitGroupSubobject.pDesc = &weaponHitGroupDesc;

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
        shaderConfig.MaxPayloadSizeInBytes = 32;
        shaderConfig.MaxAttributeSizeInBytes = 16;

        D3D12_STATE_SUBOBJECT shaderConfigSubobject = {};
        shaderConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        shaderConfigSubobject.pDesc = &shaderConfig;

        D3D12_GLOBAL_ROOT_SIGNATURE globalRoot = {};
        globalRoot.pGlobalRootSignature = globalRootSignature.Get();

        D3D12_STATE_SUBOBJECT rootSignatureSubobject = {};
        rootSignatureSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        rootSignatureSubobject.pDesc = &globalRoot;

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
        pipelineConfig.MaxTraceRecursionDepth = 6;

        D3D12_STATE_SUBOBJECT pipelineConfigSubobject = {};
        pipelineConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        pipelineConfigSubobject.pDesc = &pipelineConfig;

        D3D12_STATE_SUBOBJECT subobjects[] = {
            librarySubobject,
            hitGroupSubobject,
            weaponHitGroupSubobject,
            shaderConfigSubobject,
            rootSignatureSubobject,
            pipelineConfigSubobject
        };

        D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = _countof(subobjects);
        stateObjectDesc.pSubobjects = subobjects;

        throwIfFailed(device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&stateObject)),
            "Failed to create DXR state object");
        throwIfFailed(stateObject.As(&stateObjectProps), "Failed to query DXR state object properties");
    }

    void createShaderTable() {
        shaderRecordSize = static_cast<UINT>(alignTo(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));

        rayGenOffset = 0;
        missOffset = alignTo(rayGenOffset + shaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        hitGroupOffset = alignTo(missOffset + shaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT64 hitGroupTableSize = shaderRecordSize * 2;
        const UINT64 tableSize = alignTo(hitGroupOffset + hitGroupTableSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        shaderTable = createBuffer(tableSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        throwIfFailed(shaderTable->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Failed to map shader table");
        memcpy(mapped + rayGenOffset, stateObjectProps->GetShaderIdentifier(L"RayGen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        memcpy(mapped + missOffset, stateObjectProps->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        memcpy(mapped + hitGroupOffset, stateObjectProps->GetShaderIdentifier(L"SphereHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        memcpy(mapped + hitGroupOffset + shaderRecordSize, stateObjectProps->GetShaderIdentifier(L"WeaponHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        shaderTable->Unmap(0, nullptr);
    }

    void createKillBannerGpuTextures() {
        D3D12_RESOURCE_DESC scratchDesc = outputTexture->GetDesc();
        scratchDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        throwIfFailed(device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&killScratchTexture)), "Kill scratch tex failed");

        D3D12_SHADER_RESOURCE_VIEW_DESC scratchSrvDesc = {};
        scratchSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        scratchSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scratchSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        scratchSrvDesc.Texture2D.MostDetailedMip = 0;
        scratchSrvDesc.Texture2D.MipLevels = 1;
        scratchSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(killScratchTexture.Get(), &scratchSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 6, srvUavDescriptorSize));

        const CpuRgbaImage banCpu = loadPngRgbaViaWic(findBannerPngPath());

        D3D12_RESOURCE_DESC imgDesc = {};
        imgDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        imgDesc.Alignment = 0;
        imgDesc.DepthOrArraySize = 1;
        imgDesc.MipLevels = 1;
        imgDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        imgDesc.SampleDesc.Count = 1;
        imgDesc.SampleDesc.Quality = 0;
        imgDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        imgDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RESOURCE_DESC bd = imgDesc;
        bd.Width = banCpu.pixelWidth;
        bd.Height = banCpu.pixelHeight;

        throwIfFailed(device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bannerTextureGpu)),
            "Banner GPU tex failed");
        bannerTextureW = banCpu.pixelWidth;
        bannerTextureH = banCpu.pixelHeight;

        const UINT pitchB = alignTextureRowPitch(banCpu.pixelWidth * 4u);
        const UINT64 uploadSzB =
            static_cast<UINT64>(pitchB) * static_cast<UINT64>(banCpu.pixelHeight);

        ComPtr<ID3D12Resource> stagingBanner =
            createBuffer(uploadSzB, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        auto copyImage =
            [&](const CpuRgbaImage& cpu, ID3D12Resource* stagingUpload, ID3D12Resource* dstGpu, UINT pitch, UINT gw, UINT gh) {
                void* mapped = nullptr;
                D3D12_RANGE mr = {};
                throwIfFailed(stagingUpload->Map(0, &mr, &mapped), "staging map");
                memcpy(mapped, cpu.pixels.data(), static_cast<size_t>(pitch) * static_cast<size_t>(gh));
                stagingUpload->Unmap(0, nullptr);

                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLoc.SubresourceIndex = 0;
                dstLoc.pResource = dstGpu;

                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLoc.pResource = stagingUpload;
                srcLoc.PlacedFootprint.Offset = 0;
                srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srcLoc.PlacedFootprint.Footprint.Width = gw;
                srcLoc.PlacedFootprint.Footprint.Height = gh;
                srcLoc.PlacedFootprint.Footprint.Depth = 1;
                srcLoc.PlacedFootprint.Footprint.RowPitch = pitch;

                commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                auto b =
                    transition(dstGpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1, &b);
            };

        commandAllocators[0]->Reset();
        throwIfFailed(commandList->Reset(commandAllocators[0].Get(), nullptr), "Reset banner upload CL");
        copyImage(banCpu, stagingBanner.Get(), bannerTextureGpu.Get(), pitchB, bannerTextureW, bannerTextureH);

        commandList->Close();
        ID3D12CommandList* run[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, run);
        waitForGpu();

        D3D12_SHADER_RESOURCE_VIEW_DESC imgSrvDesc = {};
        imgSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        imgSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        imgSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        imgSrvDesc.Texture2D.MostDetailedMip = 0;
        imgSrvDesc.Texture2D.MipLevels = 1;
        imgSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        device->CreateShaderResourceView(bannerTextureGpu.Get(), &imgSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 5, srvUavDescriptorSize));
    }

    void createKillBannerOverlayCompute() {
        D3D12_DESCRIPTOR_RANGE rangeSrcScratch = {};
        rangeSrcScratch.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeSrcScratch.NumDescriptors = 1;
        rangeSrcScratch.BaseShaderRegister = 0;
        rangeSrcScratch.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE rangeSprite = {};
        rangeSprite.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeSprite.NumDescriptors = 1;
        rangeSprite.BaseShaderRegister = 1;
        rangeSprite.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE rangeDstUav = {};
        rangeDstUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rangeDstUav.NumDescriptors = 1;
        rangeDstUav.BaseShaderRegister = 0;
        rangeDstUav.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[4] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &rangeSrcScratch;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &rangeSprite;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &rangeDstUav;

        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].Constants.Num32BitValues = 8;
        params[3].Constants.ShaderRegister = 0;
        params[3].Constants.RegisterSpace = 0;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof(params);
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        rsDesc.NumStaticSamplers = 0;
        rsDesc.pStaticSamplers = nullptr;

        ComPtr<ID3DBlob> sigBlob;
        ComPtr<ID3DBlob> sigErrors;
        throwIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErrors),
            "Kill overlay SIG failed");
        throwIfFailed(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
            IID_PPV_ARGS(&killOverlayRootSignature)), "Kill overlay root sig failed");

        ComPtr<IDxcBlob> shaderBlob = compileOverlayComputeLibrary();
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = killOverlayRootSignature.Get();
        computePsoDesc.CS.pShaderBytecode = shaderBlob->GetBufferPointer();
        computePsoDesc.CS.BytecodeLength = shaderBlob->GetBufferSize();

        throwIfFailed(device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&killOverlayPso)),
            "Kill overlay PSO failed");
    }

    void createNoirOverlayCompute() {
        D3D12_DESCRIPTOR_RANGE rangeSrc = {};
        rangeSrc.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeSrc.NumDescriptors = 1;
        rangeSrc.BaseShaderRegister = 0;
        rangeSrc.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE rangeDstUav = {};
        rangeDstUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rangeDstUav.NumDescriptors = 1;
        rangeDstUav.BaseShaderRegister = 0;
        rangeDstUav.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &rangeSrc;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &rangeDstUav;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].Constants.Num32BitValues = 2;
        params[2].Constants.ShaderRegister = 0;
        params[2].Constants.RegisterSpace = 0;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof(params);
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        rsDesc.NumStaticSamplers = 0;
        rsDesc.pStaticSamplers = nullptr;

        ComPtr<ID3DBlob> sigBlob;
        ComPtr<ID3DBlob> sigErrors;
        throwIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErrors),
            "Noir overlay SIG failed");
        throwIfFailed(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
            IID_PPV_ARGS(&noirOverlayRootSignature)), "Noir overlay root sig failed");

        ComPtr<IDxcBlob> shaderBlob = compileNoirComputeLibrary();
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = noirOverlayRootSignature.Get();
        computePsoDesc.CS.pShaderBytecode = shaderBlob->GetBufferPointer();
        computePsoDesc.CS.BytecodeLength = shaderBlob->GetBufferSize();

        throwIfFailed(device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&noirOverlayPso)),
            "Noir overlay PSO failed");
    }

    void compositeNoirGrade() {
        if (!noirOverlayPso || !killScratchTexture || !outputTexture) {
            return;
        }

        auto copySrcBarrier =
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1, &copySrcBarrier);
        commandList->CopyResource(killScratchTexture.Get(), outputTexture.Get());

        D3D12_RESOURCE_BARRIER midBarriers[] = {
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(killScratchTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(_countof(midBarriers), midBarriers);

        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(noirOverlayRootSignature.Get());
        commandList->SetComputeRootDescriptorTable(0,
            offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 6, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(1,
            offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));

        const float noirKv[2] = {
            static_cast<float>(width),
            static_cast<float>(height),
        };
        commandList->SetComputeRoot32BitConstants(2, _countof(noirKv), noirKv, 0);

        commandList->SetPipelineState(noirOverlayPso.Get());
        const UINT gx = (width + 7u) / 8u;
        const UINT gy = (height + 7u) / 8u;
        commandList->Dispatch(gx, gy, 1);

        auto scratchRestore =
            transition(killScratchTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &scratchRestore);
    }

    void updateKillBannerTimer(float dt) {
        if (!killBannerActive) {
            return;
        }
        killBannerElapsedSeconds += dt;
        if (killBannerElapsedSeconds >= KillBannerTotalSeconds) {
            killBannerActive = false;
        }
    }

    static void killOverlayAspectHalfExtents(UINT tw, UINT th, float screenSpan, float& outHalfW, float& outHalfH) {
        float halfW = screenSpan * 0.5f;
        float halfH = screenSpan * 0.5f;
        const float texAspect = static_cast<float>(tw) / static_cast<float>((std::max)(1u, th));
        const float invAspect = static_cast<float>(th) / static_cast<float>((std::max)(1u, tw));
        if (tw >= th) {
            halfH = (screenSpan * invAspect) * 0.5f;
        }
        else {
            halfW = (screenSpan * texAspect) * 0.5f;
        }
        outHalfW = halfW;
        outHalfH = halfH;
    }

    static float killBannerToneStrength(float elapsed) {
        constexpr float FadeIn = KillBannerFadeInSeconds;
        constexpr float FadeOut = KillBannerFadeOutSeconds;

        float e = 1.0f;
        if (FadeIn > 1.0e-5f && elapsed < FadeIn) {
            const float ti = (std::clamp)(elapsed / FadeIn, 0.0f, 1.0f);
            const float easedIn = ti * ti * (3.0f - 2.0f * ti);
            e *= easedIn;
        }
        if (FadeOut > 1.0e-5f && elapsed > KillBannerTotalSeconds - FadeOut) {
            const float to = (std::clamp)((KillBannerTotalSeconds - elapsed) / FadeOut, 0.0f, 1.0f);
            const float easedOut = to * to * (3.0f - 2.0f * to);
            e *= easedOut;
        }
        e = (std::clamp)(e, 0.0f, 1.0f);
        return KillBannerToneStrengthMin + e * (KillBannerToneStrengthMax - KillBannerToneStrengthMin);
    }

    static float killBannerExpandMul(float elapsed) {
        constexpr float Ramp = KillBannerExpandRampSeconds;
        const float tt = (Ramp > 1.0e-5f) ? (std::clamp)(elapsed / Ramp, 0.0f, 1.0f) : 1.0f;
        const float sm = tt * tt * (3.0f - 2.0f * tt);
        return KillBannerExpandScaleStart + sm * (KillBannerExpandScalePeak - KillBannerExpandScaleStart);
    }

    void compositeKillBannerIfActive() {
        if (!killBannerActive || !bannerTextureGpu || !killOverlayPso) {
            return;
        }

        const float cx = static_cast<float>(width) * 0.5f;
        const float cy = static_cast<float>(height) * (0.5f + KillOverlayCenterDownFracOfHeight);
        const float minDim = static_cast<float>((std::min)(width, height));

        float banHalfW = 1.0f;
        float banHalfH = 1.0f;
        const float spanScale = KillBannerSpriteFrac * killBannerExpandMul(killBannerElapsedSeconds);
        killOverlayAspectHalfExtents(bannerTextureW, bannerTextureH, spanScale * minDim, banHalfW, banHalfH);

        const float bannerStrength = killBannerToneStrength(killBannerElapsedSeconds);

        D3D12_RESOURCE_BARRIER preSync =
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1, &preSync);
        commandList->CopyResource(killScratchTexture.Get(), outputTexture.Get());

        D3D12_RESOURCE_BARRIER midSync[] = {
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(killScratchTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(_countof(midSync), midSync);

        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(killOverlayRootSignature.Get());
        commandList->SetComputeRootDescriptorTable(0,
            offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 6, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(2,
            offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));
        commandList->SetPipelineState(killOverlayPso.Get());

        const UINT gx = (width + 7u) / 8u;
        const UINT gy = (height + 7u) / 8u;

        const float kv[8] = {
            static_cast<float>(width),
            static_cast<float>(height),
            cx,
            cy,
            banHalfW,
            banHalfH,
            bannerStrength,
            KillBannerAlphaCapMax,
        };
        commandList->SetComputeRootDescriptorTable(1,
            offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 5, srvUavDescriptorSize));
        commandList->SetComputeRoot32BitConstants(3, static_cast<UINT>(_countof(kv)), kv, 0);
        commandList->Dispatch(gx, gy, 1);

        D3D12_RESOURCE_BARRIER postSync =
            transition(killScratchTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &postSync);
    }

    void render(const CameraState& camera) {
        float dt = beginFrameSimulation();
        updateKillBannerTimer(dt);
        updateWeaponInspect(dt);
        updateFragmentPhysics(dt);
        updateTargetOrbRegen(dt);
        updateWeaponRecoil(dt);

        commandAllocators[frameIndex]->Reset();
        commandList->Reset(commandAllocators[frameIndex].Get(), nullptr);

        uploadSceneIfNeeded();
        writeInstanceBuffer(camera);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.NumDescs = 2;
        tlasInputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
        tlasDesc.Inputs = tlasInputs;
        tlasDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        tlasDesc.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
        auto tlasBarrier = uavBarrier(tlasResult.Get());
        commandList->ResourceBarrier(1, &tlasBarrier);

        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(globalRootSignature.Get());
        commandList->SetComputeRootDescriptorTable(0, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(1, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 1, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(2, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 2, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(3, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 3, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(4, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 4, srvUavDescriptorSize));
        const FrameConstants constants = {
            width,
            height,
            frameIndex,
            sphereCount,
            camera.position,
            camera.fovYRadians,
            camera.forward,
            0.0f,
            camera.right,
            0.0f,
            camera.up,
            0.0f
        };
        commandList->SetComputeRoot32BitConstants(5, sizeof(constants) / sizeof(UINT), &constants, 0);

        commandList->SetPipelineState1(stateObject.Get());

        D3D12_DISPATCH_RAYS_DESC dispatch = {};
        const D3D12_GPU_VIRTUAL_ADDRESS tableStart = shaderTable->GetGPUVirtualAddress();
        dispatch.RayGenerationShaderRecord.StartAddress = tableStart + rayGenOffset;
        dispatch.RayGenerationShaderRecord.SizeInBytes = shaderRecordSize;
        dispatch.MissShaderTable.StartAddress = tableStart + missOffset;
        dispatch.MissShaderTable.SizeInBytes = shaderRecordSize;
        dispatch.MissShaderTable.StrideInBytes = shaderRecordSize;
        dispatch.HitGroupTable.StartAddress = tableStart + hitGroupOffset;
        dispatch.HitGroupTable.SizeInBytes = shaderRecordSize * 2;
        dispatch.HitGroupTable.StrideInBytes = shaderRecordSize;
        dispatch.Width = width;
        dispatch.Height = height;
        dispatch.Depth = 1;
        commandList->DispatchRays(&dispatch);

        D3D12_RESOURCE_BARRIER rtOutUavFlush = {};
        rtOutUavFlush.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rtOutUavFlush.UAV.pResource = outputTexture.Get();
        commandList->ResourceBarrier(1, &rtOutUavFlush);

        compositeKillBannerIfActive();
        compositeNoirGrade();

        auto outputUav = uavBarrier(outputTexture.Get());
        commandList->ResourceBarrier(1, &outputUav);

        D3D12_RESOURCE_BARRIER copyBarriers[] = {
            transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        commandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
        commandList->CopyResource(renderTargets[frameIndex].Get(), outputTexture.Get());

        D3D12_RESOURCE_BARRIER presentBarriers[] = {
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
        };
        commandList->ResourceBarrier(_countof(presentBarriers), presentBarriers);

        commandList->Close();
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);

        throwIfFailed(swapChain->Present(1, 0), "Swap-chain present failed");
        moveToNextFrame();
    }

    void waitForGpu() {
        const UINT64 value = ++fenceValue;
        throwIfFailed(commandQueue->Signal(fence.Get(), value), "Failed to signal fence");
        if (fence->GetCompletedValue() < value) {
            throwIfFailed(fence->SetEventOnCompletion(value, fenceEvent), "Failed to set fence completion event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    void moveToNextFrame() {
        const UINT64 value = ++fenceValue;
        throwIfFailed(commandQueue->Signal(fence.Get(), value), "Failed to signal frame fence");
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < value) {
            throwIfFailed(fence->SetEventOnCompletion(value, fenceEvent), "Failed to set frame fence event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    void shutdown() {
        if (device && commandQueue && fence && fenceEvent) {
            waitForGpu();
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
    }
};

DxrRenderer::DxrRenderer() : impl(new Impl()) {}

DxrRenderer::~DxrRenderer() {
    shutdown();
    delete impl;
    impl = nullptr;
}

bool DxrRenderer::initialize(HWND hwnd, int width, int height) {
    try {
        impl->initialize(hwnd, width, height);
        return true;
    }
    catch (const std::exception& e) {
        impl->lastError.assign(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

void DxrRenderer::render(const CameraState& camera) {
    try {
        impl->render(camera);
    }
    catch (const std::exception& e) {
        impl->lastError.assign(e.what(), e.what() + strlen(e.what()));
    }
}

bool DxrRenderer::shoot(const CameraState& camera) {
    if (!impl) {
        return false;
    }
    return impl->shoot(camera);
}

void DxrRenderer::beginWeaponInspect() {
    if (impl) {
        impl->beginWeaponInspect();
    }
}

void DxrRenderer::shutdown() {
    if (impl) {
        impl->shutdown();
    }
}

const wchar_t* DxrRenderer::lastError() const {
    if (!impl || impl->lastError.empty()) {
        return L"No error";
    }
    return impl->lastError.c_str();
}