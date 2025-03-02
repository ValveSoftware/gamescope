#include <linux/input-event-codes.h>

#include "shortcut.h"
#include "convar.h"
#include "steamcompmgr_shared.hpp"
#include "main.hpp"

namespace gamescope
{
	ShortcutHandler::ShortcutHandler()
	{
		// default mappings
		m_binds = {
			{ KEY_F, GAMESCOPE_SHORTCUT_FULLSCREEN },
			{ KEY_N, GAMESCOPE_SHORTCUT_FILTER_NEAREST },
			{ KEY_B, GAMESCOPE_SHORTCUT_FILTER_LINEAR },
			{ KEY_U, GAMESCOPE_SHORTCUT_UPSCALE_FSR },
			{ KEY_Y, GAMESCOPE_SHORTCUT_UPSCALE_NIS },
			{ KEY_I, GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_UP },
			{ KEY_O, GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_DOWN },
			{ KEY_S, GAMESCOPE_SHORTCUT_SCREENSHOT },
			{ KEY_G, GAMESCOPE_SHORTCUT_KEYBOARD_GRAB }
		};
	}

	ShortcutHandler::~ShortcutHandler() {}

	void ShortcutHandler::Bind( uint32_t key, GamescopeShortcut shortcut )
	{
		Unbind( key );
		m_binds.insert( { key, shortcut } );
	}

	void ShortcutHandler::Unbind( uint32_t key )
	{
		m_binds.erase( key );
	}

	void ShortcutHandler::Unbind( GamescopeShortcut shortcut )
	{
		m_binds.erase( shortcut );
	}

	GamescopeShortcut ShortcutHandler::GetShortcut( uint32_t key )
	{
		if (auto it = m_binds.find( key ); it != m_binds.end())
		{
			return it->second;
		}

		return GAMESCOPE_SHORTCUT_NONE;
	}

	void ShortcutHandler::HandleShortcut( GamescopeShortcut shortcut )
	{
		switch ( shortcut )
		{
			case GAMESCOPE_SHORTCUT_FILTER_NEAREST:
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::PIXEL;
				break;
			case GAMESCOPE_SHORTCUT_FILTER_LINEAR:
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
				break;
			case GAMESCOPE_SHORTCUT_UPSCALE_FSR:
				g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR) ?
					GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::FSR;
				break;
			case GAMESCOPE_SHORTCUT_UPSCALE_NIS:
				g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS) ? 
					GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::NIS;
				break;
			case GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_UP:
				g_upscaleFilterSharpness = std::min( 20, g_upscaleFilterSharpness + 1 );
				break;
			case GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_DOWN:
				g_upscaleFilterSharpness = std::max( 0, g_upscaleFilterSharpness - 1 );
				break;
			case GAMESCOPE_SHORTCUT_SCREENSHOT:
				gamescope::CScreenshotManager::Get().TakeScreenshot( true );
				break;
			case GAMESCOPE_SHORTCUT_KEYBOARD_GRAB:
				g_bGrabbed = !g_bGrabbed;
				break;
			default:
				break;
		}
	}

	static const char* ShortcutNames[GAMESCOPE_SHORTCUT_COUNT] = {
		"",
		"fullscreen",
		"filter_nearest",
		"filter_linear",
		"upscale_fsr",
		"upscale_nis",
		"upscale_sharpness_up",
		"upscale_sharpness_down",
		"screenshot",
		"keyboard_grab",
	};

	static GamescopeShortcut ParseShortcut( std::string_view sv )
	{
		if ( sv.length() > 0 )
		{
			for ( auto i = 0; i < GAMESCOPE_SHORTCUT_COUNT; i++ )
			{
				if ( sv == ShortcutNames[i] )
				{
					return static_cast<GamescopeShortcut>( i );
				}
			}
		}

		return GAMESCOPE_SHORTCUT_NONE;
	}

	static ConCommand cc_bind( "bind",
	"Bind a shortcut to SUPER-<key> combination. e.g. 'bind 33,fullscreen'",
	[](std::span<std::string_view> svArgs )
	{
		if ( svArgs.size() < 2 )
		{
			console_log.errorf( "Invalid number of arguments." );
			return;
		}

		// Space delimited args would be nice but requires
		// changes to gamescopectl / gamescope_private_execute.
		auto svBinds = gamescope::Split( svArgs[1], "," );
		if ( svBinds.size() < 2 )
		{
			console_log.errorf( "Invalid number of arguments." );
			return;
		}

		// TODO: key names
		std::optional<uint32_t> ouKey = Parse<uint32_t>( svBinds[0] );
		if ( !ouKey )
		{
			console_log.errorf( "Failed to parse key." );
			return;
		}

		uint32_t uKey = *ouKey;
		auto shortcut = ParseShortcut( svBinds[1] );

		if ( shortcut != GAMESCOPE_SHORTCUT_NONE )
		{
			g_shortcutHandler.Bind( uKey, shortcut );
		}
	});

	static ConCommand cc_unbind( "unbind", "Unbind a shortcut from SUPER-<key> combination.",
	[](std::span<std::string_view> svArgs )
	{
		if ( svArgs.size() < 2 )
		{
			console_log.errorf( "Invalid number of arguments." );
			return;
		}

		// TODO: key names
		std::optional<uint32_t> ouKey = Parse<uint32_t>( svArgs[1] );
		if ( !ouKey )
		{
			console_log.errorf( "Failed to parse key." );
			return;
		}

		g_shortcutHandler.Unbind( *ouKey );
	});

	static ConCommand cc_shortcuts( "shortcuts", "List shortcut names for use with 'bind'",
	[](std::span<std::string_view> svArgs )
	{
		console_log.infof( "Shortcut names:" );
		for ( auto i = 0; i < GAMESCOPE_SHORTCUT_COUNT; i++ )
		{
			console_log.infof( "  %s", ShortcutNames[i] );
		}
	});
}
