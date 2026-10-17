#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>

#include "backend.h"

static LogScope logind_sleep_log("logind_sleep");

static const char *logind_service = "org.freedesktop.login1";
static const char *logind_path = "/org/freedesktop/login1";
static const char *logind_interface = "org.freedesktop.login1.Manager";

static const uint32_t screen_sleep_timeout_ms = 2000;

struct logind_sleep_state {
	sd_bus *bus;
	int inhibit_lock_fd;
	int bus_fd;
	int stop_pipe_fd[2];
	bool previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
};

static struct logind_sleep_state logind_sleep_state = { .inhibit_lock_fd = { -1 }, .bus_fd = { -1 }, .stop_pipe_fd = { -1, -1 } };

static int logind_sleep_take_inhibit_lock(sd_bus *bus, int *inhibit_lock_fd)
{
	sd_bus_message *reply = NULL;
	int tmp_fd;
	int ret;

	ret = sd_bus_call_method(bus, logind_service, logind_path,
			logind_interface, "Inhibit", NULL, &reply, "ssss",
			"sleep", "Gamescope compositor", "Turn display off before sleep",
			"delay");
	if (ret < 0) {
		logind_sleep_log.errorf("lock: sd_bus_call_method failed %d", ret);
		goto unref_message;
	}

	ret = sd_bus_message_read(reply, "h", &tmp_fd);
	if (ret < 0) {
		logind_sleep_log.errorf("lock: sd_bus_message_read failed %d", ret);
		goto unref_message;
	}

	ret = fcntl(tmp_fd, F_DUPFD_CLOEXEC, 3);
	if (ret < 0) {
		logind_sleep_log.errorf_errno("lock: fcntl failed");
		goto unref_message;
	}

	*inhibit_lock_fd = ret;

	logind_sleep_log.debugf("inhibit lock successfully taken");

unref_message:
	sd_bus_message_unref(reply);
	return ret;
}

static int logind_sleep_prepare_for_sleep_cb(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct logind_sleep_state *state = (struct logind_sleep_state*)userdata;
	int going_to_sleep;
	int ret;

	ret = sd_bus_message_read(m, "b", &going_to_sleep);
	if (ret < 0) {
		logind_sleep_log.errorf("callback: sd_bus_message_read failed %d", ret);
		return ret;
	}

	if (going_to_sleep) {
		state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = GetBackend()->ScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL);
		state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = GetBackend()->ScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL);
		GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, true);
		GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, true);
		if (!GetBackend()->WaitForScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, screen_sleep_timeout_ms) ||
		    !GetBackend()->WaitForScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, screen_sleep_timeout_ms))
			logind_sleep_log.errorf("callback: timed out waiting for screen to sleep");
		close(state->inhibit_lock_fd);
		state->inhibit_lock_fd = -1;
	} else {
		logind_sleep_take_inhibit_lock(state->bus, &state->inhibit_lock_fd);
		GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL]);
		GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL]);
	}

	return 0;
}

static void logind_sleep_run(struct logind_sleep_state *state)
{
	int ret;
	struct pollfd pollfds[2];

	pthread_setname_np(pthread_self(), "gamescope-sleep");

	logind_sleep_log.debugf("initializing D-Bus connection");

	ret = sd_bus_default_system(&state->bus);
	if (ret < 0) {
		logind_sleep_log.errorf("run: sd_bus_default_system failed %d", ret);
		goto exit;
	}

	ret = logind_sleep_take_inhibit_lock(state->bus, &state->inhibit_lock_fd);
	if (ret < 0)
		goto cleanup_bus;

	ret = sd_bus_match_signal(state->bus, NULL, logind_service, logind_path,
			logind_interface, "PrepareForSleep", logind_sleep_prepare_for_sleep_cb,
			state);
	if (ret < 0) {
		logind_sleep_log.errorf("run: sd_bus_match_signal failed %d", ret);
		goto cleanup_inhibit_fd;
	}

	state->bus_fd = sd_bus_get_fd(state->bus);
	if (state->bus_fd < 0) {
		logind_sleep_log.errorf("run: sd_bus_get_fd failed %d", state->bus_fd);
		goto cleanup_inhibit_fd;
	}

	logind_sleep_log.debugf("D-Bus connection initialized");

	pollfds[0] = { .fd = state->bus_fd, .events = POLLIN, };
	pollfds[1] = { .fd = state->stop_pipe_fd[0], .events = POLLIN, };
	while (true) {
		ret = poll(pollfds, 2, -1);
		if (ret < 0) {
			logind_sleep_log.errorf_errno("poll failed");
			goto cleanup_inhibit_fd;
		}

		if (pollfds[1].revents & POLLIN)
			break;

		if (pollfds[0].revents & POLLHUP) {
			logind_sleep_log.errorf("run: lost connection to bus");
			goto cleanup_inhibit_fd;
		}

		if (pollfds[0].revents & POLLIN) {
			ret = sd_bus_process(state->bus, NULL);
			if (ret < 0) {
				logind_sleep_log.errorf("run: sd_bus_process failed %d", ret);
				goto cleanup_inhibit_fd;
			}
		}
	}

cleanup_inhibit_fd:
	if (state->inhibit_lock_fd >= 0)
		close(state->inhibit_lock_fd);
cleanup_bus:
	sd_bus_flush_close_unref(state->bus);
exit:
	close(state->stop_pipe_fd[0]);
	close(state->stop_pipe_fd[1]);
	state->stop_pipe_fd[0] = -1;
	state->stop_pipe_fd[1] = -1;
	logind_sleep_log.debugf("stopping");
}

void logind_sleep_stop()
{
	struct logind_sleep_state *state = &logind_sleep_state;

	if (!GetBackend()->SupportsScreenSleep())
		return;

	if (state->stop_pipe_fd[1] < 0)
		return;

	if (write(state->stop_pipe_fd[1], "\n", 1 ) < 0 )
		logind_sleep_log.errorf_errno("stop: write failed");
}

void logind_sleep_init()
{
	struct logind_sleep_state *state = &logind_sleep_state;

	if (!GetBackend()->SupportsScreenSleep())
		return;

	if (pipe2(state->stop_pipe_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
		logind_sleep_log.errorf_errno("init: pipe2 failed");
		return;
	}

	std::thread thread(logind_sleep_run, state);
	thread.detach();
}
