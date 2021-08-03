#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "main.hpp"
#include "pipewire.hpp"
#include "log.hpp"

static LogScope log("pipewire");

static struct pipewire_state pipewire_state = { .stream_node_id = SPA_ID_INVALID };
static int nudgePipe[2] = { -1, -1 };

static std::atomic<struct pipewire_buffer *> out_buffer;
static std::atomic<struct pipewire_buffer *> in_buffer;

static const struct spa_pod *get_format_param(struct spa_pod_builder *builder) {
	struct spa_rectangle size = SPA_RECTANGLE(g_nOutputWidth, g_nOutputHeight);
	struct spa_fraction framerate = SPA_FRACTION(0, 1);

	return (const struct spa_pod *) spa_pod_builder_add_object(builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerate));
}

static void request_buffer(struct pipewire_state *state)
{
	struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(state->stream);
	if (!pw_buffer) {
		log.errorf("warning: out of buffers");
		state->needs_buffer = true;
		return;
	}

	struct spa_buffer *spa_buffer = pw_buffer->buffer;
	uint8_t *data = (uint8_t *) spa_buffer->datas[0].data;
	assert(data != nullptr);

	struct pipewire_buffer *buffer = (struct pipewire_buffer *) pw_buffer->user_data;

	struct spa_meta_header *header = (struct spa_meta_header *) spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof(*header));
	if (header != nullptr) {
		header->pts = -1;
		header->flags = 0;
		header->seq = state->seq++;
		header->dts_offset = 0;
	}

	struct spa_chunk *chunk = spa_buffer->datas[0].chunk;
	chunk->offset = 0;
	chunk->size = state->video_info.size.height * state->stride;
	chunk->stride = state->stride;

	struct pipewire_buffer *old = out_buffer.exchange(buffer);
	assert(old == nullptr);
}

static void dispatch_nudge(struct pipewire_state *state, int fd)
{
	while (true) {
		static char buf[1024];
		if (read(fd, buf, sizeof(buf)) < 0) {
			if (errno != EAGAIN)
				log.errorf_errno("dispatch_nudge: read failed");
			break;
		}
	}

	if (g_nOutputWidth != state->video_info.size.width || g_nOutputHeight != state->video_info.size.height) {
		log.debugf("renegociating stream params (size: %dx%d)", g_nOutputWidth, g_nOutputHeight);

		uint8_t buf[1024];
		struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		const struct spa_pod *format_param = get_format_param(&builder);
		int ret = pw_stream_update_params(state->stream, &format_param, 1);
		if (ret < 0) {
			log.errorf("pw_stream_update_params failed");
		}
	}

	struct pipewire_buffer *buffer = in_buffer.exchange(nullptr);
	if (buffer != nullptr) {
		pw_stream_queue_buffer(state->stream, buffer->buffer);

		if (state->streaming) {
			request_buffer(state);
		}
	}
}

static void stream_handle_state_changed(void *data, enum pw_stream_state old_stream_state, enum pw_stream_state stream_state, const char *error)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	log.debugf("stream state changed: %s", pw_stream_state_as_string(stream_state));

	switch (stream_state) {
	case PW_STREAM_STATE_PAUSED:
		if (state->stream_node_id == SPA_ID_INVALID) {
			state->stream_node_id = pw_stream_get_node_id(state->stream);
		}
		state->streaming = false;
		state->seq = 0;
		break;
	case PW_STREAM_STATE_STREAMING:
		state->streaming = true;
		request_buffer(state);
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

	int bpp = 4;
	int ret = spa_format_video_raw_parse(param, &state->video_info);
	if (ret < 0) {
		log.errorf("spa_format_video_raw_parse failed");
		return;
	}
	state->stride = SPA_ROUND_UP_N(state->video_info.size.width * bpp, 4);

	log.debugf("format changed (size: %dx%d)", state->video_info.size.width, state->video_info.size.height);

	uint8_t buf[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	int buffers = 4;
	int size = state->stride * state->video_info.size.height;
	int data_type = 1 << SPA_DATA_MemFd;

	const struct spa_pod *buffers_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, 1, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
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
		log.errorf("pw_stream_update_params failed");
	}
}

static void stream_handle_process(void *data)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	if (state->needs_buffer) {
		state->needs_buffer = false;
		request_buffer(state);
	}
}

