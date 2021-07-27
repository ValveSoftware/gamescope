#include <stdio.h>

#include <thread>

#include "main.hpp"
#include "pipewire.hpp"

static void stream_handle_state_changed(void *data, enum pw_stream_state old_stream_state, enum pw_stream_state stream_state, const char *error)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	fprintf(stderr, "pipewire: stream state changed: %s\n", pw_stream_state_as_string(stream_state));

	switch (stream_state) {
	case PW_STREAM_STATE_PAUSED:
		if (state->stream_node_id == SPA_ID_INVALID) {
			state->stream_node_id = pw_stream_get_node_id(state->stream);
		}
		state->streaming = false;
		break;
	case PW_STREAM_STATE_STREAMING:
		state->streaming = true;
		break;
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		state->running = false;
		break;
	default:
		break;
	}
}

static void stream_handle_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	if (param == nullptr || id != SPA_PARAM_Format)
		return;

	int bpp = 3;
	int ret = spa_format_video_raw_parse(param, &state->video_info);
	if (ret < 0) {
		fprintf(stderr, "pipewire: spa_format_video_raw_parse failed\n");
		return;
	}
	state->stride = SPA_ROUND_UP_N(state->video_info.size.width * bpp, 4);
	state->size = state->stride * state->video_info.size.height;

	uint8_t buf[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	int buffers = 1;
	int data_type = 1 << SPA_DATA_MemPtr;

	const struct spa_pod *buffers_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, 1, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(state->size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(state->stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_type));
	const struct spa_pod *meta_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	const struct spa_pod *params[] = { buffers_param, meta_param };

	ret = pw_stream_update_params(state->stream, params, sizeof(params) / sizeof(params[0]));
	if (ret != 0) {
		fprintf(stderr, "pipewire: pw_stream_update_params failed\n");
	}
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_handle_state_changed,
	.param_changed = stream_handle_param_changed,
	//.add_buffer = stream_handle_add_buffer,
	//.remove_buffer = stream_handle_remove_buffer,
};

static void run_pipewire(struct pipewire_state *state)
{
	state->running = true;
	while (state->running) {
		int ret = pw_loop_iterate(state->loop, -1);
		if (ret < 0) {
			fprintf(stderr, "pipewire: pw_loop_iterate failed\n");
			break;
		}
	}

	fprintf(stderr, "pipewire: exiting\n");
	pw_stream_destroy(state->stream);
	pw_core_disconnect(state->core);
	pw_context_destroy(state->context);
	pw_loop_destroy(state->loop);
}

bool init_pipewire(void)
{
	pw_init(nullptr, nullptr);

	static struct pipewire_state state = { .stream_node_id = SPA_ID_INVALID };

	state.loop = pw_loop_new(nullptr);
	if (!state.loop) {
		fprintf(stderr, "pipewire: pw_loop_new failed\n");
		return false;
	}

	state.context = pw_context_new(state.loop, nullptr, 0);
	if (!state.context) {
		fprintf(stderr, "pipewire: pw_context_new failed\n");
		return false;
	}

	state.core = pw_context_connect(state.context, nullptr, 0);
	if (!state.core) {
		fprintf(stderr, "pipewire: pw_context_connect failed\n");
		return false;
	}

	state.stream = pw_stream_new(state.core, "gamescope",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			nullptr));
	if (!state.stream) {
		fprintf(stderr, "pipewire: pw_stream_new failed\n");
		return false;
	}

	static struct spa_hook stream_hook;
	pw_stream_add_listener(state.stream, &stream_hook, &stream_events, &state);

	struct spa_rectangle size = SPA_RECTANGLE(g_nOutputWidth, g_nOutputHeight);
	struct spa_fraction framerate = SPA_FRACTION(0, 1);

	uint8_t buf[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *format_param =
		(const struct spa_pod *)spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_RGB),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerate));

	// TODO: PW_STREAM_FLAG_ALLOC_BUFFERS
	enum pw_stream_flags flags = PW_STREAM_FLAG_DRIVER;
	int ret = pw_stream_connect(state.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, &format_param, 1);
	if (ret != 0) {
		fprintf(stderr, "pipewire: pw_stream_connect failed\n");
		return false;
	}

	while (state.stream_node_id == SPA_ID_INVALID) {
		int ret = pw_loop_iterate(state.loop, -1);
		if (ret < 0) {
			fprintf(stderr, "pipewire: pw_loop_iterate failed\n");
			return false;
		}
	}

	fprintf(stderr, "pipewire: stream available on node ID: %u\n", state.stream_node_id);

	std::thread thread(run_pipewire, &state);
	thread.detach();

	return true;
}
