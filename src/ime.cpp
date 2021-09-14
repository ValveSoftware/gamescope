#include "ime.hpp"
#include "wlserver.hpp"
#include "log.hpp"

#include <unistd.h>

#include <unordered_map>
#include <vector>

extern "C" {
#define delete delete_
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#undef delete
}

/* The C/C++ standard library doesn't expose a reliable way to decode UTF-8,
 * so we need to ship our own implementation. Yay for locales. */

static const uint32_t UTF8_INVALID = 0xFFFD;

static size_t utf8_size(const char *str)
{
	uint8_t u8 = (uint8_t)str[0];
	if ((u8 & 0x80) == 0) {
		return 1;
	} else if ((u8 & 0xE0) == 0xC0) {
		return 2;
	} else if ((u8 & 0xF0) == 0xE0) {
		return 3;
	} else if ((u8 & 0xF8) == 0xF0) {
		return 4;
	} else {
		return 0;
	}
}

static uint32_t utf8_decode(const char **str_ptr)
{
	const char *str = *str_ptr;
	size_t size = utf8_size(str);
	if (size == 0) {
		*str_ptr = &str[1];
		return UTF8_INVALID;
	}

	*str_ptr = &str[size];

	const uint32_t masks[] = { 0x7F, 0x1F, 0x0F, 0x07 };
	uint32_t ret = (uint32_t)str[0] & masks[size - 1];
	for (size_t i = 1; i < size; i++) {
		ret <<= 6;
		ret |= str[i] & 0x3F;
	}
	return ret;
}

struct wlserver_input_method_key {
	xkb_keycode_t keycode;
	xkb_keysym_t keysym;
};

struct wlserver_input_method {
	struct wlr_input_method_v2 *input_method;
	struct wlserver_t *server;

	// Used to send emulated input events
	struct wlr_keyboard keyboard;
	struct wlr_input_device keyboard_device;
	std::unordered_map<uint32_t, struct wlserver_input_method_key> keys;

	struct wl_listener commit;
	struct wl_listener destroy;
};

static LogScope ime_log("ime");

static struct wlserver_input_method *active_input_method = nullptr;

static xkb_keycode_t keycode_from_ch(struct wlserver_input_method *ime, uint32_t ch)
{
	if (ime->keys.count(ch) > 0) {
		return ime->keys[ch].keycode;
	}

	xkb_keysym_t keysym = xkb_utf32_to_keysym(ch);
	if (keysym == XKB_KEY_NoSymbol) {
		return XKB_KEYCODE_INVALID;
	}

	// Add 1 because some clients don't handle correctly zero keycodes
	xkb_keycode_t keycode = ime->keys.size() + 1;
	ime->keys[ch] = (struct wlserver_input_method_key){ keycode, keysym };
	return keycode;
}

static struct xkb_keymap *generate_keymap(struct wlserver_input_method *ime)
{
	uint32_t keycode_offset = 8;

	char *str = NULL;
	size_t str_size = 0;
	FILE *f = open_memstream(&str, &str_size);

	fprintf(f,
		"xkb_keymap {\n"
		"\n"
		"xkb_keycodes \"(unnamed)\" {\n"
		"	minimum = %u;\n"
		"	maximum = %lu;\n",
		keycode_offset + 1,
		ime->keys.size() + keycode_offset + 1
	);

	for (const auto kv : ime->keys) {
		xkb_keycode_t keycode = kv.second.keycode;
		fprintf(f, "	<K%u> = %u;\n", keycode, keycode + keycode_offset);
	}

	// TODO: should we really be including "complete" here?
	fprintf(f,
		"};\n"
		"\n"
		"xkb_types \"(unnamed)\" { include \"complete\" };\n"
		"\n"
		"xkb_compatibility \"(unnamed)\" { include \"complete\" };\n"
		"\n"
		"xkb_symbols \"(unnamed)\" {\n"
	);

	for (const auto kv : ime->keys) {
		xkb_keycode_t keycode = kv.second.keycode;
		xkb_keysym_t keysym = kv.second.keysym;

		char keysym_name[256];
		xkb_keysym_get_name(keysym, keysym_name, sizeof(keysym_name));

		if (keysym_name[0] == '0' && keysym_name[1] == 'x') {
			// Unicode keysyms need special handling
			snprintf(keysym_name, sizeof(keysym_name), "U%04x", keysym);
		}

		fprintf(f, "	key <K%u> {[ %s ]};\n", keycode, keysym_name);
	}

