#include "DirHelpers.h"

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

namespace gamescope {
    std::string_view GetHomeDir()
    {
        static std::string s_sHomeDir = []() -> std::string
        {
            const char *pszHomeDir = getenv( "HOME" );
            if ( pszHomeDir )
                return pszHomeDir;
    
            return getpwuid( getuid() )->pw_dir;
        }();
        return s_sHomeDir;
    }

    std::string GetLocalUsrDir()
    {
        return std::string{ GetHomeDir() } + "/.local";
    }

    std::string GetUsrDir()
    {
        return "/usr";
    }

    std::string_view GetConfigDir()
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
}
