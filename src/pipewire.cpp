
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "main.hpp"
#include "pipewire.hpp"
#include "log.hpp"

#include <spa/debug/format.h>

static LogScope pwr_log("pipewire");

static struct pipewire_state pipewire_state = { .stream_node_id = SPA_ID_INVALID };

// Pending buffer for PipeWire → steamcompmgr
static std::atomic<struct pipewire_buffer *> out_buffer;
// Pending buffer for steamcompmgr → PipeWire
static std::atomic<struct pipewire_buffer *> in_buffer;

// Requested capture size
static uint32_t s_nRequestedWidth;
static uint32_t s_nRequestedHeight;
static uint32_t s_nCaptureWidth;
static uint32_t s_nCaptureHeight;
static uint32_t s_nOutputWidth;
static uint32_t s_nOutputHeight;

static void destroy_buffer(struct pipewire_buffer *buffer) {
	assert(buffer->buffer == nullptr);

	switch (buffer->type) {
	case SPA_DATA_MemFd:
		munmap(buffer->shm.data, buffer->shm.size);
		close(buffer->shm.fd);
		break;
	case SPA_DATA_DmaBuf:
		break; // nothing to do
	default:
		assert(false); // unreachable
	}	

	// If out_buffer == buffer, then set it to nullptr.
	// We don't care about the result.
	struct pipewire_buffer *buffer1 = buffer;
	out_buffer.compare_exchange_strong(buffer1, nullptr);
	struct pipewire_buffer *buffer2 = buffer;
	in_buffer.compare_exchange_strong(buffer2, nullptr);

	delete buffer;
}

void pipewire_destroy_buffer(struct pipewire_buffer *buffer)
{
	destroy_buffer(buffer);
}

static void calculate_capture_size()
{
	s_nCaptureWidth = s_nOutputWidth;
	s_nCaptureHeight = s_nOutputHeight;

	if (s_nRequestedWidth > 0 && s_nRequestedHeight > 0 &&
	    (s_nOutputWidth > s_nRequestedWidth || s_nOutputHeight > s_nRequestedHeight)) {
		// Need to clamp to the smallest dimension
		float flRatioW = static_cast<float>(s_nRequestedWidth) / s_nOutputWidth;
		float flRatioH = static_cast<float>(s_nRequestedHeight) / s_nOutputHeight;
		if (flRatioW <= flRatioH) {
			s_nCaptureWidth = s_nRequestedWidth;
			s_nCaptureHeight = static_cast<uint32_t>(ceilf(flRatioW * s_nOutputHeight));
		} else {
			s_nCaptureWidth = static_cast<uint32_t>(ceilf(flRatioH * s_nOutputWidth));
			s_nCaptureHeight = s_nRequestedHeight;
		}
	}
}

static const struct spa_pod *build_format_params(struct spa_pod_builder *builder, const enum spa_video_format format, const std::span<const uint64_t>& modifiers)
{
	struct spa_rectangle size = SPA_RECTANGLE(s_nCaptureWidth, s_nCaptureHeight);
	struct spa_rectangle min_requested_size = { 0, 0 };
	struct spa_rectangle max_requested_size = { UINT32_MAX, UINT32_MAX };
	struct spa_fraction framerate = SPA_FRACTION(0, 1);

	struct spa_pod_frame obj_frame;
	spa_pod_builder_push_object(builder, &obj_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(format),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerate),
		SPA_FORMAT_VIDEO_requested_size, SPA_POD_CHOICE_RANGE_Rectangle( &min_requested_size, &min_requested_size, &max_requested_size ),
		SPA_FORMAT_VIDEO_gamescope_focus_appid, SPA_POD_CHOICE_RANGE_Long( 0ll, INT64_MIN, INT64_MAX ),
		0);
	if (format == SPA_VIDEO_FORMAT_NV12) {
		spa_pod_builder_add(builder,
			SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_CHOICE_ENUM_Id(3,
							SPA_VIDEO_COLOR_MATRIX_BT601,
							SPA_VIDEO_COLOR_MATRIX_BT601,
							SPA_VIDEO_COLOR_MATRIX_BT709),
			SPA_FORMAT_VIDEO_colorRange, SPA_POD_CHOICE_ENUM_Id(3,
							SPA_VIDEO_COLOR_RANGE_16_235,
							SPA_VIDEO_COLOR_RANGE_16_235,
							SPA_VIDEO_COLOR_RANGE_0_255),
			0);
	}
	if (modifiers.size() == 1) {
		// Pre-fixate if there is only one modifier
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(builder, modifiers[0]);
	} else if (modifiers.size() > 0) {
		struct spa_pod_frame choice_frame;
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(builder, &choice_frame, SPA_CHOICE_Enum, 0);
		spa_pod_builder_long(builder, modifiers[0]); // default, but should be ignored because of FLAG_DONT_FIXATE
		for (const uint64_t modifier : modifiers) {
			spa_pod_builder_long(builder, modifier);
		}
		spa_pod_builder_pop(builder, &choice_frame);
	}
	return (const struct spa_pod *) spa_pod_builder_pop(builder, &obj_frame);
}


