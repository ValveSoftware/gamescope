#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <cstring>

#include "steamcompmgr.hpp"
#include "main.hpp"

static bool inited = false;
static int msgid = 0;

struct mangoapp_msg_header {
    long msg_type;  // Message queue ID, never change
    uint32_t version;  // for major changes in the way things work //
} __attribute__((packed));

struct mangoapp_msg_v1 {
    struct mangoapp_msg_header hdr;

    uint32_t pid;
    uint64_t visible_frametime_ns;
    uint8_t fsrUpscale;
    uint8_t fsrSharpness;
    uint64_t app_frametime_ns;
    uint64_t latency_ns;
    uint32_t outputWidth;
    uint32_t outputHeight;
    // WARNING: Always ADD fields, never remove or repurpose fields
} __attribute__((packed)) mangoapp_msg_v1;

void init_mangoapp(){
    int key = ftok("mangoapp", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);
    mangoapp_msg_v1.hdr.msg_type = 1;
    mangoapp_msg_v1.hdr.version = 1;
    mangoapp_msg_v1.fsrUpscale = 0;
    mangoapp_msg_v1.fsrSharpness = 0;
    inited = true;
}

void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns ) {
    if (!inited)
        init_mangoapp();

    mangoapp_msg_v1.visible_frametime_ns = visible_frametime;
    mangoapp_msg_v1.fsrUpscale = g_bFSRActive;
    mangoapp_msg_v1.fsrSharpness = g_upscaleFilterSharpness;
    mangoapp_msg_v1.app_frametime_ns = app_frametime_ns;
    mangoapp_msg_v1.latency_ns = latency_ns;
    mangoapp_msg_v1.pid = focusWindow_pid;
    mangoapp_msg_v1.outputWidth = g_nOutputWidth;
    mangoapp_msg_v1.outputHeight = g_nOutputHeight;
    msgsnd(msgid, &mangoapp_msg_v1, sizeof(mangoapp_msg_v1) - sizeof(mangoapp_msg_v1.hdr.msg_type), IPC_NOWAIT);
}
