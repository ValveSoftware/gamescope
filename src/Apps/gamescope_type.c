#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <wayland-client.h>

#include "gamescope-input-method-client-protocol.h"

// Based on wl-ime-type by Simon Ser

static struct gamescope_input_method_manager *ime_manager = NULL;
static struct wl_seat *seat = NULL;

static bool ime_unavailable = false;
static uint32_t ime_serial = 0;

static void noop(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static void ime_handle_done(void *data, struct gamescope_input_method *ime, uint32_t serial)
{
	(void)data; (void)ime;
	ime_serial = serial;
}

static void ime_handle_unavailable(void *data, struct gamescope_input_method *ime)
{
	(void)data; (void)ime;
	ime_unavailable = true;
}

static const struct gamescope_input_method_listener ime_listener = {
	.done = ime_handle_done,
	.unavailable = ime_handle_unavailable,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
	(void)data;	(void)version;

	if (strcmp(iface, gamescope_input_method_manager_interface.name) == 0) {
		ime_manager = wl_registry_bind(registry, name, &gamescope_input_method_manager_interface, 2);
	} else if (seat == NULL && strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = noop,
};

static int getch(void) {
    struct termios oldattr;
    tcgetattr(STDIN_FILENO, &oldattr);
    struct termios newattr = oldattr;
    newattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
    int ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
    return ch;
}

int main()
{
	const char *display_name = getenv("GAMESCOPE_WAYLAND_DISPLAY");
	if (!display_name)
		display_name = "gamescope-0";

	struct wl_display *display = wl_display_connect(display_name);
	if (display == NULL) {
		fprintf(stderr, "wl_display_connect failed\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_registry_destroy(registry);

	if (seat == NULL) {
		fprintf(stderr, "Compositor has no seat\n");
		return 1;
	}
	if (ime_manager == NULL) {
		fprintf(stderr, "Compositor doesn't support input-method-unstable-v2\n");
		return 1;
	}

	struct gamescope_input_method *ime = gamescope_input_method_manager_create_input_method(ime_manager, seat);
	gamescope_input_method_add_listener(ime, &ime_listener, NULL);

	// Wait for the initial done/unavailable event
	wl_display_roundtrip(display);

	if (ime_unavailable) {
		fprintf(stderr, "IME is unavailable (maybe another IME is active?)\n");
		return 1;
	}

	bool in_escape = false;
	for (;;)
	{
		int ch = getch();

		if (in_escape) {
			if (ch == '[')
				continue;

			switch(ch) {
				case 'A': gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_UP); gamescope_input_method_commit(ime, ime_serial); break;
				case 'B': gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_DOWN); gamescope_input_method_commit(ime, ime_serial); break;
				case 'C': gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_RIGHT); gamescope_input_method_commit(ime, ime_serial); break;
				case 'D': gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_LEFT); gamescope_input_method_commit(ime, ime_serial); break;
				case '3': gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_DELETE_RIGHT); gamescope_input_method_commit(ime, ime_serial); getch(); break;
				case '5': for (int i = 0; i < 128; i++) { gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_UP); gamescope_input_method_commit(ime, ime_serial); } break;
				case '6': for (int i = 0; i < 128; i++) { gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_DOWN); gamescope_input_method_commit(ime, ime_serial); } break;
				case 'H': for (int i = 0; i < 128; i++) { gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_LEFT); gamescope_input_method_commit(ime, ime_serial); } break;
				case 'F': for (int i = 0; i < 128; i++) { gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_MOVE_RIGHT); gamescope_input_method_commit(ime, ime_serial); } break;
			}

			wl_display_roundtrip(display);
			in_escape = false;
			continue;
		}

		if (ch == '\b' || ch == 0x7f) // delete
			gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_DELETE_LEFT);
		else if (ch == '\r')
			continue;
		else if (ch == '\n')
			gamescope_input_method_set_action(ime, GAMESCOPE_INPUT_METHOD_ACTION_SUBMIT);
		else if (ch == '\033') { // esc
			in_escape = true;
			continue;
		}
		else {
			char str[2] = { (char)ch, '\0' };
			gamescope_input_method_set_string(ime, str);
		}

		gamescope_input_method_commit(ime, ime_serial);
		wl_display_roundtrip(display);
	}

	gamescope_input_method_destroy(ime);
	gamescope_input_method_manager_destroy(ime_manager);
	wl_display_disconnect(display);

	return 0;
}
