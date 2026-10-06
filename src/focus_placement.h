#pragma once

#include <algorithm>

// Where a focus window belongs so the pointer, which X confines to the screen,
// can reach all of it. An axis the window does not fit on is left alone,
// clients center those to keep the middle reachable.
struct focus_placement_t
{
	int x, y;
};

static inline focus_placement_t focus_placement( int x, int y, int w, int h, int screen_w, int screen_h )
{
	return {
		w <= screen_w ? std::clamp( x, 0, screen_w - w ) : x,
		h <= screen_h ? std::clamp( y, 0, screen_h - h ) : y,
	};
}