static std::vector<const struct spa_pod *> build_format_params(struct spa_pod_builder *builder)
{
	std::vector<const struct spa_pod *> params;

	for (const enum spa_video_format format : {
		SPA_VIDEO_FORMAT_BGRx,
		SPA_VIDEO_FORMAT_NV12,
	}) {
		// TODO: Get supported modifiers for format
		std::vector<uint64_t> modifiers = { DRM_FORMAT_MOD_LINEAR };
		params.push_back(build_format_params(builder, format, modifiers));
		params.push_back(build_format_params(builder, format, {}));
	}

//	for (auto& param : params)
//		spa_debug_format(2, nullptr, param);
	return params;
}

static struct pipewire_buffer *dequeue_buffer(struct pipewire_state *state)
{
	struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(state->stream);
	if (!pw_buffer) {
		pwr_log.warnf("out of buffers");
		return nullptr;
	}
	struct pipewire_buffer *buffer = (struct pipewire_buffer *) pw_buffer->user_data;
	// Past this exchange, the PipeWire thread shares the buffer with the steamcompmgr thread
	buffer->copying = true;
	return buffer;
}

static void stream_handle_process(void *data)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	if (out_buffer == nullptr) {
		out_buffer = dequeue_buffer(state);
	}

	struct pipewire_buffer *buffer = in_buffer.exchange(nullptr);
	if (buffer == nullptr) {
		// Nothing was submitted
		return;
	}
	// We now completely own the buffer, it's no longer shared with the steamcompmgr thread.
	buffer->copying = false;

	if (buffer->IsStale()) {
		destroy_buffer(buffer);
		return;
	}

	gamescope::OwningRc<CVulkanTexture> &tex = buffer->texture;
	assert(tex != nullptr);

	struct pw_buffer *pw_buffer = buffer->buffer;
	struct spa_buffer *spa_buffer = pw_buffer->buffer;

	bool needs_reneg = buffer->video_info.size.width != tex->width() || buffer->video_info.size.height != tex->height();

	struct spa_meta_header *header = (struct spa_meta_header *) spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof(*header));
	if (header != nullptr) {
		header->pts = -1;
		header->flags = needs_reneg ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
		header->seq = state->seq++;
		header->dts_offset = 0;
	}

	float *requested_size_scale = (float *) spa_buffer_find_meta_data(spa_buffer, SPA_META_requested_size_scale, sizeof(*requested_size_scale));
	if (requested_size_scale != nullptr) {
		*requested_size_scale = ((float)tex->width() / g_nOutputWidth);
	}

	struct spa_chunk *chunk = spa_buffer->datas[0].chunk;
	chunk->flags = needs_reneg ? SPA_CHUNK_FLAG_CORRUPTED : 0;

	struct wlr_dmabuf_attributes dmabuf;
	switch (buffer->type) {
	case SPA_DATA_MemFd:
		chunk->offset = 0;
		chunk->size = buffer->shm.size;
		chunk->stride = buffer->shm.stride;

		if (!needs_reneg) {
			uint8_t *pMappedData = tex->mappedData();

			if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
				for (uint32_t i = 0; i < tex->height(); i++) {
					const uint32_t lumaPwOffset = 0;
					memcpy(
						&buffer->shm.data[lumaPwOffset      + i * buffer->shm.stride],
						&pMappedData     [tex->lumaOffset() + i * tex->lumaRowPitch()],
						std::min<size_t>(buffer->shm.stride, tex->lumaRowPitch()));
				}

				for (uint32_t i = 0; i < (tex->height() + 1) / 2; i++) {
					const uint32_t chromaPwOffset = tex->height() * buffer->shm.stride;
					memcpy(
						&buffer->shm.data[chromaPwOffset      + i * buffer->shm.stride],
						&pMappedData     [tex->chromaOffset() + i * tex->chromaRowPitch()],
						std::min<size_t>(buffer->shm.stride, tex->chromaRowPitch()));
				}
			}
			else
			{
				for (uint32_t i = 0; i < tex->height(); i++) {
					memcpy(
						&buffer->shm.data[i * buffer->shm.stride],
						&pMappedData     [i * tex->rowPitch()],
						std::min<size_t>(buffer->shm.stride, tex->rowPitch()));
				}
			}
		}
		break;
	case SPA_DATA_DmaBuf:
		dmabuf = tex->dmabuf();
		assert(dmabuf.n_planes == 1);
		chunk->offset = dmabuf.offset[0];
		chunk->stride = dmabuf.stride[0];
		chunk->size = dmabuf.height * chunk->stride;
		if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
			chunk->size += ((dmabuf.height + 1)/2 * chunk->stride);
		}
		break;
	default:
		assert(false); // unreachable
	}

	int ret = pw_stream_queue_buffer(state->stream, pw_buffer);
	if (ret < 0) {
		pwr_log.errorf("pw_stream_queue_buffer failed");
	}
}

