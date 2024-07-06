#include "../Utils/Process.h"
#include "../log.hpp"

#include <cassert>
#include <cstdlib>

#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gamescope
{
    static LogScope s_ReaperLog( "reaper" );

    // Watches over a PID and waits for all the children to die.
    // It sets itself up as a subreaper so any children get reparented ti oti.
    // If the primary process dies, it kills all the children.
    //
    // Gamescope can have a lot of bad things happen to it, crashes, segfaults, whatever
    // but we always want to make sure that we cleanly kill all of our children when we die.
    // This child process attempts to stay alive as long as it can in order to fulfil Gamescope's
    // dying wish -- to kill all of it's children.
    int GamescopeReaperProcess( int argc, char **argv )
    {
        pthread_setname_np( pthread_self(), "gamescope-reaper" );

        static constexpr struct option k_ReaperOptions[] =
        {
            { "label", required_argument, nullptr, 0 },
            { "new-session-id", no_argument, nullptr, 0 },
            { "respawn", no_argument, nullptr, 0 },
        };

        bool bRespawn = false;
        bool bNewSession = false;
        static bool s_bRun = true;

        int nOptIndex = -1;
        int nOption = -1;
        while ( ( nOption = getopt_long(argc, argv, "", k_ReaperOptions, &nOptIndex ) ) != -1 )
        {
            if ( nOption == '?' )
            {
                s_ReaperLog.errorf( "Unknown option." );
            }
            assert( nOption == 0 );

            const char *pszOptionName = k_ReaperOptions[ nOptIndex ].name;
            if ( !strcmp( pszOptionName, "label" ) )
            {
                // Do nothing.
                continue;
            }
            else if ( !strcmp( pszOptionName, "respawn" ) )
            {
                bRespawn = true;
            }
            else if ( !strcmp( pszOptionName, "new-session-id" ) )
            {
                bNewSession = true;
            }
        }

        int nSubCommandArgc = 0;
        for ( int i = 0; i < argc; i++ )
        {
            if ( strcmp( "--", argv[ i ] ) == 0 && i + 1 < argc )
            {
                nSubCommandArgc = i + 1;
                break;
            }
        }

        if ( nSubCommandArgc == 0 )
        {
            s_ReaperLog.errorf( "No sub-command!" );
            return 1;
        }

        // Mirror some of the busy work we do in ProcessPreSpawn,
        // in case someone else wants to use this utility.
        Process::ResetSignals();
        std::array<int, 3> nExcludedFds =
        {{
            STDIN_FILENO,
            STDOUT_FILENO,
            STDERR_FILENO,
        }};
        Process::CloseAllFds( nExcludedFds );

        // We typically don't make a new sid, as we want to keep the same stdin/stdout
        // Don't really care about it for pgroup reasons, as processes can leave those.
        if ( bNewSession )
            setsid();

        // Set up a signal handler, so that SIGTERM, etc goes
        // and kills all the children.
        struct sigaction reaperSignalHandler{};
        reaperSignalHandler.sa_handler = []( int nSignal )
        {
            switch ( nSignal )
            {
            case SIGHUP:
            case SIGINT:
            case SIGQUIT:
            case SIGTERM:
                sigaction( SIGHUP, nullptr, nullptr );
                sigaction( SIGINT, nullptr, nullptr );
                sigaction( SIGQUIT, nullptr, nullptr );
                sigaction( SIGTERM, nullptr, nullptr );

                if ( s_bRun )
                {
                    s_ReaperLog.infof( "Parent of gamescopereaper was killed. Killing children." );

                    s_bRun = false;
                    Process::KillAllChildren( getpid(), SIGTERM );
                }
                break;
            }
        };
        sigaction( SIGHUP, &reaperSignalHandler, nullptr );
        sigaction( SIGINT, &reaperSignalHandler, nullptr );
        sigaction( SIGQUIT, &reaperSignalHandler, nullptr );
        sigaction( SIGTERM, &reaperSignalHandler, nullptr );

        // (Don't Lose) The Children
        Process::BecomeSubreaper();
        Process::SetDeathSignal( SIGTERM );

        pid_t nPrimaryChild = Process::SpawnProcess( &argv[ nSubCommandArgc ] );
        assert( nPrimaryChild != 0 );

        if ( nPrimaryChild > 0 )
        {
            // Wait for the primary child to die, then forward the death signal to
            // all of the other children, if we aren't in a PID namespace.
            Process::WaitForChild( nPrimaryChild );

            if ( bRespawn )
            {
                while ( s_bRun )
                {
                    s_ReaperLog.infof( "\"%s\" process shut down. Restarting.", argv[ nSubCommandArgc ] );

                    nPrimaryChild = Process::SpawnProcess( &argv[ nSubCommandArgc ] );
                    Process::WaitForChild( nPrimaryChild );
                }
            }

            s_bRun = false;
            Process::KillAllChildren( getpid(), SIGTERM );
            Process::WaitForAllChildren();

            return 0;
        }
        else
        {
            s_ReaperLog.errorf_errno( "Failed to create child process \"%s\" in reaper.", argv[ nSubCommandArgc ] );

            s_bRun = false;
            Process::KillAllChildren( getpid(), SIGTERM );
            Process::WaitForAllChildren();

            return 1;
        }
    }

}

int main( int argc, char **argv )
{
    return gamescope::GamescopeReaperProcess( argc, argv );
}
