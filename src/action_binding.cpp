#include "action_binding.h"
#include "Utils/String.h"
#include "convar.h"

#if HAVE_SCRIPTING
#include "Script/Script.h"
#endif

#include <algorithm>
#include <cctype>

LogScope log_binding( "binding" );

namespace gamescope
{
    static constexpr std::pair<xkb_keysym_t, xkb_keysym_t> k_mapKeysymRemapping[]
    {
        { XKB_KEY_ISO_Left_Tab, XKB_KEY_Tab },
        { XKB_KEY_ISO_Enter, XKB_KEY_Return },
        { XKB_KEY_Meta_L, XKB_KEY_Super_L },
        { XKB_KEY_Meta_R, XKB_KEY_Super_R },
        { XKB_KEY_ISO_Level3_Shift, XKB_KEY_Alt_R },
    };

    xkb_keysym_t NormalizeKeysymForHotkey( xkb_keysym_t uKeySym )
    {
        uKeySym = xkb_keysym_to_upper( uKeySym );

        for ( auto [ uBadKey, uGoodKey ] : k_mapKeysymRemapping )
        {
            if ( uKeySym == uBadKey )
                uKeySym = uGoodKey;
        }

        return uKeySym;
    }

    std::string ComputeDebugName( const std::unordered_set<xkb_keysym_t> &syms )
    {
        std::string sTriggerDebugName;
        bool bFirst = true;

        for ( xkb_keysym_t uKeySym : syms )
        {
            if ( !bFirst )
            {
                sTriggerDebugName += " + ";
            }
            bFirst = false;

            char szName[256] = "";
            if ( xkb_keysym_get_name( uKeySym, szName, sizeof( szName ) ) <= 0 )
            {
                snprintf( szName, sizeof( szName ), "xkb_keysym_t( 0x%x )", uKeySym );
            }

            sTriggerDebugName += szName;
        }

        return sTriggerDebugName;
    }

    std::optional<std::unordered_set<xkb_keysym_t>> ParseHotkeyStringToKeysyms( std::string_view svCombo )
    {
        std::unordered_set<xkb_keysym_t> setKeySyms;
        std::vector<std::string_view> tokens = Split( svCombo, "+" );
        if ( tokens.empty() )
            return std::nullopt;

        for ( std::string_view tokenView : tokens )
        {
            while ( !tokenView.empty() && isspace( static_cast<unsigned char>( tokenView.front() ) ) )
                tokenView.remove_prefix( 1 );
            while ( !tokenView.empty() && isspace( static_cast<unsigned char>( tokenView.back() ) ) )
                tokenView.remove_suffix( 1 );

            if ( tokenView.empty() )
                continue;

            std::string token{ tokenView };
            std::string lowerToken = token;
            std::transform( lowerToken.begin(), lowerToken.end(), lowerToken.begin(), []( unsigned char c ){ return std::tolower( c ); } );

            xkb_keysym_t keySym = XKB_KEY_NoSymbol;

            if ( lowerToken == "super" || lowerToken == "win" || lowerToken == "meta" || lowerToken == "mod4" || lowerToken == "super_l" )
                keySym = XKB_KEY_Super_L;
            else if ( lowerToken == "super_r" )
                keySym = XKB_KEY_Super_R;
            else if ( lowerToken == "ctrl" || lowerToken == "control" || lowerToken == "control_l" || lowerToken == "ctrl_l" )
                keySym = XKB_KEY_Control_L;
            else if ( lowerToken == "control_r" || lowerToken == "ctrl_r" )
                keySym = XKB_KEY_Control_R;
            else if ( lowerToken == "alt" || lowerToken == "mod1" || lowerToken == "alt_l" )
                keySym = XKB_KEY_Alt_L;
            else if ( lowerToken == "alt_r" || lowerToken == "altgr" )
                keySym = XKB_KEY_Alt_R;
            else if ( lowerToken == "shift" || lowerToken == "shift_l" )
                keySym = XKB_KEY_Shift_L;
            else if ( lowerToken == "shift_r" )
                keySym = XKB_KEY_Shift_R;
            else if ( lowerToken == "enter" )
                keySym = XKB_KEY_Return;
            else if ( lowerToken == "esc" )
                keySym = XKB_KEY_Escape;
            else
            {
                keySym = xkb_keysym_from_name( token.c_str(), XKB_KEYSYM_CASE_INSENSITIVE );
                if ( keySym == XKB_KEY_NoSymbol )
                {
                    keySym = xkb_keysym_from_name( token.c_str(), XKB_KEYSYM_NO_FLAGS );
                }
                if ( keySym == XKB_KEY_NoSymbol && token.length() == 1 )
                {
                    keySym = xkb_utf32_to_keysym( static_cast<uint32_t>( static_cast<unsigned char>( std::tolower( token[0] ) ) ) );
                }
            }

            if ( keySym == XKB_KEY_NoSymbol )
            {
                log_binding.warnf( "Unknown key '%s' in hotkey combo '%.*s'",
                    token.c_str(), static_cast<int>( svCombo.size() ), svCombo.data() );
                return std::nullopt;
            }

            setKeySyms.emplace( NormalizeKeysymForHotkey( keySym ) );
        }

        if ( setKeySyms.empty() )
            return std::nullopt;

        return setKeySyms;
    }

