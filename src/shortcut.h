#pragma once

#include <cstdint>
#include <map>

namespace gamescope
{
	enum GamescopeShortcut
	{
		GAMESCOPE_SHORTCUT_NONE,

		GAMESCOPE_SHORTCUT_FULLSCREEN,
		GAMESCOPE_SHORTCUT_FILTER_NEAREST,
		GAMESCOPE_SHORTCUT_FILTER_LINEAR,
		GAMESCOPE_SHORTCUT_UPSCALE_FSR,
		GAMESCOPE_SHORTCUT_UPSCALE_NIS,
		GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_UP,
		GAMESCOPE_SHORTCUT_UPSCALE_SHARPNESS_DOWN,
		GAMESCOPE_SHORTCUT_SCREENSHOT,
		GAMESCOPE_SHORTCUT_KEYBOARD_GRAB,

		GAMESCOPE_SHORTCUT_COUNT
	};

	class ShortcutHandler
	{
	public:
		ShortcutHandler();
		~ShortcutHandler();

		void Bind( uint32_t key, GamescopeShortcut shortcut );
		void Unbind( uint32_t key);
		void Unbind( GamescopeShortcut shortcut );

		GamescopeShortcut GetShortcut( uint32_t key );

		void HandleShortcut( GamescopeShortcut shortcut );

	private:
		std::map<uint32_t, GamescopeShortcut> m_binds;
	};
}
