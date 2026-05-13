#pragma once
#include <windows.h>
#include "objects.h"

struct CameraState {
    Vec3 position;
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    float fovYRadians;
};

class DxrRenderer {
public:
    DxrRenderer();
    ~DxrRenderer();

    DxrRenderer(const DxrRenderer&) = delete;
    DxrRenderer& operator=(const DxrRenderer&) = delete;

    bool initialize(HWND hwnd, int width, int height);
    void render(const CameraState& camera);
    void shutdown();

    const wchar_t* lastError() const;

private:
    struct Impl;
    Impl* impl;
};