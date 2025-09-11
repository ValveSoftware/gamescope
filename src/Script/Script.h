#pragma once

#include "../Utils/Dict.h"

#include <mutex>

#if HAVE_SCRIPTING

#include <sol/sol.hpp>

namespace gamescope
{
    class CScriptScopedLock;

    struct GamescopeScript_t
    {
        // Stores table entries, and caches sol::table
        // handles for things we use frequently.

        sol::table Base;

        struct ConVars_t
        {
            sol::table Base;
        } Convars;

        struct Config_t
        {
            sol::table Base;

            sol::table KnownDisplays;

            sol::table Input;

            std::optional<std::pair<std::string_view, sol::table>> LookupDisplay( CScriptScopedLock &script, std::string_view psvVendor, uint16_t uProduct, std::string_view psvModel, std::string_view psvDataString );

            std::vector<std::pair<std::vector<uint32_t>, std::vector<std::string>>> GetHotkeys( CScriptScopedLock &script );
        } Config;
    };

    class CScriptManager
    {
    public:
        CScriptManager();

        template <typename... Args>
        void CallHook( std::string_view svName, Args&&... args )
        {
            auto range = m_Hooks.equal_range( svName );
            for ( auto iter = range.first; iter != range.second; iter++ )
            {
                int32_t nPreviousScriptId = m_nCurrentScriptId;

                Hook_t *pHook = &iter->second;

                m_nCurrentScriptId = pHook->nScriptId;
                iter->second.fnCallback( std::forward<Args>( args )... );    
                m_nCurrentScriptId = nPreviousScriptId;
            }
        }

        void RunDefaultScripts();

        void RunScriptText( std::string_view svContents );

        void RunFile( std::string_view svPath );
        bool RunFolder( std::string_view svPath, bool bRecursive = false );

        void InvalidateAllHooks();
        void InvalidateHooksForScript( int32_t nScriptId );

        sol::state *operator->() { return &m_State; }

        sol::state &State() { return m_State; }
        GamescopeScript_t &Gamescope() { return m_Gamescope; }

        std::mutex &Mutex() { return m_mutMutex; }

    protected:
        static CScriptManager &GlobalScriptScope();
        friend CScriptScopedLock;
    private:
        mutable std::mutex m_mutMutex;

        sol::state m_State;
        GamescopeScript_t m_Gamescope;

        struct Hook_t
        {
            sol::function fnCallback;
            int32_t nScriptId = -1;
        };

        MultiDict<Hook_t> m_Hooks;

        int32_t m_nCurrentScriptId = -1;

        static int32_t s_nNextScriptId;
    };

    class CScriptScopedLock
    {
    public:
        CScriptScopedLock()
            : CScriptScopedLock{ CScriptManager::GlobalScriptScope() }
        {
        }

        CScriptScopedLock( CScriptManager &manager )
            : m_Lock{ manager.Mutex() }
            , m_ScriptManager{ manager }
        {
        }

        ~CScriptScopedLock()
        {
        }

        CScriptManager &Manager() { return m_ScriptManager; }
        sol::state *State() { return &m_ScriptManager.State(); }

        sol::state *operator ->() { return State(); }
    private:
        std::scoped_lock<std::mutex> m_Lock;
        CScriptManager &m_ScriptManager;
    };

    template <typename T>
    T TableToVec( const sol::table &table )
    {
        if ( !table )
            return T{};

        T out{};
        for ( int i = 0; i < T::length(); i++ )
        {
            std::array<std::string_view, 4> ppsvIndices
            {
                "x", "y", "z", "w"
            };

            sol::optional<float> ofValue = table[ppsvIndices[i]];
            out[i] = ofValue ? *ofValue : 0;
        }
        return out;
    }

    template <typename T>
    std::vector<T> TableToVector( const sol::table &table )
    {
        std::vector<T> out;

        if ( table )
        {
            for ( auto &iter : table )
            {
                sol::optional<T> oValue = iter.second.as<sol::optional<T>>();
                if ( oValue )
                    out.emplace_back( *oValue );
            }
        }

        return out;
    }

    #define SCRIPTDESC_TEMPLATE( type ) template <typename type>
    #define DECLARE_SCRIPTDESC( type ) \
        static sol::usertype< type > s_ScriptType; \
        class CEnsureScriptTypeInstantiation { public: CEnsureScriptTypeInstantiation() { (void) type :: s_ScriptType; } } m_EnsureTemplateInstantiation_ScriptDesc;

    #define START_SCRIPTDESC( type, lua_name ) \
        inline sol::usertype<type> type::s_ScriptType = CScriptScopedLock()->new_usertype<type>( lua_name
    #define START_SCRIPTDESC_ANON( type ) \
        inline sol::usertype<type> type::s_ScriptType = CScriptScopedLock()->new_usertype<type>( typeid( type ).name()
    #define SCRIPTDESC( x, y ) , x, y
    #define END_SCRIPTDESC() );
}

#else

    #define SCRIPTDESC_TEMPLATE( type )
    #define DECLARE_SCRIPTDESC( x )
    #define START_SCRIPTDESC( type, lua_name )
    #define START_SCRIPTDESC_ANON( type )
    #define SCRIPTDESC( x, y )
    #define END_SCRIPTDESC()

#endif