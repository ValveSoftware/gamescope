#include "Script.h"
#include "convar.h"
#include "color_helpers.h"
#include "../log.hpp"

#include <filesystem>
#include <algorithm>

std::string_view GetHomeDir();

namespace gamescope
{
    using namespace std::literals;

    static LogScope s_ScriptLog{ "script" }; 
    static LogScope s_ScriptMgrLog{ "scriptmgr" };

    static ConVar<bool> cv_script_use_local_scripts{ "script_use_local_scripts", false, "Whether or not to use the local scripts (../config) as opposed to the ones in /etc/gamescope.d" };
    static ConVar<bool> cv_script_use_user_scripts{ "script_use_user_scripts", true, "Whether or not to use user config scripts ($XDG_CONFIG_DIR/gamescope) at all." };

    static std::string_view GetConfigDir()
    {
        static std::string s_sConfigDir = []() -> std::string
        {
            const char *pszConfigHome = getenv( "XDG_CONFIG_HOME" );
            if ( pszConfigHome && *pszConfigHome )
                return pszConfigHome;

            return std::string{ GetHomeDir() } + "/.config";
        }();

        return s_sConfigDir;
    }

    static inline void PanicFunction( sol::optional<std::string> oMsg )
    {
        s_ScriptLog.errorf( "Lua is in a panic state and will now abort() the application" );
        if ( oMsg )
        {
            s_ScriptLog.errorf( "\tError Message: %s", oMsg->c_str() );
        }

        abort();
    }

    static inline int ExceptionFunction( lua_State* pState, sol::optional<const std::exception&> oException, sol::string_view psvDescription )
    {
        // L is the lua state, which you can wrap in a state_view if necessary
        // maybe_exception will contain exception, if it exists
        // description will either be the what() of the exception or a description saying that we hit the general-case catch(...)

        s_ScriptLog.errorf( "An exception occurred:\n    %.*s",
            (int)psvDescription.length(), psvDescription.data() );

        // you must push 1 element onto the stack to be
        // transported through as the error object in Lua
        // note that Lua -- and 99.5% of all Lua users and libraries -- expects a string
        // so we push a single string (in our case, the description of the error)
        return sol::stack::push( pState, psvDescription );
    }

    static inline void LuaErrorHandler( const std::string &msg )
    {
        s_ScriptLog.errorf( "An error occurred:\n    %.*s",
            (int)msg.length(), msg.data() );
    }

    int32_t CScriptManager::s_nNextScriptId = 0;

    CScriptManager &CScriptManager::GlobalScriptScope()
    {
        static CScriptManager s_State;
        return s_State;
    }

    CScriptManager::CScriptManager()
    {
        m_State.open_libraries();

        static bool s_bSetDefaultHandler = false;
        if ( !s_bSetDefaultHandler )
        {
            m_State["_gamescope_error_handler"] = LuaErrorHandler;

            sol::protected_function::set_default_handler( m_State["_gamescope_error_handler"] );
            s_bSetDefaultHandler = true;
        }

        m_State.set_panic( sol::c_call<decltype(&PanicFunction), &PanicFunction> );
        m_State.set_exception_handler( &ExceptionFunction );

        m_Gamescope.Base = m_State.create_named_table( "gamescope" );
        m_Gamescope.Base["hook"] = [this]( std::string_view svName, sol::function fnFunc )
        {
            m_Hooks.emplace( std::make_pair( svName, Hook_t{ std::move( fnFunc ), m_nCurrentScriptId } ) );
        };
        m_Gamescope.Base.new_enum<EOTF>( "eotf",
            {
                { "gamma22", EOTF_Gamma22 },
                { "pq", EOTF_PQ },
                { "count", EOTF_Count },
            }
        );
        m_Gamescope.Base.new_enum<LogPriority>( "log_priority",
            {
                { "silent", LOG_SILENT },
                { "error", LOG_ERROR },
                { "warning", LOG_WARNING },
                { "info", LOG_INFO },
                { "debug", LOG_DEBUG },
            }
        );
        m_Gamescope.Base["log"] = []( LogPriority ePriority, std::string_view svText ) { s_ScriptLog.log( ePriority, svText ); };

        m_Gamescope.Convars.Base = m_State.create_table();
        m_Gamescope.Base.set( "convars", m_Gamescope.Convars.Base );

        m_Gamescope.Config.Base = m_State.create_table();
        m_Gamescope.Base.set( "config", m_Gamescope.Config.Base );

        m_Gamescope.Config.KnownDisplays = m_State.create_table();
        m_Gamescope.Config.Base.set( "known_displays", m_Gamescope.Config.KnownDisplays );
    }

