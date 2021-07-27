#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

struct pipewire_state {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	bool running;

	struct pw_stream *stream;
	uint32_t stream_node_id;
	bool streaming;
	struct spa_video_info_raw video_info;
	int size, stride;
};

bool init_pipewire(void);