    std::vector<CServerActionBinding *> CServerActionBinding::s_Bindings;

    CServerActionBinding::CServerActionBinding( std::string sDescription )
        : m_sDescription( std::move( sDescription ) )
    {
        s_Bindings.push_back( this );
    }

    CServerActionBinding::~CServerActionBinding()
    {
        std::erase_if( s_Bindings, [this]( CServerActionBinding *pBinding ){ return pBinding == this; } );
    }

    void CServerActionBinding::AddKeyboardTrigger( std::unordered_set<xkb_keysym_t> setKeySyms )
    {
        std::string sTriggerDebugName = ComputeDebugName( setKeySyms );
        log_binding.infof( "(%s) -> Adding new trigger [%s].", m_sDescription.c_str(), sTriggerDebugName.c_str() );
        m_KeyboardTriggers.emplace_back( std::move( setKeySyms ), std::move( sTriggerDebugName ) );
    }

    void CServerActionBinding::ClearTriggers()
    {
        log_binding.infof( "(%s) -> Cleared triggers.", m_sDescription.c_str() );
        m_KeyboardTriggers.clear();
    }

    void CServerActionBinding::Arm( uint32_t uArmFlags )
    {
        log_binding.debugf( "(%s) -> Arming: %x.", m_sDescription.c_str(), uArmFlags );
        m_ouArmFlags = uArmFlags;
    }

    void CServerActionBinding::Disarm()
    {
        log_binding.debugf( "(%s) -> Disarming.", m_sDescription.c_str() );
        m_ouArmFlags = std::nullopt;
    }

    std::span<CServerActionBinding *> CServerActionBinding::GetBindings()
    {
        return s_Bindings;
    }

    CNativeActionBinding::CNativeActionBinding( std::string sDescription, std::string sCommand )
        : CServerActionBinding( std::move( sDescription ) )
        , m_eType( ActionType::Command )
        , m_sCommand( std::move( sCommand ) )
    {
    }

    CNativeActionBinding::CNativeActionBinding( std::string sDescription, std::function<void()> fnCallback )
        : CServerActionBinding( std::move( sDescription ) )
        , m_eType( ActionType::CppFunc )
        , m_fnCallback( std::move( fnCallback ) )
    {
    }

#if HAVE_SCRIPTING
    CNativeActionBinding::CNativeActionBinding( std::string sDescription, sol::function luaCallback )
        : CServerActionBinding( std::move( sDescription ) )
        , m_eType( ActionType::LuaFunc )
        , m_luaCallback( std::move( luaCallback ) )
    {
    }
#endif

    bool CNativeActionBinding::Execute()
    {
        if ( !IsArmed() )
            return false;

        uint32_t uArmFlags = m_ouArmFlags.value_or( 0 );
        bool bBlockInput = !( uArmFlags & ActionBindingArmFlag_NoBlock );

        switch ( m_eType )
        {
            case ActionType::Command:
                log_binding.debugf( "(%s) -> Triggered native \"%s\" action!", m_sDescription.c_str(), m_sCommand.c_str() );
                ConCommand::Exec( m_sCommand );
                break;
            case ActionType::CppFunc:
                log_binding.debugf( "(%s) -> Triggered native C++ callback action!", m_sDescription.c_str() );
                if ( m_fnCallback )
                    m_fnCallback();
                break;
#if HAVE_SCRIPTING
            case ActionType::LuaFunc:
                log_binding.debugf( "(%s) -> Triggered native Lua callback action!", m_sDescription.c_str() );
                if ( m_luaCallback.valid() )
                {
                    CScriptScopedLock script;
                    m_luaCallback();
                }
                break;
#endif
        }

        if ( uArmFlags & ActionBindingArmFlag_OneShot )
        {
            Disarm();
        }

        return bBlockInput;
    }

