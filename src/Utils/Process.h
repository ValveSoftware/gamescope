#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>

#include <sys/types.h>

namespace gamescope::Process
{
    void BecomeSubreaper();
    void SetDeathSignal( int nSignal );

    void KillAllChildren( pid_t nParentPid, int nSignal );
    void KillProcess( pid_t nPid, int nSignal );

    std::optional<int> WaitForChild( pid_t nPid );

    // Wait for all children to die,
    // but stop waiting if we hit a specific PID specified by onStopPid.
    // Returns true if we stopped because we hit the pid specified by onStopPid.
    //
    // Similar to what an `init` process would do.
    bool WaitForAllChildren( std::optional<pid_t> onStopPid = std::nullopt );

    bool CloseFd( int nFd );

    void RaiseFdLimit();
    void RestoreFdLimit();
    void ResetSignals();

    void CloseAllFds( std::span<int> nExcludedFds );

    pid_t SpawnProcess( char **argv, std::function<void()> fnPreambleInChild = nullptr, bool bDoubleFork = false );
    pid_t SpawnProcessInWatchdog( char **argv, bool bRespawn = false, std::function<void()> fnPreambleInChild = nullptr );

    bool HasCapSysNice();
    void SetNice( int nNice );
    void RestoreNice();

    bool SetRealtime();
    void RestoreRealtime();

    const char *GetProcessName();

    uint32_t GetAppIdFromCgroup( std::istream &stream );
    uint32_t GetAppIdFromReaper( pid_t pid );
    uint32_t GetAppIdFromPid( pid_t pid );
}
