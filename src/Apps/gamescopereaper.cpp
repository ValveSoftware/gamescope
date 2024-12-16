#include "../Utils/Process.h"
#include "../log.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>

#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

namespace gamescope
{
    static LogScope s_ReaperLog("reaper");

    // Utility to get all child PIDs of a given parent PID, recursively.
    std::vector<pid_t> GetChildPIDs(pid_t parentPid)
    {
        std::vector<pid_t> childPids;
        DIR *procDir = opendir("/proc");
        if (!procDir)
        {
            s_ReaperLog.errorf_errno("Failed to open /proc directory.");
            return childPids;
        }

        struct dirent *entry;
        while ((entry = readdir(procDir)) != nullptr)
        {
            if (entry->d_type != DT_DIR)
                continue;

            pid_t pid = atoi(entry->d_name);
            if (pid <= 0)
                continue;

            std::ifstream statFile(std::string("/proc/") + entry->d_name + "/stat");
            if (!statFile.is_open())
                continue;

            std::string statLine;
            std::getline(statFile, statLine);
            statFile.close();

            size_t openParen = statLine.find('(');
            size_t closeParen = statLine.find(')', openParen);
            if (openParen == std::string::npos || closeParen == std::string::npos)
                continue;

            // Extract the PPID (4th field, after process name with parentheses)
            size_t afterName = closeParen + 2; // After ") " (space after parentheses)
            std::istringstream iss(statLine.substr(afterName));
            std::string token;
            int i = 0;
            pid_t ppid = 0;

            while (iss >> token)
            {
                i++;
                if (i == 3) // PPID is the 4th field, but we're offset because of the name
                {
                    ppid = std::stoi(token);
                    break;
                }
            }

            if (ppid == parentPid)
            {
                childPids.push_back(pid);

                // Recursively find children of this child
                std::vector<pid_t> grandChildren = GetChildPIDs(pid);
                childPids.insert(childPids.end(), grandChildren.begin(), grandChildren.end());
            }
        }

        closedir(procDir);
        return childPids;
    }

    // Kill a process and all its children recursively
    void KillProcessTree(pid_t pid, int signal)
    {
        std::vector<pid_t> childPids = GetChildPIDs(pid);
        for (pid_t childPid : childPids)
        {
            kill(childPid, signal);
        }
        kill(pid, signal);
    }

    int GamescopeReaperProcess(int argc, char **argv)
    {
        pthread_setname_np(pthread_self(), "gamescope-reaper");

        static constexpr struct option k_ReaperOptions[] =
        {
            {"label", required_argument, nullptr, 0},
            {"new-session-id", no_argument, nullptr, 0},
            {"respawn", no_argument, nullptr, 0},
        };

        bool bRespawn = false;
        bool bNewSession = false;
        static bool s_bRun = true;

        int nOptIndex = -1;
        int nOption = -1;
        while ((nOption = getopt_long(argc, argv, "", k_ReaperOptions, &nOptIndex)) != -1)
        {
            if (nOption == '?')
            {
                s_ReaperLog.errorf("Unknown option.");
            }
            assert(nOption == 0);

            const char *pszOptionName = k_ReaperOptions[nOptIndex].name;
            if (!strcmp(pszOptionName, "label"))
            {
                // Do nothing.
                continue;
            }
            else if (!strcmp(pszOptionName, "respawn"))
            {
                bRespawn = true;
            }
            else if (!strcmp(pszOptionName, "new-session-id"))
            {
                bNewSession = true;
            }
        }

        int nSubCommandArgc = 0;
        for (int i = 0; i < argc; i++)
        {
            if (strcmp("--", argv[i]) == 0 && i + 1 < argc)
            {
                nSubCommandArgc = i + 1;
                break;
            }
        }

        if (nSubCommandArgc == 0)
        {
            s_ReaperLog.errorf("No sub-command!");
            return 1;
        }

        Process::ResetSignals();
        std::array<int, 3> nExcludedFds =
        {{
            STDIN_FILENO,
            STDOUT_FILENO,
            STDERR_FILENO,
        }};
        Process::CloseAllFds(nExcludedFds);

        if (bNewSession)
            setsid();

        struct sigaction reaperSignalHandler{};
        reaperSignalHandler.sa_handler = [](int nSignal)
        {
            switch (nSignal)
            {
            case SIGHUP:
            case SIGINT:
            case SIGQUIT:
            case SIGTERM:
                if (s_bRun)
                {
                    s_ReaperLog.infof("Parent of gamescopereaper was killed. Killing children.");
                    s_bRun = false;
                }
                break;
            }
        };
        sigaction(SIGHUP, &reaperSignalHandler, nullptr);
        sigaction(SIGINT, &reaperSignalHandler, nullptr);
        sigaction(SIGQUIT, &reaperSignalHandler, nullptr);
        sigaction(SIGTERM, &reaperSignalHandler, nullptr);

        Process::BecomeSubreaper();
        Process::SetDeathSignal(SIGTERM);

        pid_t nPrimaryChild = Process::SpawnProcess(&argv[nSubCommandArgc]);
        assert(nPrimaryChild != 0);

        if (nPrimaryChild > 0)
        {
            Process::WaitForAllChildren(nPrimaryChild);

            if (bRespawn)
            {
                while (s_bRun)
                {
                    s_ReaperLog.infof("\"%s\" process shut down. Restarting.", argv[nSubCommandArgc]);
                    nPrimaryChild = Process::SpawnProcess(&argv[nSubCommandArgc]);
                    Process::WaitForAllChildren(nPrimaryChild);
                }
            }

            s_bRun = false;
            KillProcessTree(getpid(), SIGTERM);
            return 0;
        }
        else
        {
            s_ReaperLog.errorf_errno("Failed to create child process \"%s\" in reaper.", argv[nSubCommandArgc]);
            s_bRun = false;
            KillProcessTree(getpid(), SIGTERM);
            return 1;
        }
    }

} // namespace gamescope

int main(int argc, char **argv)
{
    return gamescope::GamescopeReaperProcess(argc, argv);
}