	fprintf(f,
		"};\n"
		"\n"
		"};\n"
	);

	fclose(f);

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(context, str, str_size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(context);

	free(str);

	return keymap;
}

static void type_text(struct wlserver_input_method *ime, const char *text)
{
	ime->keys.clear();

	std::vector<xkb_keycode_t> keycodes;
	while (text[0] != '\0') {
		uint32_t ch = utf8_decode(&text);

		xkb_keycode_t keycode = keycode_from_ch(ime, ch);
		if (keycode == XKB_KEYCODE_INVALID) {
			ime_log.errorf("warning: cannot type character U+%X\n", ch);
			continue;
		}

		keycodes.push_back(keycode);
	}

	struct xkb_keymap *keymap = generate_keymap(ime);
	if (keymap == nullptr) {
		ime_log.errorf("failed to generate keymap\n");
		return;
	}
	wlr_keyboard_set_keymap(&ime->keyboard, keymap);
	xkb_keymap_unref(keymap);

	struct wlr_seat *seat = ime->server->wlr.seat;
	wlr_seat_set_keyboard(seat, &ime->keyboard_device);
	for (size_t i = 0; i < keycodes.size(); i++) {
		wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_PRESSED);
		wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_RELEASED);
	}
}

static void ime_handle_commit(struct wl_listener *l, void *data)
{
	struct wlserver_input_method *ime = wl_container_of(l, ime, commit);

	const char *text = ime->input_method->current.commit_text;
	if (text != nullptr) {
		type_text(ime, text);
	}
}

static void ime_handle_destroy(struct wl_listener *l, void *data)
{
	struct wlserver_input_method *ime = wl_container_of(l, ime, destroy);

	active_input_method = nullptr;

	wlr_input_device_destroy(&ime->keyboard_device);

	wl_list_remove(&ime->commit.link);
	wl_list_remove(&ime->destroy.link);
	delete ime;
}

static void keyboard_destroy(struct wlr_keyboard *kyeboard) {}

static const struct wlr_keyboard_impl keyboard_impl = {
	.destroy = keyboard_destroy,
};

static void keyboard_device_destroy(struct wlr_input_device *dev) {}

static const struct wlr_input_device_impl keyboard_device_impl = {
	.destroy = keyboard_device_destroy,
};

static void wlserver_new_input_method(struct wl_listener *l, void *data)
{
	struct wlserver_t *wlserver = wl_container_of(l, wlserver, new_input_method);
	struct wlr_input_method_v2 *wlr_ime = (struct wlr_input_method_v2 *)data;

	if (active_input_method != nullptr) {
		wlr_input_method_v2_send_unavailable(wlr_ime);
		return;
	}

	struct wlserver_input_method *ime = new wlserver_input_method();
	ime->input_method = wlr_ime;
	ime->server = wlserver;
	ime->commit.notify = ime_handle_commit;
	wl_signal_add(&wlr_ime->events.commit, &ime->commit);
	ime->destroy.notify = ime_handle_destroy;
	wl_signal_add(&wlr_ime->events.destroy, &ime->destroy);

	wlr_keyboard_init(&ime->keyboard, &keyboard_impl);
	wlr_input_device_init(&ime->keyboard_device, WLR_INPUT_DEVICE_KEYBOARD, &keyboard_device_impl, "ime", 0, 0);
	ime->keyboard_device.keyboard = &ime->keyboard;

	wlr_input_method_v2_send_activate(wlr_ime);
	wlr_input_method_v2_send_done(wlr_ime);

	active_input_method = ime;
}

void create_ime_manager(struct wlserver_t *wlserver)
{
	struct wlr_input_method_manager_v2 *ime_manager = wlr_input_method_manager_v2_create(wlserver->display);

	wlserver->new_input_method.notify = wlserver_new_input_method;
	wl_signal_add(&ime_manager->events.input_method, &wlserver->new_input_method);
}
