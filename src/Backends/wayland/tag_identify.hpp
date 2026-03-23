#pragma once

#include "tags.hpp"

#include <wayland-client.h>

[[maybe_unused]]
static bool IsGamescopeProxy(void* pProxy) {
	// HACK: this probably should never be called with a null pointer, but it
	// was happening after a window was closed.
	if (!pProxy)
		return false;

	const char* const* pTag = wl_proxy_get_tag((wl_proxy*)pProxy);

	return pTag == &GAMESCOPE_proxy_tag || pTag == &GAMESCOPE_plane_tag ||
		   pTag == &GAMESCOPE_toplevel_tag;
}

[[maybe_unused]]
static bool IsGamescopePlane(wl_surface* pSurface) {
	// HACK: this probably should never be called with a null pointer, but it
	// was happening after a window was closed.
	if (!pSurface)
		return false;
	const char* const* pTag = wl_proxy_get_tag((wl_proxy*)pSurface);

	return pTag == &GAMESCOPE_plane_tag || pTag == &GAMESCOPE_toplevel_tag;
}

[[maybe_unused]]
static bool IsGamescopeToplevel(wl_surface* pSurface) {
	// HACK: this probably should never be called with a null pointer, but it
	// was happening after a window was closed.
	return pSurface && (wl_proxy_get_tag((wl_proxy*)pSurface) == &GAMESCOPE_toplevel_tag);
}
