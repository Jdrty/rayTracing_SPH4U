#include "../include/spawnWindow.h"
#include "../include/renderer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <mmsystem.h>
#include <objbase.h>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

static DxrRenderer renderer;

namespace {
constexpr float PI = 3.1415926535f;
constexpr float EyeHeight = 1.7f;
constexpr float WalkSpeed = 4.0f;
constexpr float SprintSpeed = 8.0f;
constexpr float JumpSpeed = 5.5f;
constexpr float Gravity = 14.0f;
constexpr float MouseSensitivity = 0.0022f;

// Lightweight bunny-hop-ish movement: inertia in XZ + airaccel; turn + strafe (A/D × yaw delta) boosts in air.
constexpr float BhopGroundFrictionPerSec = 11.5f;
constexpr float BhopGroundAccel = 55.0f;
constexpr float BhopAirAccel = 120.0f;
constexpr float BhopAirAccelStrafeAssist = 1.42f;
constexpr float BhopMaxAirSpeedXZ = 15.8f;
constexpr float BhopMaxGroundSpeedMultiplier = 1.12f;
constexpr float BhopTurnStrafeAccel = 5.75f;

struct FpsCamera {
	Vec3 position = { 0.0f, EyeHeight, 4.0f };
	Vec3 hzVel = { 0.0f, 0.0f, 0.0f };
	float yaw = 0.0f;
	float pitch = 0.0f;
	float verticalVelocity = 0.0f;
	std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();

	static bool keyDown(int key) {
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	}