static void randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int anonymous_shm_open(void)
{
	char name[] = "/gamescope-pw-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

static void stream_handle_add_buffer(void *user_data, struct pw_buffer *pw_buffer)
{
	struct pipewire_state *state = (struct pipewire_state *) user_data;

	struct spa_buffer *spa_buffer = pw_buffer->buffer;
	struct spa_data *spa_data = &spa_buffer->datas[0];

	if ((spa_data->type & (1 << SPA_DATA_MemFd)) == 0) {
		log.errorf("unsupported data type");
		return;
	}

	int fd = anonymous_shm_open();
	if (fd < 0) {
		log.errorf("failed to create shm file");
		return;
	}

	off_t size = state->stride * state->video_info.size.height;
	if (ftruncate(fd, size) != 0) {
		log.errorf_errno("ftruncate failed");
		close(fd);
		return;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		log.errorf_errno("mmap failed");
		close(fd);
		return;
	}

	struct pipewire_buffer *buffer = new pipewire_buffer();
	buffer->buffer = pw_buffer;
	buffer->video_info = state->video_info;
	buffer->stride = state->stride;
	buffer->data = (uint8_t *) data;

	pw_buffer->user_data = buffer;

	spa_data->type = SPA_DATA_MemFd;
	spa_data->flags = SPA_DATA_FLAG_READABLE;
	spa_data->fd = fd;
	spa_data->mapoffset = 0;
	spa_data->maxsize = size;
	spa_data->data = data;
}

static void stream_handle_remove_buffer(void *data, struct pw_buffer *pw_buffer)
{
	struct spa_buffer *spa_buffer = pw_buffer->buffer;
	struct spa_data *spa_data = &spa_buffer->datas[0];
	struct pipewire_buffer *buffer = (struct pipewire_buffer *) pw_buffer->user_data;

	delete buffer;
	munmap(spa_data->data, spa_data->maxsize);
	close(spa_data->fd);
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_handle_state_changed,
	.param_changed = stream_handle_param_changed,
	.add_buffer = stream_handle_add_buffer,
	.remove_buffer = stream_handle_remove_buffer,
	.process = stream_handle_process,
};

enum event_type {
	EVENT_PIPEWIRE,
	EVENT_NUDGE,
	EVENT_COUNT // keep last
};

static void run_pipewire(struct pipewire_state *state)
{
	struct pollfd pollfds[] = {
		[EVENT_PIPEWIRE] = {
			.fd = pw_loop_get_fd(state->loop),
			.events = POLLIN,
		},
		[EVENT_NUDGE] = {
			.fd = nudgePipe[0],
			.events = POLLIN,
		},
	};

	state->running = true;
	while (state->running) {
		int ret = poll(pollfds, EVENT_COUNT, -1);
		if (ret < 0) {
			log.errorf_errno("poll failed");
			break;
		}

		if (pollfds[EVENT_PIPEWIRE].revents & POLLHUP) {
			log.errorf("lost connection to server");
			break;
		}

		assert(!(pollfds[EVENT_NUDGE].revents & POLLHUP));

		if (pollfds[EVENT_PIPEWIRE].revents & POLLIN) {
			ret = pw_loop_iterate(state->loop, -1);
			if (ret < 0) {
				log.errorf("pw_loop_iterate failed");
				break;
			}
		}

		if (pollfds[EVENT_NUDGE].revents & POLLIN) {
			dispatch_nudge(state, nudgePipe[0]);
		}
	}

	log.infof("exiting");
	pw_stream_destroy(state->stream);
	pw_core_disconnect(state->core);
	pw_context_destroy(state->context);
	pw_loop_destroy(state->loop);
}

bool init_pipewire(void)
{
	struct pipewire_state *state = &pipewire_state;

	pw_init(nullptr, nullptr);

	if (pipe2(nudgePipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		log.errorf_errno("pipe2 failed");
		return false;
	}

	state->loop = pw_loop_new(nullptr);
	if (!state->loop) {
		log.errorf("pw_loop_new failed");
		return false;
	}

	state->context = pw_context_new(state->loop, nullptr, 0);
	if (!state->context) {
		log.errorf("pw_context_new failed");
		return false;
	}

	state->core = pw_context_connect(state->context, nullptr, 0);
	if (!state->core) {
		log.errorf("pw_context_connect failed");
		return false;
	}

	state->stream = pw_stream_new(state->core, "gamescope",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			nullptr));
	if (!state->stream) {
		log.errorf("pw_stream_new failed");
		return false;
	}

	static struct spa_hook stream_hook;
	pw_stream_add_listener(state->stream, &stream_hook, &stream_events, state);

	uint8_t buf[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *format_param = get_format_param(&builder);

	enum pw_stream_flags flags = (enum pw_stream_flags)(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);
	int ret = pw_stream_connect(state->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, &format_param, 1);
	if (ret != 0) {
		log.errorf("pw_stream_connect failed");
		return false;
	}

	while (state->stream_node_id == SPA_ID_INVALID) {
		int ret = pw_loop_iterate(state->loop, -1);
		if (ret < 0) {
			log.errorf("pw_loop_iterate failed");
			return false;
		}
	}

	log.infof("stream available on node ID: %u", state->stream_node_id);

	std::thread thread(run_pipewire, state);
	thread.detach();

	return true;
}

uint32_t get_pipewire_stream_node_id(void)
{
	return pipewire_state.stream_node_id;
}

struct pipewire_buffer *dequeue_pipewire_buffer(void)
{
	return out_buffer.exchange(nullptr);
}

void push_pipewire_buffer(struct pipewire_buffer *buffer)
{
	struct pipewire_buffer *old = in_buffer.exchange(buffer);
	assert(old == nullptr);
	nudge_pipewire();
}

void nudge_pipewire(void)
{
	if (write(nudgePipe[1], "\n", 1) < 0)
		log.errorf_errno("nudge_pipewire: write failed");
}
