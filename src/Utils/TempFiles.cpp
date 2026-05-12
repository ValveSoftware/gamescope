#include <climits>
#include <cstdio>
#include <cstdlib>

#include <list>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "TempFiles.h"

namespace gamescope
{
    class CDeferUnlinks
    {
    public:
        void Add( std::string sPath )
        { m_DeferredUnlinks.emplace_front( sPath ); }

    private:
        class CDeferUnlink
        {
        public:
            CDeferUnlink( std::string sPath ) : m_sPath{ std::move( sPath ) } {}

            ~CDeferUnlink( ) { unlink( m_sPath.c_str( ) ); }

        private:
            const std::string m_sPath;
        };

        std::list<CDeferUnlink> m_DeferredUnlinks;
    };
    static CDeferUnlinks s_DeferredUnlinks;

    int MakeTempFile(
        char ( &pszOutPath )[ PATH_MAX ],
        const char *pszTemplate,
        bool        bDeferUnlink )
    {
        const char *pXDGPath = getenv( "XDG_RUNTIME_DIR" );
        if ( !pXDGPath || !*pXDGPath ) return -1;

        snprintf( pszOutPath, PATH_MAX, "%s/%s", pXDGPath, pszTemplate );

        // Overwrites pszOutPath with the file path.
        int nFd = mkostemp( pszOutPath, O_CLOEXEC );
        if ( nFd < 0 ) return -1;

        // Unlink so it gets destroyed when Gamescope dies.
        if ( bDeferUnlink ) { s_DeferredUnlinks.Add( pszOutPath ); }
        else
        {
            unlink( pszOutPath );
        }

        return nFd;
    }

    FILE *MakeTempFile(
        char ( &pszOutPath )[ PATH_MAX ],
        const char *pszTemplate,
        const char *pszMode,
        bool        bDeferUnlink )
    {
        int nFd = MakeTempFile( pszOutPath, pszTemplate, bDeferUnlink );
        if ( nFd < 0 ) return nullptr;

        FILE *pFile = fdopen( nFd, pszMode );
        if ( !pFile )
        {
            close( nFd );
            return nullptr;
        }

        // fclose will close the file **and** the underlying file descriptor.
        return pFile;
    }
} // namespace gamescope
