#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
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
	int stop_fd;
	std::mutex stop_fd_mutex;
	bool previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
	std::thread thread;
	bool slept_screens;
};

static struct logind_sleep_state logind_sleep_state = { .inhibit_lock_fd = -1, .stop_fd = -1 };

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

	logind_sleep_log.debugf("lock: inhibit lock successfully taken");

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

	logind_sleep_log.debugf("callback: running for PrepareForSleep=%d", going_to_sleep);

	if (going_to_sleep) {
		if (GetBackend()->IsVisible()) {
			state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = GetBackend()->ScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL);
			state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = GetBackend()->ScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL);
			logind_sleep_log.debugf("callback: saving screen sleep status: external=%d internal=%d",
				state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL],
				state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL]);
			GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, true);
			GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, true);
			if (!GetBackend()->WaitForScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, screen_sleep_timeout_ms) ||
				!GetBackend()->WaitForScreenAsleep(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, screen_sleep_timeout_ms))
				logind_sleep_log.errorf("callback: timed out waiting for screen to sleep");
			state->slept_screens = true;
		}
		if (state->inhibit_lock_fd >= 0) {
			close(state->inhibit_lock_fd);
			logind_sleep_log.debugf("callback: inhibit lock released");
		}
		state->inhibit_lock_fd = -1;
	} else {
		logind_sleep_take_inhibit_lock(state->bus, &state->inhibit_lock_fd);
		if (state->slept_screens) {
			GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL]);
			GetBackend()->SleepScreen(gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, state->previous_screen_sleep_status[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL]);
			state->slept_screens = false;
		}
	}

	return 0;
}

static void logind_sleep_run(struct logind_sleep_state *state)
{
	sd_event *event;
	sd_event_source *source;
	int ret;

	pthread_setname_np(pthread_self(), "gamescope-sleep");

	logind_sleep_log.debugf("run: initializing D-Bus connection");

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

	ret = sd_event_default(&event);
	if (ret < 0) {
		logind_sleep_log.errorf("run: sd_event_default failed %d", ret);
		goto cleanup_inhibit_fd;
	}

	ret = sd_bus_attach_event(state->bus, event, SD_EVENT_PRIORITY_NORMAL);
	if (ret < 0) {
		logind_sleep_log.errorf("run: sd_bus_attach_event failed %d", ret);
		goto cleanup_event;
	}

	// Will call sd_event_exit()
	ret = sd_event_add_io(event, &source, state->stop_fd, EPOLLIN, NULL, NULL);
	if (ret < 0) {
		logind_sleep_log.errorf("run: sd_event_add_io failed %d", ret);
		goto cleanup_event;
	}

	logind_sleep_log.debugf("run: entering event loop");

	ret = sd_event_loop(event);
	if (ret < 0)
		logind_sleep_log.errorf("run: sd_event_loop failed %d", ret);

	sd_event_source_disable_unref(source);
cleanup_event:
	sd_event_unref(event);
cleanup_inhibit_fd:
	if (state->inhibit_lock_fd >= 0)
		close(state->inhibit_lock_fd);
cleanup_bus:
	sd_bus_unref(state->bus);
exit:
	{
		std::lock_guard<std::mutex> lock(state->stop_fd_mutex);

		close(state->stop_fd);
		state->stop_fd = -1;
	}
	logind_sleep_log.debugf("run: stopping");
}

void logind_sleep_stop()
{
	struct logind_sleep_state *state = &logind_sleep_state;

	if (!state->thread.joinable())
		return;

	{
		std::lock_guard<std::mutex> lock(state->stop_fd_mutex);

		if (state->stop_fd >= 0 && eventfd_write(state->stop_fd, 1) < 0)
			logind_sleep_log.errorf_errno("stop: eventfd_write failed");
	}

	state->thread.join();
}

void logind_sleep_init()
{
	struct logind_sleep_state *state = &logind_sleep_state;

	if (!GetBackend()->SupportsScreenSleep())
		return;

	state->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (state->stop_fd < 0) {
		logind_sleep_log.errorf_errno("init: eventfd failed");
		return;
	}

	state->thread = std::thread(logind_sleep_run, state);

	atexit(logind_sleep_stop);
}
