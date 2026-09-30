#pragma once

#include "WaylandProtocol.h"
#include "gamescope-action-binding-protocol.h"
#include "../action_binding.h"

namespace gamescope::WaylandServer
{
    using Keybind_t = gamescope::Keybind_t;
    class CGamescopeActionBinding : public CWaylandResource, public CServerActionBinding
    {
    public:
        WL_PROTO_DEFINE( gamescope_action_binding, 1 );

        CGamescopeActionBinding( WaylandResourceDesc_t desc )
            : CWaylandResource( desc )
            , CServerActionBinding()
        {
        }

        ~CGamescopeActionBinding() = default;

        // gamescope_action_binding

        void SetDescription( const char *pszDescription )
        {
            CServerActionBinding::SetDescription( pszDescription ? pszDescription : "" );
        }

        void AddKeyboardTrigger( wl_array *pKeysymsArray )
        {
            size_t zKeysymCount = pKeysymsArray->size / sizeof( xkb_keysym_t );

            std::span<const xkb_keysym_t> pKeysyms = std::span<const xkb_keysym_t> {
                reinterpret_cast<const xkb_keysym_t *>( pKeysymsArray->data ),
                zKeysymCount };

            std::unordered_set<xkb_keysym_t> setKeySyms;
            for ( xkb_keysym_t uKeySym : pKeysyms )
            {
                setKeySyms.emplace( NormalizeKeysymForHotkey( uKeySym ) );
            }

            CServerActionBinding::AddKeyboardTrigger( std::move( setKeySyms ) );
        }

        void ClearTriggers()
        {
            CServerActionBinding::ClearTriggers();
        }

        void Arm( uint32_t uArmFlags )
        {
            CServerActionBinding::Arm( uArmFlags );
        }

        void Disarm()
        {
            CServerActionBinding::Disarm();
        }

        virtual bool Execute() override
        {
            if ( !IsArmed() )
                return false;

            uint32_t uArmFlags = m_ouArmFlags.value_or( 0 );
            bool bBlockInput = !( uArmFlags & GAMESCOPE_ACTION_BINDING_ARM_FLAG_NO_BLOCK );

            uint32_t uTriggerFlags = GAMESCOPE_ACTION_BINDING_TRIGGER_FLAG_KEYBOARD;

            uint64_t ulNow = get_time_in_nanos();

            static uint32_t s_uSequence = 0;
            uint32_t uTimeLo = static_cast<uint32_t>( ulNow & 0xffffffff );
            uint32_t uTimeHi = static_cast<uint32_t>( ulNow >> 32 );

            log_binding.debugf( "(%s) -> Triggered!", m_sDescription.c_str() );
            gamescope_action_binding_send_triggered( GetResource(), s_uSequence++, uTimeLo, uTimeHi, uTriggerFlags );

            if ( uArmFlags & GAMESCOPE_ACTION_BINDING_ARM_FLAG_ONE_SHOT )
            {
                log_binding.debugf( "(%s) -> Disarming due to one-shot.", m_sDescription.c_str() );
                Disarm();
            }

            return bBlockInput;
        }

        static std::span<CServerActionBinding *> GetBindings()
        {
            return CServerActionBinding::GetBindings();
        }
    };

    const struct gamescope_action_binding_interface CGamescopeActionBinding::Implementation =
    {
        .destroy = WL_PROTO_DESTROY(),
        .set_description = WL_PROTO( CGamescopeActionBinding, SetDescription ),
        .add_keyboard_trigger = WL_PROTO( CGamescopeActionBinding, AddKeyboardTrigger ),
        .clear_triggers = WL_PROTO( CGamescopeActionBinding, ClearTriggers ),
        .arm = WL_PROTO( CGamescopeActionBinding, Arm ),
        .disarm = WL_PROTO( CGamescopeActionBinding, Disarm ),
    };

    //////////////////////////////////
    // CGamescopeActionBindingManager
    //////////////////////////////////
    class CGamescopeActionBindingManager : public CWaylandResource
    {
    public:
        WL_PROTO_DEFINE( gamescope_action_binding_manager, 1 );
        WL_PROTO_DEFAULT_CONSTRUCTOR();

        void CreateActionBinding( uint32_t uId )
        {
            CWaylandResource::Create<CGamescopeActionBinding>( m_pClient, m_uVersion, uId );
        }
    };

    const struct gamescope_action_binding_manager_interface CGamescopeActionBindingManager::Implementation =
    {
        .destroy = WL_PROTO_DESTROY(),
        .create_action_binding = WL_PROTO( CGamescopeActionBindingManager, CreateActionBinding ),
    };
}