	static Vec3 add(Vec3 a, Vec3 b) {
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	static Vec3 mul(Vec3 v, float s) {
		return { v.x * s, v.y * s, v.z * s };
	}

	static float length(Vec3 v) {
		return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	}

	static float lengthXZ(Vec3 v) {
		return sqrtf(v.x * v.x + v.z * v.z);
	}

	static Vec3 normalize(Vec3 v) {
		float len = length(v);
		if (len <= 0.0001f) {
			return { 0.0f, 0.0f, 0.0f };
		}
		return { v.x / len, v.y / len, v.z / len };
	}

	static Vec3 normalizeXZ(Vec3 v) {
		const float len = lengthXZ(v);
		if (len <= 0.0001f) {
			return { 0.0f, 0.0f, 0.0f };
		}
		return { v.x / len, 0.0f, v.z / len };
	}

	static float dotXZ(Vec3 a, Vec3 b) {
		return a.x * b.x + a.z * b.z;
	}

	static void accelerateXZ(Vec3& vel, Vec3 wishDirXZ, float wishSpeed, float accelPerSec, float dt) {
		const float wdLen = lengthXZ(wishDirXZ);
		if (wdLen <= 1.0e-4f || wishSpeed <= 0.0f) {
			return;
		}

		const Vec3 wd = { wishDirXZ.x / wdLen, 0.0f, wishDirXZ.z / wdLen };
		const float currentSpeed = dotXZ(vel, wd);
		float addSpeed = wishSpeed - currentSpeed;
		if (addSpeed <= 0.0f) {
			return;
		}

		float accelSpeed = accelPerSec * wishSpeed * dt;
		accelSpeed = (std::min)(accelSpeed, addSpeed);
		vel.x += wd.x * accelSpeed;
		vel.z += wd.z * accelSpeed;
	}

	static void frictionGround(Vec3& velXZ, float dt) {
		const float spd = lengthXZ(velXZ);
		if (spd < 1.0e-5f) {
			return;
		}

		const float scaled = spd * BhopGroundFrictionPerSec * dt;
		float newSpd = spd - scaled;
		if (newSpd < 0.0f) {
			newSpd = 0.0f;
		}

		const float frac = spd > 1.0e-5f ? (newSpd / spd) : 0.0f;
		velXZ.x *= frac;
		velXZ.z *= frac;
	}

	void updateMouse(HWND hwnd) {
		if (GetForegroundWindow() != hwnd) {
			return;
		}

		RECT rect = {};
		GetClientRect(hwnd, &rect);
		POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
		ClientToScreen(hwnd, &center);

		POINT mouse = {};
		GetCursorPos(&mouse);
		const int dx = mouse.x - center.x;
		const int dy = mouse.y - center.y;

		if (dx != 0 || dy != 0) {
			yaw += static_cast<float>(dx) * MouseSensitivity;
			pitch -= static_cast<float>(dy) * MouseSensitivity;
			pitch = std::clamp(pitch, -1.45f, 1.45f);
			SetCursorPos(center.x, center.y);
		}
	}

	CameraState update(HWND hwnd) {
		auto now = std::chrono::steady_clock::now();
		float dt = std::chrono::duration<float>(now - lastTick).count();
		lastTick = now;
		dt = std::clamp(dt, 0.0f, 0.05f);

		const float yawBefore = yaw;
		updateMouse(hwnd);
		const float yawDelta = yaw - yawBefore;

		Vec3 forwardFlat = { sinf(yaw), 0.0f, -cosf(yaw) };
		Vec3 right = { cosf(yaw), 0.0f, sinf(yaw) };

		const bool sprint = keyDown(VK_SHIFT);

		const bool groundedForMove =
			position.y <= EyeHeight + 0.01f && verticalVelocity <= 0.35f;

		Vec3 wish = { 0.0f, 0.0f, 0.0f };
		if (keyDown('W')) {
			wish = add(wish, forwardFlat);
		}
		if (keyDown('S')) {
			wish = add(wish, mul(forwardFlat, -1.0f));
		}
		if (keyDown('D')) {
			wish = add(wish, right);
		}
		if (keyDown('A')) {
			wish = add(wish, mul(right, -1.0f));
		}

		float groundWishSpeed = sprint ? SprintSpeed : WalkSpeed;
		float groundCap = groundWishSpeed * BhopMaxGroundSpeedMultiplier;

		if (groundedForMove) {
			frictionGround(hzVel, dt);
			if (lengthXZ(wish) > 1.0e-4f) {
				accelerateXZ(hzVel, wish, groundWishSpeed, BhopGroundAccel, dt);
			}
			const float spd = lengthXZ(hzVel);
			if (spd > groundCap) {
				const float frac = groundCap / spd;
				hzVel.x *= frac;
				hzVel.z *= frac;
			}
		}
		else {
			const float wishSpeedAir = sprint ? SprintSpeed * 1.12f : WalkSpeed * 1.06f;

			float airAccel = BhopAirAccel;
			float strHeld = keyDown('D') ? 1.0f : keyDown('A') ? -1.0f : 0.0f;
			if ((keyDown('D') ^ keyDown('A')) != 0) {
				airAccel *= BhopAirAccelStrafeAssist;
			}
			if (lengthXZ(wish) > 1.0e-4f) {
				accelerateXZ(hzVel, wish, wishSpeedAir * (sprint ? 1.06f : 1.0f), airAccel, dt);
			}

			const float airSpdXZ = lengthXZ(hzVel);
			if ((keyDown('W') || keyDown('S')) != 0 && strHeld != 0.0f && airSpdXZ > 1.85f &&
				yawDelta * strHeld > 0.0f && fabsf(yawDelta) > 1e-6f) {
				const Vec3 v = hzVel;
				Vec3 perp = normalizeXZ({ -v.z, 0.0f, v.x });
				const float orb = fabsf(yawDelta) * BhopTurnStrafeAccel *
				    sqrtf((std::min)(airSpdXZ, BhopMaxAirSpeedXZ));
				hzVel = add(hzVel, mul(perp, orb * dt));
			}

			float spd = lengthXZ(hzVel);
			if (spd > BhopMaxAirSpeedXZ) {
				const float frac = BhopMaxAirSpeedXZ / spd;
				hzVel.x *= frac;
				hzVel.z *= frac;
			}
		}

		position.x += hzVel.x * dt;
		position.z += hzVel.z * dt;

		const bool jumpHeld = keyDown(VK_SPACE);

		verticalVelocity -= Gravity * dt;
		position.y += verticalVelocity * dt;

		bool landedHard = position.y <= EyeHeight;
		if (landedHard) {
			position.y = EyeHeight;
			verticalVelocity = 0.0f;
		}

		if (jumpHeld && (landedHard || (position.y <= EyeHeight + 0.012f && verticalVelocity <= 0.45f))) {
			verticalVelocity = JumpSpeed;
			position.y = EyeHeight + 0.02f;
		}

		const float cp = cosf(pitch);
		Vec3 forward = normalize({ sinf(yaw) * cp, sinf(pitch), -cosf(yaw) * cp });
		Vec3 up = normalize({
			right.y * forward.z - right.z * forward.y,
			right.z * forward.x - right.x * forward.z,
			right.x * forward.y - right.y * forward.x
		});

		return { position, forward, right, up, 80.0f * PI / 180.0f };
	}
};

static FpsCamera camera;

std::filesystem::path findGunshotPath() {
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

	for (const auto& searchRoot : roots) {
		const std::filesystem::path candidates[] = {
			searchRoot / L"assets" / L"u_62htdrvg4y-gun-shot-359196.mp3",
			searchRoot / L"rayTracing_SPH4U" / L"assets" / L"u_62htdrvg4y-gun-shot-359196.mp3"
		};

		for (const auto& path : candidates) {
			if (std::filesystem::exists(path)) {
				return path;
			}
		}
	}

	return {};
}

void initializeGunshotSound() {
	const std::filesystem::path path = findGunshotPath();
	if (path.empty()) {
		fwprintf(stderr, L"Gunshot MP3 not found in assets.\n");
		return;
	}

	std::wstring command = L"open \"" + path.wstring() + L"\" type mpegvideo alias gunshot";
	mciSendStringW(L"close gunshot", nullptr, 0, nullptr);
	mciSendStringW(command.c_str(), nullptr, 0, nullptr);
}

void playGunshotSound() {
	mciSendStringW(L"stop gunshot", nullptr, 0, nullptr);
	mciSendStringW(L"seek gunshot to start", nullptr, 0, nullptr);
	mciSendStringW(L"play gunshot", nullptr, 0, nullptr);
}
}

LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_DESTROY:		// safely exit window on user request
		renderer.shutdown();
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);	// let windows handle wtv else
}

