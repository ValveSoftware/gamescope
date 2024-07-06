#include <cstdlib>
#include <cstdio>
#include <climits>

#include <fcntl.h>
#include <unistd.h>

#include "TempFiles.h"

namespace gamescope
{
    int MakeTempFile( char ( &pszOutPath )[ PATH_MAX ], const char *pszTemplate )
    {
        const char *pXDGPath = getenv( "XDG_RUNTIME_DIR" );
        if ( !pXDGPath || !*pXDGPath )
            return -1;

        snprintf( pszOutPath, PATH_MAX, "%s/%s", pXDGPath, pszTemplate );

        // Overwrites pszOutPath with the file path.
        int nFd = mkostemp( pszOutPath, O_CLOEXEC );
        if ( nFd < 0 )
            return -1;

        // Unlink so it gets destroyed when Gamescope dies.
        unlink( pszOutPath );
        return nFd;
    }

    FILE *MakeTempFile( char ( &pszOutPath )[ PATH_MAX ], const char *pszTemplate, const char *pszMode )
    {
        int nFd = MakeTempFile( pszOutPath, pszTemplate );
        if ( nFd < 0 )
            return nullptr;

        FILE *pFile = fdopen( nFd, pszMode );
        if ( !pFile )
        {
            close( nFd );
            return nullptr;
        }

        // fclose will close the file **and** the underlying file descriptor.
        return pFile;
    }
}