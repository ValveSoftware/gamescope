#include "convar.h"
#include "Utils/Version.h"
#include <algorithm>

LogScope console_log("console");

extern void PrintGamescopeVersion();

namespace gamescope
{
    bool ConCommand::Exec( std::span<std::string_view> args )
    {
        if ( args.size() < 1 )
        {
            console_log.warnf( "No command specified." );
            return false;
        }

        std::string_view commandName = args[0];
        auto iter = GetCommands().find( commandName );
        if ( iter == GetCommands().end() )
        {
            console_log.warnf( "Command not found." );
            return false;
        }

        iter->second->Invoke( args );
        return true;
    }

    Dict<ConCommand *>& ConCommand::GetCommands()
    {
        static Dict<ConCommand *> s_Commands;
        return s_Commands;
    }

    static ConCommand cc_help("help", "List all Gamescope convars and commands",
    []( std::span<std::string_view> args )
    {
        auto &commands = ConCommand::GetCommands();

        struct CommandHelp { std::string_view pszName; std::string_view pszDesc; };
        std::vector<CommandHelp> commandHelps;
        for ( auto &command : commands )
            commandHelps.emplace_back( command.second->GetName(), command.second->GetDescription() );
        std::sort( commandHelps.begin(), commandHelps.end(),
            []( const CommandHelp &a, const CommandHelp &b )
            {
                return a.pszName < b.pszName;
            });

        for ( auto &help : commandHelps )
        {
            console_log.infof( "%.*s: %.*s",
                (int)help.pszName.size(), help.pszName.data(), 
                (int)help.pszDesc.size(), help.pszDesc.data() );
        }
    });

    static ConCommand cc_version("version", "Print current Gamescope version",
    []( std::span<std::string_view> args )
    {
        PrintVersion();
    });
}