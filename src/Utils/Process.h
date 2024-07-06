#pragma once

#include <optional>
#include <functional>
#include <span>

#include <sys/types.h>

namespace gamescope::Process
{
    void BecomeSubreaper();
    void SetDeathSignal( int nSignal );

    void KillAllChildren( pid_t nParentPid, int nSignal );
    void KillProcess( pid_t nPid, int nSignal );

    std::optional<int> WaitForChild( pid_t nPid );
    void WaitForAllChildren();

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

}