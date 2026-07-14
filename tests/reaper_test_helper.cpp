#include <cstdio>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

// Mimic a reaper parent process for GetAppIdFromReaper unit tests.
// Call as: reaper_test_helper [steam-launch args...]
int main() {
    prctl(PR_SET_NAME, "reaper", 0, 0, 0);

    // Spawn a child process (game)
    pid_t game_pid = fork();
    if (game_pid == 0) {
        prctl(PR_SET_NAME, "game", 0, 0, 0);
        while (true) pause();
        _exit(0);
    }

    // Prints its PID on stdout
    printf("%d\n", static_cast<int>(game_pid));
    fflush(stdout);

    // Wait for both processes to be killed
    while (true) pause();

    return 0;
}
