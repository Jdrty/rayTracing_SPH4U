#include "../include/spawnWindow.h"

LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:	// make it so resizing actually maintains a window
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1)); // fill "dirty" areas
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_DESTROY:		// safely exit window on user request
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
		CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,	// I don't really care what size, let windows pick ¯\_(ツ)_/¯
		NULL,					// no parent window
		NULL,					// no menu
		hInstance,				// program to be ran
		NULL					// no extra data
		);
	ShowWindow(hwnd, SW_SHOW);			// make window visible

	MSG msg = {};							// empty struct to hold messages (messages as in inputs)
	while (GetMessage(&msg, NULL, 0, 0)) {	// as long as i keep recieving messages, run
		TranslateMessage(&msg);	  // turn raw key input into win readable format
		DispatchMessage(&msg);	  // send to proc
	}
}