    void CScriptManager::RunDefaultScripts()
    {
        const char *sScriptPathEnv = getenv("GAMESCOPE_SCRIPT_PATH");

        if ( cv_script_use_local_scripts )
        {
            RunFolder( "../scripts", true );
        }
        else if ( sScriptPathEnv )
        {
            std::vector<std::string_view> sScriptPaths = gamescope::Split( sScriptPathEnv, ":" );
            for ( const auto &sScriptPath : sScriptPaths )
            {
                RunFolder( sScriptPath, true );
            }
        }
        else
        {
            RunFolder( SCRIPT_DIR, true );
            RunFolder( "/etc/gamescope/scripts", true );
        }

        if ( cv_script_use_user_scripts )
        {
            std::string sUserConfigs = std::string{ GetConfigDir() } + "/gamescope/scripts";
            RunFolder( sUserConfigs, true );
        }
    }

    void CScriptManager::RunScriptText( std::string_view svContents )
    {
        uint32_t uScriptId = s_nNextScriptId++;

        {
            int32_t nPreviousScriptId = m_nCurrentScriptId;

            m_nCurrentScriptId = uScriptId;
            State().script( svContents );
            m_nCurrentScriptId = nPreviousScriptId;
        }
    }
    void CScriptManager::RunFile( std::string_view svPath )
    {
        uint32_t uScriptId = s_nNextScriptId++;

        s_ScriptMgrLog.infof( "Running script file '%.*s' (id: %u)",
            int( svPath.length() ), svPath.data(),
            uScriptId );

        std::string sPath = std::string( svPath );

        {
            int32_t nPreviousScriptId = m_nCurrentScriptId;

            m_nCurrentScriptId = uScriptId;
            State().script_file( std::move( sPath ) );
            m_nCurrentScriptId = nPreviousScriptId;
        }
    }
    bool CScriptManager::RunFolder( std::string_view svDirectory, bool bRecursive )
    {
        s_ScriptMgrLog.infof( "Loading scripts from: '%.*s'",
            int( svDirectory.size() ), svDirectory.data() );

        std::filesystem::path dirConfig = std::filesystem::path{ svDirectory };
        if ( !std::filesystem::is_directory( dirConfig ) )
        {
            s_ScriptMgrLog.warnf( "Directory '%.*s' does not exist",
                int( svDirectory.size() ), svDirectory.data() );
            return false;
        }

        if ( access( dirConfig.c_str(), R_OK | X_OK ) != 0 )
        {
            s_ScriptMgrLog.warnf( "Cannot open directory '%.*s'",
                int( svDirectory.size() ), svDirectory.data() );
            return false;
        }

        std::vector<std::string> sFiles;
        std::vector<std::string> sDirectories;
        for ( const auto &iter : std::filesystem::directory_iterator( dirConfig ) )
        {
            const std::filesystem::path &path = iter.path();
            // XXX: is_regular_file -> What about symlinks?
            if ( std::filesystem::is_regular_file( iter.status() ) && path.extension() == ".lua"sv )
            {
                sFiles.push_back( path );
            }

            if ( bRecursive && std::filesystem::is_directory( iter.status() ) )
            {
                sDirectories.push_back( path );
            }
        }

        std::sort( sFiles.begin(), sFiles.end() );
        std::sort( sDirectories.begin(), sDirectories.end() );

        for ( const auto &sPath : sFiles )
        {            
            RunFile( sPath );
        }

        if ( bRecursive )
        {
            for ( const auto &sPath : sDirectories )
            {
                RunFolder( sPath, bRecursive );
            }
        }

        return true;
    }

    void CScriptManager::InvalidateAllHooks()
    {
        m_Hooks.clear();
    }

    void CScriptManager::InvalidateHooksForScript( int32_t nScriptId )
    {
        if ( nScriptId < 0 )
            return;

        std::erase_if( m_Hooks, [ nScriptId ]( const auto &iter ) -> bool
        {
            return iter.second.nScriptId == nScriptId;
        });
    }

    //
    // GamescopeScript_t
    //

    std::optional<std::pair<std::string_view, sol::table>> GamescopeScript_t::Config_t::LookupDisplay( CScriptScopedLock &script, std::string_view psvVendor,  uint16_t uProduct, std::string_view psvModel, std::string_view psvDataString )
    {
        int nMaxPrority = -1;
        std::optional<std::pair<std::string_view, sol::table>> oOutDisplay;

        sol::table tDisplay = script->create_table();
        tDisplay["vendor"] = psvVendor;
        tDisplay["product"] = uProduct;
        tDisplay["model"] = psvModel;
        tDisplay["data_string"] = psvDataString;

        for ( auto iter : KnownDisplays )
        {
            sol::optional<sol::table> otTable = iter.second.as<sol::optional<sol::table>>();
            if ( !otTable )
                continue;
            sol::table tTable = *otTable;

            sol::optional<sol::function> ofnMatches = tTable["matches"];
            if ( !ofnMatches )
                continue;

            sol::function fnMatches = *ofnMatches;
            if ( !fnMatches )
                continue;

            int nPriority = fnMatches( tDisplay);
            if ( nPriority <= nMaxPrority )
                continue;

            std::string_view psvKey = iter.first.as<std::string_view>();

            nMaxPrority = nPriority;
            oOutDisplay = std::make_pair( psvKey, tTable );
        }

        return oOutDisplay;
    }

}
