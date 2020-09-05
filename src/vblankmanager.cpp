// Try to figure out when vblank is and notify steamcompmgr to render some time before it

#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

#include "X11/Xlib.h"
#include "assert.h"

#include "gpuvis_trace_utils.h"

#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "main.hpp"

static Display *g_nestedDpy;

std::atomic<uint64_t> g_lastVblank;

uint64_t g_uVblankDrawBufferNS = 5'000'000;

void vblankThreadRun( void )
{
	while ( true )
	{
		uint64_t lastVblank = g_lastVblank - g_uVblankDrawBufferNS;
		uint64_t nsecInterval = 1'000'000'000ul / g_nOutputRefresh;

		uint64_t now = get_time_in_nanos();
		uint64_t targetPoint = lastVblank + nsecInterval;
		while ( targetPoint < now )
			targetPoint += nsecInterval;

		sleep_until_nanos( targetPoint );

		// give the time of vblank to steamcompmgr
		uint64_t vblanktime = get_time_in_nanos();
		XEvent repaintMsg = {};
		repaintMsg.xclient.type = ClientMessage;
		repaintMsg.xclient.window = DefaultRootWindow( g_nestedDpy );
		repaintMsg.xclient.format = 32;
		repaintMsg.xclient.data.l[0] = 24;
		repaintMsg.xclient.data.l[1] = 8;
		// Although these are longs which are 64-bit, something funky goes on
		// that stops us from encoding more than 32-bits in each element.
		// This matches the format above which is "32".
		repaintMsg.xclient.data.l[2] = uint32_t(vblanktime >> 32);
		repaintMsg.xclient.data.l[3] = uint32_t(vblanktime & 0xFFFFFFFF);

		// send a message to nudge it out of its event loop
		XSendEvent( g_nestedDpy , DefaultRootWindow( g_nestedDpy ), True, SubstructureRedirectMask, &repaintMsg);
		XFlush( g_nestedDpy );
		
		gpuvis_trace_printf( "sent vblank\n" );
		
		// Get on the other side of it now
		sleep_for_nanos( g_uVblankDrawBufferNS + 1'000'000 );
	}
}

void vblank_init( void )
{
	g_nestedDpy = XOpenDisplay( wlserver_get_nested_display() );
	assert( g_nestedDpy != nullptr );
	
	g_lastVblank = get_time_in_nanos();

	std::thread vblankThread( vblankThreadRun );
	vblankThread.detach();
}

void vblank_mark_possible_vblank( void )
{
	g_lastVblank = get_time_in_nanos();
}
