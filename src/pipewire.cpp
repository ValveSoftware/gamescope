
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

#include <spa/debug/format.h>

static LogScope pwr_log("pipewire");

static struct pipewire_state pipewire_state = { .stream_node_id = SPA_ID_INVALID };
static int nudgePipe[2] = { -1, -1 };

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
	{
		off_t size = buffer->shm.stride * buffer->video_info.size.height;
		if (buffer->video_info.format == SPA_VIDEO_FORMAT_NV12) {
			size += buffer->shm.stride * ((buffer->video_info.size.height + 1) / 2);
		}
		munmap(buffer->shm.data, size);
		close(buffer->shm.fd);
		break;
	}
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

static void build_format_params(struct spa_pod_builder *builder, spa_video_format format, std::vector<const struct spa_pod *> &params) {
	struct spa_rectangle size = SPA_RECTANGLE(s_nCaptureWidth, s_nCaptureHeight);
	struct spa_rectangle min_requested_size = { 0, 0 };
	struct spa_rectangle max_requested_size = { UINT32_MAX, UINT32_MAX };
	struct spa_fraction framerate = SPA_FRACTION(0, 1);
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

	struct spa_pod_frame obj_frame, choice_frame;
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
	spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
	spa_pod_builder_push_choice(builder, &choice_frame, SPA_CHOICE_Enum, 0);
	spa_pod_builder_long(builder, modifier); // default
	spa_pod_builder_long(builder, modifier);
	spa_pod_builder_pop(builder, &choice_frame);
	params.push_back((const struct spa_pod *) spa_pod_builder_pop(builder, &obj_frame));

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
	params.push_back((const struct spa_pod *) spa_pod_builder_pop(builder, &obj_frame));

//	for (auto& param : params)
//		spa_debug_format(2, nullptr, param);
}


static std::vector<const struct spa_pod *> build_format_params(struct spa_pod_builder *builder)
{
	std::vector<const struct spa_pod *> params;

	build_format_params(builder, SPA_VIDEO_FORMAT_BGRx, params);
	build_format_params(builder, SPA_VIDEO_FORMAT_NV12, params);

	return params;
}

static void request_buffer(struct pipewire_state *state)
{
	struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(state->stream);
	if (!pw_buffer) {
		pwr_log.errorf("warning: out of buffers");
		return;
	}

	struct pipewire_buffer *buffer = (struct pipewire_buffer *) pw_buffer->user_data;
	buffer->copying = true;

	// Past this exchange, the PipeWire thread shares the buffer with the
	// steamcompmgr thread
	struct pipewire_buffer *old = out_buffer.exchange(buffer);
	assert(old == nullptr);
}

static void copy_buffer(struct pipewire_state *state, struct pipewire_buffer *buffer)
{
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
		chunk->size = state->video_info.size.height * buffer->shm.stride;
		if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
			chunk->size += ((state->video_info.size.height + 1)/2 * buffer->shm.stride);
		}
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
}

static void dispatch_nudge(struct pipewire_state *state, int fd)
{
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

	struct pipewire_buffer *buffer = in_buffer.exchange(nullptr);
	if (buffer != nullptr) {
		// We now completely own the buffer, it's no longer shared with the
		// steamcompmgr thread.

		buffer->copying = false;

		if (buffer->buffer != nullptr) {
			copy_buffer(state, buffer);

			int ret = pw_stream_queue_buffer(state->stream, buffer->buffer);
			if (ret < 0) {
				pwr_log.errorf("pw_stream_queue_buffer failed");
			}
		} else {
			destroy_buffer(buffer);
		}
	}
}

static void stream_handle_state_changed(void *data, enum pw_stream_state old_stream_state, enum pw_stream_state stream_state, const char *error)
{
	struct pipewire_state *state = (struct pipewire_state *) data;

	pwr_log.infof("stream state changed: %s", pw_stream_state_as_string(stream_state));

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

	int buffers = 4;
	int shm_size = state->shm_stride * state->video_info.size.height;
	if (state->video_info.format == SPA_VIDEO_FORMAT_NV12) {
		shm_size += ((state->video_info.size.height + 1) / 2) * state->shm_stride;
	}
	int data_type = state->dmabuf ? (1 << SPA_DATA_DmaBuf) : (1 << SPA_DATA_MemFd);

	const struct spa_pod *buffers_param =
		(const struct spa_pod *) spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, 1, 8),
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

