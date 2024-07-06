#pragma once

#include <memory>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include "rendervulkan.hpp"
#include "pipewire_gamescope.hpp"

struct pipewire_state {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	bool running;

	struct pw_stream *stream;
	uint32_t stream_node_id;
	std::atomic<bool> streaming;
	struct spa_video_info_raw video_info;
	struct spa_gamescope gamescope_info;
	uint64_t focus_appid;
	bool dmabuf;
	int shm_stride;
	uint64_t seq;
};

/**
 * PipeWire buffers are allocated by the PipeWire thread, and are temporarily
 * shared with the steamcompmgr thread (via dequeue_pipewire_buffer and
 * push_pipewire_buffer) for copying.
 */
struct pipewire_buffer {
	enum spa_data_type type; // SPA_DATA_MemFd or SPA_DATA_DmaBuf
	struct spa_video_info_raw video_info;
	struct spa_gamescope gamescope_info;
	gamescope::OwningRc<CVulkanTexture> texture;

	// Only used for SPA_DATA_MemFd
	struct {
		int stride;
		uint8_t *data;
		int fd;
	} shm;

	// The following fields are not thread-safe

	// The PipeWire buffer, or nullptr if it's been destroyed.
	std::atomic<struct pw_buffer *> buffer;
	bool IsStale() const 
	{
		return buffer == nullptr;
	}
	// We pass the buffer to the steamcompmgr thread for copying. This is set
	// to true if the buffer is currently owned by the steamcompmgr thread.
	bool copying;
};

bool init_pipewire(void);
uint32_t get_pipewire_stream_node_id(void);
struct pipewire_buffer *dequeue_pipewire_buffer(void);
bool pipewire_is_streaming();
void pipewire_destroy_buffer(struct pipewire_buffer *buffer);
void push_pipewire_buffer(struct pipewire_buffer *buffer);
void nudge_pipewire(void);