static void on_nudge(void *data, int fd, uint32_t mask)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	while (true) {
		static char buf[1024];
		if (read(fd, buf, sizeof(buf)) < 0) {
			if (errno != EAGAIN)
				pwr_log.errorf_errno("dispatch_nudge: read failed");
			break;
		}
	}

	if (g_nOutputWidth != s_nOutputWidth || g_nOutputHeight != s_nOutputHeight) {
		s_nOutputWidth = g_nOutputWidth;
		s_nOutputHeight = g_nOutputHeight;
		calculate_capture_size();
	}
	if (s_nCaptureWidth != state->video_info.size.width || s_nCaptureHeight != state->video_info.size.height) {
		pwr_log.debugf("renegotiating stream params (size: %dx%d)", s_nCaptureWidth, s_nCaptureHeight);

		uint8_t buf[4096];
		struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		std::vector<const struct spa_pod *> format_params = build_format_params(&builder);
		int ret = pw_stream_update_params(state->stream, format_params.data(), format_params.size());
		if (ret < 0) {
			pwr_log.errorf("pw_stream_update_params failed");
		}
	}

	pw_stream_trigger_process(state->stream);
}

static void stream_handle_state_changed(void *data, enum pw_stream_state old_stream_state, enum pw_stream_state stream_state, const char *error)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	pwr_log.infof("stream state changed: %s", pw_stream_state_as_string(stream_state));

	state->streaming = stream_state == PW_STREAM_STATE_STREAMING;

	if (error != nullptr) {
		pwr_log.errorf("stream error: %s", error);
	}

	switch (stream_state) {
	case PW_STREAM_STATE_PAUSED:
		state->stream_node_id = pw_stream_get_node_id(state->stream);
		pwr_log.infof("stream available on node ID: %u", state->stream_node_id);
		state->seq = 0;
		break;
	case PW_STREAM_STATE_STREAMING:
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
	default:
		break;
	}
}

static void stream_handle_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	if (param == nullptr || id != SPA_PARAM_Format)
		return;

	struct spa_gamescope gamescope_info{};

	int ret = spa_format_video_raw_parse_with_gamescope(param, &state->video_info, &gamescope_info);
	if (ret < 0) {
		pwr_log.errorf("spa_format_video_raw_parse failed");
		return;
	}
	s_nRequestedWidth = gamescope_info.requested_size.width;
	s_nRequestedHeight = gamescope_info.requested_size.height;
	calculate_capture_size();

	state->gamescope_info = gamescope_info;

	int bpp = 4;
	if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
		bpp = 1;
	}

	state->shm_stride = SPA_ROUND_UP_N(state->video_info.size.width * bpp, 4);

	const struct spa_pod_prop *modifier_prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
	state->dmabuf = modifier_prop != nullptr;

	uint8_t buf[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	int shm_size = state->shm_stride * state->video_info.size.height;
	if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
		shm_size += ((state->video_info.size.height + 1) / 2) * state->shm_stride;
	}
	int data_type = state->dmabuf ? (1 << SPA_DATA_DmaBuf) : (1 << SPA_DATA_MemFd);

	const struct spa_pod *buffers_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(shm_size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(state->shm_stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_type));
	const struct spa_pod *meta_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	const struct spa_pod *scale_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_requested_size_scale),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(float)));
	const struct spa_pod *params[] = { buffers_param, meta_param, scale_param };

	ret = pw_stream_update_params(state->stream, params, sizeof(params) / sizeof(params[0]));
	if (ret != 0) {
		pwr_log.errorf("pw_stream_update_params failed");
	}

	pwr_log.debugf("format changed (size: %dx%d, requested %dx%d, format %d, stride %d, size: %d, dmabuf: %d)",
		state->video_info.size.width, state->video_info.size.height,
		s_nRequestedWidth, s_nRequestedHeight,
		state->video_info.format, state->shm_stride, shm_size, state->dmabuf);
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

