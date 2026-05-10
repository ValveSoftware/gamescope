// Input Method Editor

#pragma once

#include "wlserver.hpp"
#include "gamescope-input-method-protocol.h"

void create_ime_manager(struct wlserver_t *wlserver);

struct wlserver_input_method *create_local_ime();
void destroy_ime(struct wlserver_input_method * ime);
void type_text(struct wlserver_input_method *ime, const char *text);
void perform_action(struct wlserver_input_method *ime, enum gamescope_input_method_action action);