    static std::vector<std::unique_ptr<CNativeActionBinding>> s_NativeBindings;

    void RegisterNativeBinding( std::unique_ptr<CNativeActionBinding> pBinding )
    {
        s_NativeBindings.push_back( std::move( pBinding ) );
    }

    void ClearNativeBindings()
    {
        s_NativeBindings.clear();
    }

    void RegisterHotkeysFromScript()
    {
#if HAVE_SCRIPTING
        ClearNativeBindings();

        CScriptScopedLock script;
        sol::table configTable = script.Manager().Gamescope().Config.Base;
        if ( !configTable.valid() )
            return;

        sol::object inputObj = configTable["input"];
        if ( !inputObj.is<sol::table>() )
            return;

        sol::table inputTable = inputObj.as<sol::table>();
        sol::object hotkeysObj = inputTable["hotkeys"];
        if ( !hotkeysObj.is<sol::table>() )
            return;

        sol::table hotkeysTable = hotkeysObj.as<sol::table>();
        for ( const auto &pair : hotkeysTable )
        {
            if ( !pair.first.is<std::string>() )
                continue;

            std::string sKeyCombo = pair.first.as<std::string>();
            std::optional<std::unordered_set<xkb_keysym_t>> oKeysyms;

            sol::object val = pair.second;
            std::string sDesc = sKeyCombo;
            std::string sCmd;
            sol::function luaFn;
            bool bHasCmd = false;
            bool bHasFn = false;
            bool bNestedOnly = false;

            if ( val.is<std::string>() )
            {
                sCmd = val.as<std::string>();
                bHasCmd = true;
                oKeysyms = ParseHotkeyStringToKeysyms( sKeyCombo );
            }
            else if ( val.is<sol::function>() )
            {
                luaFn = val.as<sol::function>();
                bHasFn = true;
                oKeysyms = ParseHotkeyStringToKeysyms( sKeyCombo );
            }
            else if ( val.is<sol::table>() )
            {
                sol::table tbl = val.as<sol::table>();
                sDesc = tbl.get_or<std::string>( "description", sKeyCombo );
                bNestedOnly = tbl.get_or( "nested_only", false );

                sol::object keysObj = tbl["keys"];
                if ( keysObj.is<sol::table>() )
                {
                    sol::table keysTable = keysObj.as<sol::table>();
                    std::string sJoined;
                    for ( size_t i = 1; i <= keysTable.size(); ++i )
                    {
                        sol::object elem = keysTable[i];
                        if ( elem.is<std::string>() )
                        {
                            if ( !sJoined.empty() )
                                sJoined += "+";
                            sJoined += elem.as<std::string>();
                        }
                    }
                    oKeysyms = ParseHotkeyStringToKeysyms( sJoined );
                }
                else if ( keysObj.is<std::string>() )
                {
                    oKeysyms = ParseHotkeyStringToKeysyms( keysObj.as<std::string>() );
                }
                else
                {
                    oKeysyms = ParseHotkeyStringToKeysyms( sKeyCombo );
                }

                sol::object action = tbl["action"];
                if ( !action.valid() )
                    action = tbl["func"];

                if ( action.is<std::string>() )
                {
                    sCmd = action.as<std::string>();
                    bHasCmd = true;
                }
                else if ( action.is<sol::function>() )
                {
                    luaFn = action.as<sol::function>();
                    bHasFn = true;
                }
            }

            if ( !oKeysyms || oKeysyms->empty() )
            {
                log_binding.warnf( "Failed to parse hotkey combo for '%s'", sKeyCombo.c_str() );
                continue;
            }

            if ( bHasCmd )
            {
                auto pBinding = std::make_unique<CNativeActionBinding>( std::move( sDesc ), std::move( sCmd ) );
                pBinding->AddKeyboardTrigger( std::move( *oKeysyms ) );
                pBinding->SetNestedOnly( bNestedOnly );
                pBinding->Arm( 0 );
                RegisterNativeBinding( std::move( pBinding ) );
            }
            else if ( bHasFn )
            {
                auto pBinding = std::make_unique<CNativeActionBinding>( std::move( sDesc ), std::move( luaFn ) );
                pBinding->AddKeyboardTrigger( std::move( *oKeysyms ) );
                pBinding->SetNestedOnly( bNestedOnly );
                pBinding->Arm( 0 );
                RegisterNativeBinding( std::move( pBinding ) );
            }
        }
#endif
    }
}