void spawnWindow() {
	// STA matches MCI (gunshot); MTA reliably breaks mp3 playback on the UI thread here.
	const HRESULT hrComInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	HINSTANCE hInstance = GetModuleHandle(nullptr);		// handle to program
	const wchar_t className[] = L".slf";	// name the window class, chosen name is arbitrary don't think abt it
	WNDCLASS wc = {};						// zero out the WNDCLASS struct, to be used to define the window
	wc.lpfnWndProc = winProc;				// use winproc for all message tasks of this class
	wc.hInstance = hInstance;				// attach to program instance
	wc.lpszClassName = className;			// give the class its name
	
	RegisterClass(&wc);

	MONITORINFO mi = {};
	mi.cbSize = sizeof(MONITORINFO);
	const POINT origin = { 0, 0 };
	HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	GetMonitorInfoW(primary, &mi);
	const RECT rc = mi.rcMonitor;
	const int fw = rc.right - rc.left;
	const int fh = rc.bottom - rc.top;

	HWND hwnd = CreateWindowExW(
		WS_EX_APPWINDOW,
		className,
		L".slf",
		WS_POPUP | WS_VISIBLE,
		rc.left,
		rc.top,
		fw,
		fh,
		NULL,
		NULL,
		hInstance,
		NULL);

	if (!hwnd || !renderer.initialize(hwnd, fw, fh)) {
		fwprintf(stderr, L"DXR initialization failed: %ls\n", renderer.lastError());
		MessageBox(hwnd, renderer.lastError(), L"DXR initialization failed", MB_OK | MB_ICONERROR);
		if (SUCCEEDED(hrComInit)) {
			CoUninitialize();
		}
		return;
	}

	ShowWindow(hwnd, SW_SHOW);
	SetForegroundWindow(hwnd);
	ShowCursor(FALSE);
	initializeGunshotSound();

	MSG msg = {};							// empty struct to hold messages (messages as in inputs)
	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else {
			CameraState cameraState = camera.update(hwnd);
			static bool fKeyWasDown = false;
			const bool fKeyDown = (GetAsyncKeyState('F') & 0x8000) != 0;
			if (fKeyDown && !fKeyWasDown) {
				renderer.beginWeaponInspect();
			}
			fKeyWasDown = fKeyDown;
			if ((GetAsyncKeyState(VK_LBUTTON) & 0x0001) != 0) {
				playGunshotSound();
				renderer.shoot(cameraState);
			}
			renderer.render(cameraState);
		}
	}

	mciSendStringW(L"close gunshot", nullptr, 0, nullptr);
	ShowCursor(TRUE);
	if (SUCCEEDED(hrComInit)) {
		CoUninitialize();
	}
}