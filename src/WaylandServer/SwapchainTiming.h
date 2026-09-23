#pragma once

#include <cstdint>
#include "gamescope-swapchain-protocol.h"

namespace gamescope
{
	struct PresentTimingRoute
	{
		// Accessed only under wlserver_lock, including resource destruction.
		wl_resource *resource = nullptr;
		// Predicted scanout of this swapchain's last latched frame. steamcompmgr only.
		uint64_t last_expected_present_time = 0;
		const bool timing_events = false;

		bool SendPresentTiming( uint64_t serial, uint64_t queueEnd, uint64_t dequeued, uint64_t pixelOut ) const
		{
			if ( !resource || !timing_events )
				return false;
			gamescope_swapchain_send_present_timing( resource, serial >> 32, uint32_t(serial),
				queueEnd >> 32, uint32_t(queueEnd), dequeued >> 32, uint32_t(dequeued),
				pixelOut >> 32, uint32_t(pixelOut) );
			return true;
		}

		void SendTimingProperties( uint64_t duration, uint64_t interval ) const
		{
			if ( !resource || !timing_events )
				return;
			gamescope_swapchain_send_timing_properties( resource,
				duration >> 32, uint32_t(duration), interval >> 32, uint32_t(interval) );
		}
	};
}
