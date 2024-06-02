#include <memory>
#include <X11/extensions/Xfixes.h>
#include "backend.h"
#include "defer.hpp"
#include "xwayland_ctx.hpp"

extern const char *g_pOriginalDisplay;

namespace gamescope
{
	std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor()
	{
		if ( !g_pOriginalDisplay )
			return nullptr;

		Display *display = XOpenDisplay( g_pOriginalDisplay );
		if ( !display )
			return nullptr;
		defer( XCloseDisplay( display ) );

		int xfixes_event, xfixes_error;
		if ( !XFixesQueryExtension( display, &xfixes_event, &xfixes_error ) )
		{
			xwm_log.errorf("No XFixes extension on current compositor");
			return nullptr;
		}

		XFixesCursorImage *image = XFixesGetCursorImage( display );
		if ( !image )
			return nullptr;
		defer( XFree( image ) );

		// image->pixels is `unsigned long*` :/
		// Thanks X11.
		std::vector<uint32_t> cursorData = std::vector<uint32_t>( image->width * image->height );
		for (uint32_t y = 0; y < image->height; y++)
		{
			for (uint32_t x = 0; x < image->width; x++)
			{
				cursorData[y * image->width + x] = static_cast<uint32_t>( image->pixels[image->height * y + x] );
			}
		}

		return std::make_shared<INestedHints::CursorInfo>(INestedHints::CursorInfo
		{
			.pPixels   = std::move( cursorData ),
			.uWidth    = image->width,
			.uHeight   = image->height,
			.uXHotspot = image->xhot,
			.uYHotspot = image->yhot,
		});
	}
}
