#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include <optional>
#include "convar.h"
#include "Utils/Version.h"

#include <span>

#include <wayland-client.h>
#include <gamescope-action-binding-client-protocol.h>

// TODO: Consolidate
#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }
#define WAYLAND_USERDATA_TO_THIS(type, name) []<typename... Args> ( void *pData, Args... args ) { type *pThing = (type *)pData; pThing->name( std::forward<Args>(args)... ); }

namespace gamescope
{
    class CActionBinding
    {
    public:
        bool Init( gamescope_action_binding_manager *pManager, std::span<uint32_t> pKeySyms )
        {
            Shutdown();

            m_pBinding = gamescope_action_binding_manager_create_action_binding( pManager );
            if ( !m_pBinding )
                return false;

            wl_array array;
            wl_array_init(&array);
            for ( uint32_t uKeySym : pKeySyms )
            {
                uint32_t *pKeySymPtr = (uint32_t *)wl_array_add(&array, sizeof(uint32_t) );
                *pKeySymPtr = uKeySym;
            }

            gamescope_action_binding_add_listener( m_pBinding, &s_BindingListener, (void *)this );
            gamescope_action_binding_add_keyboard_trigger( m_pBinding, &array );
            gamescope_action_binding_set_description( m_pBinding, "My Example Hotkey :)" );
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
            fprintf( stderr, "Hotkey pressed!" );
        }

    private:
        gamescope_action_binding *m_pBinding = nullptr;

        static const gamescope_action_binding_listener s_BindingListener;
    };

    const gamescope_action_binding_listener CActionBinding::s_BindingListener =
    {
        .triggered = WAYLAND_USERDATA_TO_THIS( CActionBinding, Wayland_Triggered ),
    };

    class GamescopeHotkeyExample
    {
    public:
        GamescopeHotkeyExample();
        ~GamescopeHotkeyExample();

        bool Init();
        void Run();
    private:
        wl_display *m_pDisplay = nullptr;
        gamescope_action_binding_manager *m_pActionBindingManager = nullptr;

        void Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        static const wl_registry_listener s_RegistryListener;
    };

    GamescopeHotkeyExample::GamescopeHotkeyExample()
    {
    }

    GamescopeHotkeyExample::~GamescopeHotkeyExample()
    {
    }

    bool GamescopeHotkeyExample::Init()
    {
        const char *pDisplayName = getenv( "GAMESCOPE_WAYLAND_DISPLAY" );
        if ( !pDisplayName || !*pDisplayName )
            pDisplayName = "gamescope-0";

        if ( !( m_pDisplay = wl_display_connect( pDisplayName ) ) )
        {
            fprintf( stderr, "Failed to open GAMESCOPE_WAYLAND_DISPLAY.\n" );
            return false;
        }

        {
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
                return false;
            }

            wl_registry_destroy( pRegistry );
        }

        return true;
    }

    void GamescopeHotkeyExample::Run()
    {
        // Add a test hotkey of Shift + P.
        std::vector<uint32_t> uKeySyms = { 0xffe1, 0x0070 }; // XKB_KEY_Shift_L + XKB_KEY_p

        CActionBinding binding;
        if ( !binding.Init( m_pActionBindingManager, uKeySyms ) )
            return;

        wl_display_flush( m_pDisplay );

        for ( ;; )
        {
            wl_display_dispatch( m_pDisplay );
        }
    }

    void GamescopeHotkeyExample::Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
    {
        if ( !strcmp( pInterface, gamescope_action_binding_manager_interface.name ) )
        {
            m_pActionBindingManager = (decltype(m_pActionBindingManager)) wl_registry_bind( pRegistry, uName, &gamescope_action_binding_manager_interface, uVersion );
        }
    }

    const wl_registry_listener GamescopeHotkeyExample::s_RegistryListener =
    {
        .global        = WAYLAND_USERDATA_TO_THIS( GamescopeHotkeyExample, Wayland_Registry_Global ),
        .global_remove = WAYLAND_NULL(),
    };

    static int RunHotkeyExample( int argc, char *argv[] )
    {
        gamescope::GamescopeHotkeyExample hotkeyExample;
        if ( !hotkeyExample.Init() )
            return 1;

        hotkeyExample.Run();

        return 0;
    }
}

int main( int argc, char *argv[] )
{
    return gamescope::RunHotkeyExample( argc, argv );
}
