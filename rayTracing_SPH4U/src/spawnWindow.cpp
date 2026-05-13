#include "../include/spawnWindow.h"
#include "../include/renderer.h"
#include <cstdio>

static const int WINDOW_WIDTH = 800;
static const int WINDOW_HEIGHT = 600;
static DxrRenderer renderer;

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
	HINSTANCE hInstance = GetModuleHandle(nullptr);		// handle to program
	const wchar_t className[] = L".slf";	// name the window class, chosen name is arbitrary don't think abt it
	WNDCLASS wc = {};						// zero out the WNDCLASS struct, to be used to define the window
	wc.lpfnWndProc = winProc;				// use winproc for all message tasks of this class
	wc.hInstance = hInstance;				// attach to program instance
	wc.lpszClassName = className;			// give the class its name
	
	RegisterClass(&wc);
	HWND hwnd = CreateWindowEx(			// actually make the window
		0,						// no extra style
		className,				// select window class
		L".slf",					// titlebar text
		WS_OVERLAPPEDWINDOW,	// just a regular window
		CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT,
		NULL,					// no parent window
		NULL,					// no menu
		hInstance,				// program to be ran
		NULL					// no extra data
		);

	if (!hwnd || !renderer.initialize(hwnd, WINDOW_WIDTH, WINDOW_HEIGHT)) {
		fwprintf(stderr, L"DXR initialization failed: %ls\n", renderer.lastError());
		MessageBox(hwnd, renderer.lastError(), L"DXR initialization failed", MB_OK | MB_ICONERROR);
		return;
	}

	ShowWindow(hwnd, SW_SHOW);			// make window visible

	MSG msg = {};							// empty struct to hold messages (messages as in inputs)
	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else {
			renderer.render();
		}
	}
}