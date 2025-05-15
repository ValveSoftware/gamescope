#include <linux/input-event-codes.h>

#include <span>

#include "hotkey.h"
#include "convar.h"

// TODO: Consolidate
#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }
#define WAYLAND_USERDATA_TO_THIS(type, name) []<typename... Args> ( void *pData, Args... args ) { type *pThing = (type *)pData; pThing->name( std::forward<Args>(args)... ); }

namespace gamescope
{
	HotkeyHandler g_hotkeyHandler = {};

	class CHotkeyBinding
    {
    public:
        bool Init( gamescope_action_binding_manager *pManager, std::span<uint32_t> pKeySyms, std::vector<std::string> args )
        {
            Shutdown();

            m_pBinding = gamescope_action_binding_manager_create_action_binding( pManager );
            if ( !m_pBinding )
                return false;

            m_args = args;
            for ( std::string_view sv : args )
            {
                m_sDescription += sv;
                m_sDescription += " ";
            }

            wl_array array;
            wl_array_init(&array);
            for ( uint32_t uKeySym : pKeySyms )
            {
                uint32_t *pKeySymPtr = (uint32_t *)wl_array_add(&array, sizeof(uint32_t) );
                *pKeySymPtr = uKeySym;
            }

            gamescope_action_binding_add_listener( m_pBinding, &s_BindingListener, (void *)this );
            gamescope_action_binding_add_keyboard_trigger( m_pBinding, &array );
            gamescope_action_binding_set_description( m_pBinding, m_sDescription.c_str() );
            gamescope_action_binding_arm( m_pBinding, 0 );

            return true;
        }

        void Shutdown()
        {
            if ( m_pBinding )
            {
                gamescope_action_binding_destroy( m_pBinding );
                m_pBinding = nullptr;
            }
        }

        void Wayland_Triggered( gamescope_action_binding *pBinding, uint32_t uSequence, uint32_t uTriggerFlags, uint32_t uTimeLo, uint32_t uTimeHi )
        {
            // eugh
            std::vector<std::string_view> vec;
            for ( std::string_view sv : m_args )
            {
                vec.push_back( sv );
            }

            if ( !gamescope::ConCommand::Exec( std::span<std::string_view>{ vec } ) )
            {
                fprintf( stderr, "Failed to exec: %s\n", m_sDescription.c_str() );
            }
        }

    private:
        gamescope_action_binding *m_pBinding = nullptr;
		std::vector<std::string> m_args;
        std::string m_sDescription;

        static const gamescope_action_binding_listener s_BindingListener;
    };

    const gamescope_action_binding_listener CHotkeyBinding::s_BindingListener =
    {
        .triggered = WAYLAND_USERDATA_TO_THIS( CHotkeyBinding, Wayland_Triggered ),
    };

	bool HotkeyHandler::Init()
	{
        const char *pDisplayName = getenv( "GAMESCOPE_WAYLAND_DISPLAY" );
        if ( !pDisplayName || !*pDisplayName )
            pDisplayName = "gamescope-0";

        if ( !( m_pDisplay = wl_display_connect( pDisplayName ) ) )
        {
            fprintf( stderr, "Failed to open GAMESCOPE_WAYLAND_DISPLAY.\n" );
            return false;
        }

        wl_registry *pRegistry;
        if ( !( pRegistry = wl_display_get_registry( m_pDisplay ) ) )
        {
            fprintf( stderr, "Failed to get wl_registry.\n" );
            return false;
        }

        wl_registry_add_listener( pRegistry, &s_RegistryListener, (void *)this );
        wl_display_roundtrip( m_pDisplay );
	    wl_display_roundtrip( m_pDisplay );

        if ( !m_pActionBindingManager )
        {
            fprintf( stderr, "Failed to get Gamescope binding manager\n" );
            wl_registry_destroy( pRegistry );
            return false;
        }

        wl_registry_destroy( pRegistry );

		return true;
	}

	bool HotkeyHandler::Bind( std::vector<uint32_t> pKeySyms, std::vector<std::string> args )
	{
		std::shared_ptr<CHotkeyBinding> binding = std::make_shared<CHotkeyBinding>();
        auto success = binding->Init( m_pActionBindingManager, pKeySyms, args );
        if ( success )
        {
            m_bindings.push_back(binding);
            wl_display_flush( m_pDisplay );
        }
        return success;
	}

	void HotkeyHandler::Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
    {
        if ( !strcmp( pInterface, gamescope_action_binding_manager_interface.name ) )
        {
            m_pActionBindingManager = (decltype(m_pActionBindingManager)) wl_registry_bind( pRegistry, uName, &gamescope_action_binding_manager_interface, uVersion );
        }
    }

    const wl_registry_listener HotkeyHandler::s_RegistryListener =
    {
        .global        = WAYLAND_USERDATA_TO_THIS( HotkeyHandler, Wayland_Registry_Global ),
        .global_remove = WAYLAND_NULL(),
    };
}
