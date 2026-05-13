#pragma once

#include <climits>
#include <cstdio>

namespace gamescope
{
    static constexpr const char k_szGamescopeTempFileTemplate[] =
        "gamescope-temp-XXXXXXXX";
    static constexpr const char k_szGamescopeTempShmTemplate[] =
        "gamescope-shm-XXXXXXXX";
    static constexpr const char k_szGamescopeTempMangoappTemplate[] =
        "gamescope-mangoapp-XXXXXXXX";
    static constexpr const char k_szGamescopeTempLimiterTemplate[] =
        "gamescope-limiter-XXXXXXXX";

    int MakeTempFile(
        char ( &pszOutPath )[ PATH_MAX ],
        const char *pszTemplate,
        bool        bDeferUnlink = false );
    FILE *MakeTempFile(
        char ( &pszOutPath )[ PATH_MAX ],
        const char *pszTemplate,
        const char *pszMode,
        bool        bDeferUnlink = false );
} // namespace gamescope
