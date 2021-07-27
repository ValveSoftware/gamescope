#pragma once

#include <pipewire/pipewire.h>

struct pipewire_state {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	uint32_t stream_node_id;
};

bool init_pipewire(void);
