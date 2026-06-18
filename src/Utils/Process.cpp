#include "Process.h"
#include "../Utils/Algorithm.h"
#include "../convar.h"
#include "../log.hpp"
#include "../Utils/Defer.h"

#include <algorithm>
#include <array>

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#if defined(__linux__)
#if HAVE_LIBCAP
#include <sys/capability.h>
#endif
#include <sys/prctl.h>
#elif defined(__DragonFly__) || defined(__FreeBSD__)
#include <sys/procctl.h>
#endif

#include <pthread.h>
#include <stdlib.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>

extern const char *__progname;

static LogScope s_ProcessLog( "process" );

namespace gamescope::Process
{
    static bool IsDigit( char chChar )
    {
        return chChar >= '0' && chChar <= '9';
    }

    void BecomeSubreaper()
    {
#if defined(__linux__)
        prctl( PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0 );
#elif defined(__DragonFly__) || defined(__FreeBSD__)
        procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
#else
#warning "Changing reaper process for children is not supported on this platform"
#endif
    }

    void SetDeathSignal( int nSignal )
    {
#if defined( __linux__ )
        // Kill myself when my parent dies.
        prctl( PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0 );
#else
#warning "Setting death signal is not supported on this platform"
#endif
    }

    std::vector<pid_t> GetChildPids( pid_t nPid )
    {
        std::vector<pid_t> nPids;

        DIR *pProcDir = opendir( "/proc" );
        if ( !pProcDir )
        {
            s_ProcessLog.errorf( "Failed to open /proc" );
            return {};
        }
        defer( closedir( pProcDir ) );

        struct dirent *pEntry;
        while ( ( pEntry = readdir( pProcDir ) ) )
        {
            if ( pEntry->d_type != DT_DIR )
                continue;

            if ( !IsDigit( pEntry->d_name[0] ) )
                continue;

            char szPath[ PATH_MAX ];
            snprintf( szPath, sizeof( szPath ), "/proc/%s/stat", pEntry->d_name );
            
            FILE *pStatFile = fopen( szPath, "r" );
            if ( !pStatFile )
                continue;
            defer( fclose( pStatFile ) );

            pid_t nParentPid = -1;
            fscanf( pStatFile, "%*d %*s %*c %d", &nParentPid );
            if ( nParentPid > 0 && nParentPid == nPid )
                nPids.push_back( *Parse<pid_t>( pEntry->d_name ) );
        }

        return nPids;
    }

    void KillProcessTree( std::vector<pid_t> nPids, int nSignal )
    {
        pid_t nPid = 0;
        int sz = nPids.size() - 1;
        while (sz >= 0)
        {
            nPid = nPids[sz--];
            KillProcess( nPid, nSignal );
            WaitForChild( nPid );
        }
    }


    void KillAllChildren( pid_t nParentPid, int nSignal )
    {
        std::vector<pid_t> nChildPids = GetChildPids( nParentPid );
        KillProcessTree( nChildPids, nSignal );
    }

    void KillProcess( pid_t nPid, int nSignal )
    {
        if ( kill( nPid, nSignal ) == -1 )
        {
            if ( errno == ESRCH )
            {
                // Process already terminated.
            }
            else
            {
                s_ProcessLog.errorf_errno( "Failed to kill process %d", nPid );
            }
        }
    }

    std::optional<int> WaitForChild( pid_t nPid )
    {
        for ( ;; )
        {
            int nStatus = 0;
            if ( waitpid( nPid, &nStatus, 0 ) == -1 )
            {
                if ( errno == EINTR )
                    continue;

                if ( errno != ECHILD )
                {
                    s_ProcessLog.errorf_errno( "Wait for primary child failed." );
                }

                return std::nullopt;
            }
            else
            {
                return nStatus;
            }
        }
    }

    bool WaitForAllChildren( std::optional<pid_t> onStopPid )
    {
        for ( ;; )
        {
            int nStatus = 0;
            pid_t nDeadChild = waitpid( -1, &nStatus, 0 );

            if ( onStopPid && nDeadChild == *onStopPid )
                return true;

            if ( nDeadChild == -1 && errno == ECHILD )
                return false;
        }
    }

    static std::optional<rlimit> g_oOriginalFDLimit{};
    void RaiseFdLimit()
    {
        if ( g_oOriginalFDLimit )
        {
            s_ProcessLog.errorf( "FD Limit already raised!" );
            return;
        }

        rlimit originalLimit{};
        if ( getrlimit( RLIMIT_NOFILE, &originalLimit ) != 0 )
        {
            s_ProcessLog.errorf( "Could not query maximum number of open files. Leaving at default value." );
            return;
        }

        if ( originalLimit.rlim_cur >= originalLimit.rlim_max )
        {
            // Already at max.
            return;
        }

        rlimit newLimit = originalLimit;
        newLimit.rlim_cur = newLimit.rlim_max;
        if ( setrlimit( RLIMIT_NOFILE, &newLimit ) )
        {
            s_ProcessLog.errorf( "Failed to raise the maximum number of open files. Leaving at default value." );
            return;
        }

        g_oOriginalFDLimit = originalLimit;
    }

