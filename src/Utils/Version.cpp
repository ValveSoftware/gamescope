#include "Version.h"
#include "Process.h"

#include "GamescopeVersion.h"

#include "convar.h"

namespace gamescope
{
    void PrintVersion()
    {
        console_log.infof( "%s version %s", Process::GetProcessName(), gamescope::k_szGamescopeVersion );
    }
}