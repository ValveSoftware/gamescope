#pragma once

#include "WaylandProtocol.h"

#include "gamescope-reshade-protocol.h"
#include "reshade_effect_manager.hpp"

#include <cstring>

namespace gamescope::WaylandServer
{
	class CReshadeManager : public CWaylandResource
	{
	public:
		WL_PROTO_DEFINE( gamescope_reshade, 1 );
		WL_PROTO_DEFAULT_CONSTRUCTOR();

		void EffectReadyCallback( const char* path )
		{
			gamescope_reshade_send_effect_ready(GetResource(), path);
		}

		void SetEffect( const char *path )
		{
			reshade_effect_manager_set_effect( path,
				[this]( const char* callbackPath ) { EffectReadyCallback( callbackPath ); } 
			);
		}

		void EnableEffect()
		{
			reshade_effect_manager_enable_effect();
		}

		void SetUniformVariable( const char *key, struct wl_array *value )
		{
			uint8_t* data_copy = new uint8_t[value->size];
			std::memcpy(data_copy, value->data, value->size);
			reshade_effect_manager_set_uniform_variable( key, data_copy );
		}

		void DisableEffect()
		{
			reshade_effect_manager_disable_effect();
		}

	};

	const struct gamescope_reshade_interface CReshadeManager::Implementation =
	{
		.destroy = WL_PROTO_DESTROY(),
		.set_effect = WL_PROTO( CReshadeManager, SetEffect ),
		.enable_effect = WL_PROTO( CReshadeManager, EnableEffect ),
		.set_uniform_variable = WL_PROTO( CReshadeManager, SetUniformVariable ),
		.disable_effect = WL_PROTO( CReshadeManager, DisableEffect )
	};

}