    void RestoreFdLimit()
    {
        if ( !g_oOriginalFDLimit )
            return;

        if ( setrlimit( RLIMIT_NOFILE, &*g_oOriginalFDLimit ) )
        {
            s_ProcessLog.errorf( "Failed to reset the maximum number of open files in child process." );
            s_ProcessLog.errorf( "Use of select() may fail." );
            return;
        }

        g_oOriginalFDLimit = std::nullopt;
    }

    void ResetSignals()
    {
        sigset_t set;
        sigemptyset( &set );
        sigprocmask( SIG_SETMASK, &set, nullptr );
    }

    static void ProcessPreSpawn()
    {
        ResetSignals();

        RestoreFdLimit();
        RestoreNice();
        RestoreRealtime();
    }

    bool CloseFd( int nFd )
    {
        return close( nFd ) == 0;
    }

    void CloseAllFds( std::span<int> nExcludedFds )
    {
        DIR *pProcDir = opendir( "/proc/self/fd" );
        if ( !pProcDir )
        {
            s_ProcessLog.errorf( "Failed to open /proc" );
            return;
        }
        defer( closedir( pProcDir ) );

        struct dirent *pEntry;
        while ( ( pEntry = readdir( pProcDir ) ) )
        {
            std::optional<int> onFd = Parse<int32_t>( pEntry->d_name );
            if ( !onFd )
                continue;

            int nFd = *onFd;

            bool bExcluded = Algorithm::Contains( nExcludedFds, nFd );
            if ( bExcluded )
                continue;

            if ( !CloseFd( nFd ) )
            {
                s_ProcessLog.errorf_errno( "CloseAllFds failed to close FD %d", nFd );
            }
        }
    }