static constexpr EStreamColorspace spa_color_to_gamescope(const struct spa_video_info_raw& video_info)
{
	switch (video_info.color_matrix) {
	case SPA_VIDEO_COLOR_MATRIX_BT601:
		switch (video_info.color_range) {
		case SPA_VIDEO_COLOR_RANGE_16_235: return k_EStreamColorspace_BT601;
		case SPA_VIDEO_COLOR_RANGE_0_255: return k_EStreamColorspace_BT601_Full;
		default: break;
		}
		break;
	case SPA_VIDEO_COLOR_MATRIX_BT709:
		switch (video_info.color_range) {
		case SPA_VIDEO_COLOR_RANGE_16_235: return k_EStreamColorspace_BT709;
		case SPA_VIDEO_COLOR_RANGE_0_255: return k_EStreamColorspace_BT709_Full;
		default: break;
		}
		break;
	default: break;
	}
	return k_EStreamColorspace_Unknown;
}

static void stream_handle_add_buffer(void *user_data, struct pw_buffer *pw_buffer)
{
	struct pipewire_state *state = (struct pipewire_state *) user_data;

	struct spa_buffer *spa_buffer = pw_buffer->buffer;
	struct spa_data *spa_data = &spa_buffer->datas[0];

	struct pipewire_buffer *buffer = new pipewire_buffer();
	buffer->buffer = pw_buffer;
	buffer->video_info = state->video_info;
	buffer->gamescope_info = state->gamescope_info;

	bool is_dmabuf = (spa_data->type & (1 << SPA_DATA_DmaBuf)) != 0;
	bool is_memfd = (spa_data->type & (1 << SPA_DATA_MemFd)) != 0;

	EStreamColorspace colorspace = spa_color_to_gamescope(state->video_info);
	uint32_t drmFormat = spa_format_to_drm(state->video_info.format);

	buffer->texture = new CVulkanTexture();
	CVulkanTexture::createFlags screenshotImageFlags;
	screenshotImageFlags.bMappable = true;
	screenshotImageFlags.bTransferDst = true;
	screenshotImageFlags.bStorage = true;
	if (is_dmabuf || drmFormat == DRM_FORMAT_NV12)
	{
		screenshotImageFlags.bExportable = true;
		screenshotImageFlags.bLinear = true; // TODO: support multi-planar DMA-BUF export via PipeWire
	}
	bool bImageInitSuccess = buffer->texture->BInit( s_nCaptureWidth, s_nCaptureHeight, 1u, drmFormat, screenshotImageFlags );
	if ( !bImageInitSuccess )
	{
		pwr_log.errorf("Failed to initialize pipewire texture");
		goto error;
	}
	buffer->texture->setStreamColorspace(colorspace);

	if (is_dmabuf) {
		const struct wlr_dmabuf_attributes dmabuf = buffer->texture->dmabuf();
		if (dmabuf.n_planes != 1)
		{
			pwr_log.errorf("dmabuf.n_planes != 1");
			goto error;
		}

		off_t size = lseek(dmabuf.fd[0], 0, SEEK_END);
		if (size < 0) {
			pwr_log.errorf_errno("lseek failed");
			goto error;
		}

		buffer->type = SPA_DATA_DmaBuf;

		spa_data->type = SPA_DATA_DmaBuf;
		spa_data->flags = SPA_DATA_FLAG_READABLE;
		spa_data->fd = dmabuf.fd[0];
		spa_data->mapoffset = dmabuf.offset[0];
		spa_data->maxsize = size;
		spa_data->data = nullptr;
	} else if (is_memfd) {
		int fd = anonymous_shm_open();
		if (fd < 0) {
			pwr_log.errorf("failed to create shm file");
			goto error;
		}

		size_t size = state->shm_stride * state->video_info.size.height;
		if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
			size += state->shm_stride * ((state->video_info.size.height + 1) / 2);
		}
		if (ftruncate(fd, size) != 0) {
			pwr_log.errorf_errno("ftruncate failed");
			close(fd);
			goto error;
		}

		void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (data == MAP_FAILED) {
			pwr_log.errorf_errno("mmap failed");
			close(fd);
			goto error;
		}

		buffer->type = SPA_DATA_MemFd;
		buffer->shm.size = size;
		buffer->shm.stride = state->shm_stride;
		buffer->shm.data = (uint8_t *) data;
		buffer->shm.fd = fd;

		spa_data->type = SPA_DATA_MemFd;
		spa_data->flags = SPA_DATA_FLAG_READABLE;
		spa_data->fd = fd;
		spa_data->mapoffset = 0;
		spa_data->maxsize = size;
		spa_data->data = data;
	} else {
		pwr_log.errorf("unsupported data type");
		spa_data->type = SPA_DATA_Invalid;
		goto error;
	}

	pw_buffer->user_data = buffer;

	return;

