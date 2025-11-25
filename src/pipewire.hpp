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
 * shared with the steamcompmgr thread (via pipewire_dequeue_buffer and
 * pipewire_push_buffer) for copying.
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

bool pipewire_init();
void pipewire_exit();
uint32_t pipewire_get_stream_node_id();
struct pipewire_buffer *pipewire_dequeue_buffer();
void pipewire_destroy_buffer(struct pipewire_buffer *buffer);
struct pipewire_buffer *pipewire_push_buffer(struct pipewire_buffer *buffer);
void pipewire_nudge();