uint32_t spa_format_to_drm(uint32_t spa_format)
{
	switch (spa_format)
	{
		case SPA_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
		default:
		case SPA_VIDEO_FORMAT_BGR: return DRM_FORMAT_XRGB8888;
	}
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

	EStreamColorspace colorspace = k_EStreamColorspace_Unknown;
	switch (state->video_info.color_matrix) {
	case SPA_VIDEO_COLOR_MATRIX_BT601:
		switch (state->video_info.color_range) {
		case SPA_VIDEO_COLOR_RANGE_16_235:
			colorspace = k_EStreamColorspace_BT601;
			break;
		case SPA_VIDEO_COLOR_RANGE_0_255:
			colorspace = k_EStreamColorspace_BT601_Full;
			break;
		default:
			break;
		}
		break;
	case SPA_VIDEO_COLOR_MATRIX_BT709:
		switch (state->video_info.color_range) {
		case SPA_VIDEO_COLOR_RANGE_16_235:
			colorspace = k_EStreamColorspace_BT709;
			break;
		case SPA_VIDEO_COLOR_RANGE_0_255:
			colorspace = k_EStreamColorspace_BT709_Full;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

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

		off_t size = state->shm_stride * state->video_info.size.height;
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
	.process = nullptr,
};

enum pipewire_event_type {
	EVENT_PIPEWIRE,
	EVENT_NUDGE,
	EVENT_COUNT // keep last
};

static void run_pipewire(struct pipewire_state *state)
{
	pthread_setname_np( pthread_self(), "gamescope-pw" );

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

	while (state->running) {
		int ret = poll(pollfds, EVENT_COUNT, -1);
		if (ret < 0) {
			pwr_log.errorf_errno("poll failed");
			break;
		}

		if (pollfds[EVENT_PIPEWIRE].revents & POLLHUP) {
			pwr_log.errorf("lost connection to server");
			break;
		}

		assert(!(pollfds[EVENT_NUDGE].revents & POLLHUP));

		if (pollfds[EVENT_PIPEWIRE].revents & POLLIN) {
			ret = pw_loop_iterate(state->loop, -1);
			if (ret < 0) {
				pwr_log.errorf("pw_loop_iterate failed");
				break;
			}
		}

		if (pollfds[EVENT_NUDGE].revents & POLLIN) {
			dispatch_nudge(state, nudgePipe[0]);
		}
	}

	pwr_log.infof("exiting");
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
		pwr_log.errorf_errno("pipe2 failed");
		return false;
	}

	state->loop = pw_loop_new(nullptr);
	if (!state->loop) {
		pwr_log.errorf("pw_loop_new failed");
		return false;
	}

	state->context = pw_context_new(state->loop, nullptr, 0);
	if (!state->context) {
		pwr_log.errorf("pw_context_new failed");
		return false;
	}

	state->core = pw_context_connect(state->context, nullptr, 0);
	if (!state->core) {
		pwr_log.errorf("pw_context_connect failed");
		return false;
	}

	state->stream = pw_stream_new(state->core, "gamescope",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			nullptr));
	if (!state->stream) {
		pwr_log.errorf("pw_stream_new failed");
		return false;
	}

	static struct spa_hook stream_hook;
	pw_stream_add_listener(state->stream, &stream_hook, &stream_events, state);

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

	state->running = true;
	while (state->stream_node_id == SPA_ID_INVALID) {
		int ret = pw_loop_iterate(state->loop, -1);
		if (ret < 0) {
			pwr_log.errorf("pw_loop_iterate failed");
			return false;
		}
	}

	pwr_log.infof("stream available on node ID: %u", state->stream_node_id);

	std::thread thread(run_pipewire, state);
	thread.detach();

	return true;
}

uint32_t get_pipewire_stream_node_id(void)
{
	return pipewire_state.stream_node_id;
}

bool pipewire_is_streaming()
{
	struct pipewire_state *state = &pipewire_state;
	return state->streaming;
}

struct pipewire_buffer *dequeue_pipewire_buffer(void)
{
	struct pipewire_state *state = &pipewire_state;
	if (state->streaming) {
		request_buffer(state);
	}
	return out_buffer.exchange(nullptr);
}

void push_pipewire_buffer(struct pipewire_buffer *buffer)
{
	struct pipewire_buffer *old = in_buffer.exchange(buffer);
	if ( old != nullptr )
	{
		pwr_log.errorf_errno("push_pipewire_buffer: Already had a buffer?!");
	}
	nudge_pipewire();
}

void nudge_pipewire(void)
{
	if (write(nudgePipe[1], "\n", 1) < 0)
		pwr_log.errorf_errno("nudge_pipewire: write failed");
}