    pid_t SpawnProcess( char **argv, std::function<void()> fnPreambleInChild, bool bDoubleFork )
    {
        // Create a pipe for the child to return the grandchild's
        // PID into.
        int nPidPipe[2] = { -1, -1 };
        if ( bDoubleFork )
        {
            if ( pipe2( nPidPipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
            {
                s_ProcessLog.errorf( "Failed to create PID pipe" );
                return -1;
            }
        }

        pid_t nChild = fork();
        if ( nChild < 0 )
        {
            if ( bDoubleFork )
            {
                CloseFd( nPidPipe[0] );
                CloseFd( nPidPipe[1] );
            }
            s_ProcessLog.errorf_errno( "Failed to fork() child" );
            return -1;
        }
        else if ( nChild == 0 )
        {
            std::array<int, 5> nExcludedFds =
            {{
                STDIN_FILENO,
                STDOUT_FILENO,
                STDERR_FILENO,
                nPidPipe[0], // -1 if !bDoubleFork, which is fine.
                nPidPipe[1],
            }};
            CloseAllFds( nExcludedFds );

            ProcessPreSpawn();

            if ( bDoubleFork )
            {
                // Don't need the read pipe anymore.
                CloseFd( nPidPipe[0] );
            }

            if ( fnPreambleInChild )
                fnPreambleInChild();

            if ( bDoubleFork )
            {
                pid_t nGrandChild = fork();
                if ( nGrandChild == 0 )
                {
                    CloseFd( nPidPipe[1] );

                    if ( execvp( argv[0], argv ) == -1 )
                    {
                        s_ProcessLog.errorf_errno( "Failed to start process \"%s\"", argv[0] );
                    }
                    _exit( 0 );
                }
                else if ( nGrandChild < 0 )
                {
                    s_ProcessLog.errorf_errno( "Failed to fork() grandchild." );
                }

                ssize_t sszRet = write( nPidPipe[1], &nGrandChild, sizeof( nGrandChild ) );
                (void) sszRet; // Cannot handle this error here, it is checked on the other side anyway.
                CloseFd( nPidPipe[1] );

                _exit( 0 );
            }
            else
            {
                if ( execvp( argv[0], argv ) == -1 )
                {
                    s_ProcessLog.errorf_errno( "Failed to start process \"%s\"", argv[0] );
                }
                _exit( 0 );
            }
        }

        // Parent Path
        // ...

        if ( bDoubleFork )
        {
            // Wait for the immediate child to exit, as all it does
            // is fork to spawn a child to orphan.
            WaitForChild( nChild );

            // Now that the child process is done it must have written fully to the pipe.
            // Read the PID back from the pipe and close it.
            pid_t nGrandChild = 0;
            ssize_t sszAmountRead = read( nPidPipe[0], &nGrandChild, sizeof( nGrandChild ) );
            CloseFd( nPidPipe[0] );
            CloseFd( nPidPipe[1] );

            // Sanity check what we got from the pipe.
            if ( sszAmountRead != sizeof( nGrandChild ) )
                return -1;

            return nGrandChild;
        }
        else
        {
            return nChild;
        }
    }

    pid_t SpawnProcessInWatchdog( char **argv, bool bRespawn, std::function<void()> fnPreambleInChild )
    {
        std::vector<char *> args;
        args.push_back( (char *)"gamescopereaper" );
        if ( bRespawn )
            args.push_back( (char *)"--respawn" );
        args.push_back( (char *)"--" );
        while ( *argv )
        {
            args.push_back( *argv );
            argv++;
        }
        args.push_back( NULL );
        return SpawnProcess( args.data(), fnPreambleInChild );
    }

    bool HasCapSysNice()
    {
#if defined(__linux__) && HAVE_LIBCAP
        static bool s_bHasCapSysNice = []() -> bool
        {
            cap_t pCaps = cap_get_proc();
            if ( !pCaps )
                return false;
            defer( cap_free( pCaps ) );

			cap_flag_value_t eNiceCapValue = CAP_CLEAR;
			if ( cap_get_flag( pCaps, CAP_SYS_NICE, CAP_EFFECTIVE, &eNiceCapValue ) != 0 )
                return false;

            return eNiceCapValue == CAP_SET;
        }();

        return s_bHasCapSysNice;
#else
        return false;
#endif
    }

    std::optional<int> g_oOldNice;
    std::optional<int> g_oNewNice;
    void SetNice( int nNice )
    {
#if defined(__linux__)
        if ( !HasCapSysNice() )
            return;

        errno = 0;
        int nOldNice = nice( 0 );
        if ( nOldNice != -1 || errno == 0 )
        {
            g_oOldNice = nOldNice;
        }

        errno = 0;
        int nNewNice = nice( -20 );
        if ( nNewNice != -1 || errno == 0 )
        {
            g_oNewNice = nNewNice;
        }
#endif
    }

    void RestoreNice()
    {
#if defined(__linux__)
        if ( !HasCapSysNice() )
            return;

        if ( !g_oOldNice || !g_oNewNice )
            return;

        if ( *g_oOldNice == *g_oNewNice )
            return;

        errno = 0;
        int nNewNice = nice( *g_oOldNice - *g_oNewNice );
        if ( g_oNewNice != -1 || errno == 0 )
        {
            g_oNewNice = nNewNice;
        }

        if ( g_oOldNice == g_oNewNice )
        {
            g_oOldNice = std::nullopt;
            g_oNewNice = std::nullopt;
        }
        else
        {
            s_ProcessLog.errorf( "RestoreNice: Old Nice != New Nice" );
        }
#endif
    }

    struct SchedulerInfo
    {
        int nPolicy;
        struct sched_param SchedParam;

        static std::optional<SchedulerInfo> Get()
        {
            SchedulerInfo info{};
            if ( pthread_getschedparam( pthread_self(), &info.nPolicy, &info.SchedParam) )
            {
                s_ProcessLog.errorf_errno( "Failed to get old scheduler info." );
                return std::nullopt;
            }
            return info;
        }
    };

    std::optional<SchedulerInfo> g_oOldSchedulerInfo;
    bool SetRealtime()
    {
#if defined(__linux__)
        if ( !HasCapSysNice() )
            return false;

        g_oOldSchedulerInfo = SchedulerInfo::Get();
        if ( !g_oOldSchedulerInfo )
            return false;

        struct sched_param newSched{};
        sched_getparam( 0, &newSched );
        newSched.sched_priority = sched_get_priority_min( SCHED_RR );
        if ( pthread_setschedparam( pthread_self(), SCHED_RR, &newSched ) )
        {
            s_ProcessLog.errorf_errno( "Failed to set realtime scheduling." );
            return false;
        }

        return true;
#endif
    }

    void RestoreRealtime()
    {
#if defined(__linux__)
        if ( !HasCapSysNice() )
            return;

        if ( !g_oOldSchedulerInfo )
            return;

        if ( pthread_setschedparam( pthread_self(), g_oOldSchedulerInfo->nPolicy, &g_oOldSchedulerInfo->SchedParam ) )
        {
            s_ProcessLog.errorf_errno( "Failed to restore from realtime scheduling." );
            return;
        }

        g_oOldSchedulerInfo = std::nullopt;
#endif
    }

    const char *GetProcessName()
    {
        return __progname;
    }

}
