#pragma once

#include "WaylandProtocol.h"

#include "gamescope-limiter-protocol.h"

#include <span>
#include <vector>

uint32_t wlserver_get_frame_limiter_state( void );

namespace gamescope::WaylandServer
{

	class CGamescopeLimiter : public CWaylandResource
	{
	public:
		WL_PROTO_DEFINE( gamescope_limiter, 1 );

		CGamescopeLimiter( WaylandResourceDesc_t desc )
			: CWaylandResource( desc )
		{
			s_Limiters.push_back( this );
			SendState( wlserver_get_frame_limiter_state() );
		}

		~CGamescopeLimiter()
		{
			std::erase_if( s_Limiters, [this]( CGamescopeLimiter *pLimiter ){ return pLimiter == this; } );
		}

		void SendState( uint32_t uState )
		{
			gamescope_limiter_send_state( GetResource(), uState );
		}

		static std::span<CGamescopeLimiter *> GetLimiters()
		{
			return s_Limiters;
		}

	private:
		static std::vector<CGamescopeLimiter *> s_Limiters;
	};

	const struct gamescope_limiter_interface CGamescopeLimiter::Implementation =
	{
		.destroy = WL_PROTO_DESTROY(),
	};

	std::vector<CGamescopeLimiter *> CGamescopeLimiter::s_Limiters;

}
