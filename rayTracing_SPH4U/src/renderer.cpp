#include "../include/renderer.h"

void render(uint32_t* pixels, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			pixels[y * w + x] = 0x00FF0000; // red as example
		}
	}
}