error:
	delete buffer;
}

static void stream_handle_remove_buffer(void *data, struct pw_buffer *pw_buffer)
{
	struct pipewire_buffer *buffer = (struct pipewire_buffer *) pw_buffer->user_data;

	buffer->buffer = nullptr;

	if (!buffer->copying) {
		destroy_buffer(buffer);
	}
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_handle_state_changed,
	.param_changed = stream_handle_param_changed,
	.add_buffer = stream_handle_add_buffer,
	.remove_buffer = stream_handle_remove_buffer,
	.process = stream_handle_process,
};

void pipewire_exit()
{
	struct pipewire_state *state = &pipewire_state;

	pwr_log.infof("exiting");

	pw_thread_loop_stop(state->loop);
	pw_loop_destroy_source(pw_thread_loop_get_loop(state->loop), state->nudge_source);
	close(state->nudge_fd);
	pw_stream_destroy(state->stream);
	pw_thread_loop_destroy(state->loop);
	pw_deinit();
}

bool pipewire_init()
{
	struct pipewire_state *state = &pipewire_state;

	pw_init(nullptr, nullptr);

	state->loop = pw_thread_loop_new("gamescope-pw", nullptr);
	if (!state->loop) {
		pwr_log.errorf("pw_thread_loop_new failed");
		return false;
	}
	pw_thread_loop_lock(state->loop);
	pw_thread_loop_start(state->loop);
	struct pw_loop *loop = pw_thread_loop_get_loop(state->loop);

	int nudgePipe[2];
	if (pipe2(nudgePipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		pwr_log.errorf_errno("pipe2 failed");
		return false;
	}
	state->nudge_fd = nudgePipe[1];

	state->nudge_source = pw_loop_add_io(loop, nudgePipe[0], SPA_IO_IN, true, on_nudge, state);
	if (state->nudge_source == nullptr) {
		pwr_log.errorf("pw_loop_add_io failed");
		return false;
	}

	state->stream = pw_stream_new_simple(
		loop,
		"gamescope",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			nullptr),
		&stream_events,
		state);
	if (!state->stream) {
		pwr_log.errorf("pw_stream_new_simple failed");
		return false;
	}

	s_nRequestedWidth = 0;
	s_nRequestedHeight = 0;
	s_nOutputWidth = g_nOutputWidth;
	s_nOutputHeight = g_nOutputHeight;
	calculate_capture_size();

	uint8_t buf[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	std::vector<const struct spa_pod *> format_params = build_format_params(&builder);

	enum pw_stream_flags flags = (enum pw_stream_flags)(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);
	int ret = pw_stream_connect(state->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, format_params.data(), format_params.size());
	if (ret != 0) {
		pwr_log.errorf("pw_stream_connect failed");
		return false;
	}

	pw_thread_loop_unlock(state->loop);
	return true;
}

uint32_t pipewire_get_stream_node_id()
{
	return pipewire_state.stream_node_id;
}

struct pipewire_buffer *pipewire_dequeue_buffer()
{
	struct pipewire_state *state = &pipewire_state;

	struct pipewire_buffer *buffer = out_buffer.exchange(nullptr);
	if (buffer == nullptr && state->streaming) {
		pw_thread_loop_lock(state->loop);
		buffer = dequeue_buffer(state);
		pw_thread_loop_unlock(state->loop);
	}
	return buffer;
}

struct pipewire_buffer *pipewire_push_buffer(struct pipewire_buffer *buffer)
{
	struct pipewire_buffer *old = in_buffer.exchange(buffer);
	pipewire_nudge();
	// This will be `nullptr` if the pipewire thread is keeping up, otherwise the
	// compositor should reuse this old (previous) buffer
	return old;
}

void pipewire_nudge()
{
	struct pipewire_state *state = &pipewire_state;

	if (write(state->nudge_fd, "\n", 1) < 0)
		pwr_log.errorf_errno("nudge: write failed");
}
