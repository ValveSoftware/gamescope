#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include <gamescope-action-binding-client-protocol.h>

namespace gamescope
{
	class CHotkeyBinding;

	class HotkeyHandler
	{
	public:
		HotkeyHandler() {}
		~HotkeyHandler() {}

		bool Init();
		bool Bind( std::vector<uint32_t> pKeySyms, std::vector<std::string> args );
		void Dispatch() { wl_display_dispatch(m_pDisplay); }

	private:
		wl_display *m_pDisplay;
		gamescope_action_binding_manager *m_pActionBindingManager = nullptr;
		std::vector<std::shared_ptr<CHotkeyBinding>> m_bindings;

		void Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        static const wl_registry_listener s_RegistryListener;
	};

	extern HotkeyHandler g_hotkeyHandler;
}
