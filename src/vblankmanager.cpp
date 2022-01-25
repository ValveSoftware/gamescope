// Try to figure out when vblank is and notify steamcompmgr to render some time before it

#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "gpuvis_trace_utils.h"

#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "main.hpp"

static int g_vblankPipe[2];

std::atomic<uint64_t> g_lastVblank;

uint64_t g_uVblankDrawBufferNS = 5'000'000;

void vblankThreadRun( void )
{
	pthread_setname_np( pthread_self(), "gamescope-vblk" );

	while ( true )
	{
		uint64_t lastVblank = g_lastVblank - g_uVblankDrawBufferNS;
		uint64_t nsecInterval = 1'000'000'000ul / g_nOutputRefresh;

		if ( g_nNestedRefresh != 0 )
		{
			nsecInterval = 1'000'000'000ul / g_nNestedRefresh;
		}

		uint64_t now = get_time_in_nanos();
		uint64_t targetPoint = lastVblank + nsecInterval;
		while ( targetPoint < now )
			targetPoint += nsecInterval;

		sleep_until_nanos( targetPoint );

		// give the time of vblank to steamcompmgr
		uint64_t vblanktime = get_time_in_nanos();

		ssize_t ret = write( g_vblankPipe[ 1 ], &vblanktime, sizeof( vblanktime ) );
		if ( ret <= 0 )
		{
			perror( "vblankmanager: write failed" );
		}
		else
		{
			gpuvis_trace_printf( "sent vblank" );
		}
		
		// Get on the other side of it now
		sleep_for_nanos( g_uVblankDrawBufferNS + 1'000'000 );
	}
}

int vblank_init( void )
{
	if ( pipe2( g_vblankPipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
	{
		perror( "vblankmanager: pipe failed" );
		return -1;
	}
	
	g_lastVblank = get_time_in_nanos();

	std::thread vblankThread( vblankThreadRun );
	vblankThread.detach();

	return g_vblankPipe[ 0 ];
}

void vblank_mark_possible_vblank( uint64_t nanos )
{
	g_lastVblank = nanos;
}
