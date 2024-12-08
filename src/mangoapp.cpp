#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <mqueue.h>
#include <cstring>

#include "steamcompmgr.hpp"
#include "refresh_rate.h"
#include "main.hpp"

static bool inited = false;
static int msgid = 0;
static mqd_t mangoapp_mq = 0;
extern bool g_bAppWantsHDRCached;
extern uint32_t g_focusedBaseAppId;
extern const char *g_sMangoappMqName;

struct mangoapp_msg_header {
    long msg_type;  // Message queue ID, never change
    uint32_t version;  // for major changes in the way things work //
} __attribute__((packed));

struct mangoapp_msg_v1 {
    struct mangoapp_msg_header hdr;

    uint32_t pid;
    uint64_t app_frametime_ns;
    uint8_t fsrUpscale;
    uint8_t fsrSharpness;
    uint64_t visible_frametime_ns;
    uint64_t latency_ns;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint16_t displayRefresh;
    bool bAppWantsHDR : 1;
    bool bSteamFocused : 1;
    
    // WARNING: Always ADD fields, never remove or repurpose fields
} __attribute__((packed)) mangoapp_msg_v1;

void init_mangoapp(){
    // WARNING: "mangoapp" most likely isn't in the working directory so key will be -1 (0xffffffff)
    // TODO: Deprecate SysV message queues
    key_t key = ftok("mangoapp", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);
    struct mq_attr attrs = {
        .mq_flags = O_NONBLOCK,
        .mq_maxmsg = 4,
        .mq_msgsize = sizeof( mangoapp_msg_v1 ),
    };
    if ( g_sMangoappMqName != nullptr ) {
        mq_unlink( g_sMangoappMqName );
        mangoapp_mq = mq_open( g_sMangoappMqName, O_CREAT | O_EXCL | O_WRONLY | O_NONBLOCK, S_IRUSR | S_IWUSR, &attrs );
        if (mangoapp_mq == -1)
            console_log.errorf("Failed to create mangoapp message queue at %s %d (%s)", g_sMangoappMqName, errno, strerror(errno));
    }

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
    mangoapp_msg_v1.displayRefresh = (uint16_t) gamescope::ConvertmHzToHz( g_nOutputRefresh );
    mangoapp_msg_v1.bAppWantsHDR = g_bAppWantsHDRCached;
    mangoapp_msg_v1.bSteamFocused = g_focusedBaseAppId == 769;
    if (mangoapp_mq > 0 && mq_send( mangoapp_mq, (const char *)&mangoapp_msg_v1, sizeof( struct mangoapp_msg_v1 ), 0 ) == -1 && errno != EAGAIN) {
        console_log.errorf( "Failed to send message to mangoapp %d: %s", errno, strerror( errno ) );
    }
    msgsnd(msgid, &mangoapp_msg_v1, sizeof(mangoapp_msg_v1) - sizeof(mangoapp_msg_v1.hdr.msg_type), IPC_NOWAIT);
}

extern uint64_t g_uCurrentBasePlaneCommitID;
extern bool g_bCurrentBasePlaneIsFifo;
void mangoapp_output_update( uint64_t vblanktime )
{
    if ( !g_bCurrentBasePlaneIsFifo )
    {
        return;
    }

	static uint64_t s_uLastBasePlaneCommitID = 0;
	if ( s_uLastBasePlaneCommitID != g_uCurrentBasePlaneCommitID )
	{
		static uint64_t s_uLastBasePlaneUpdateVBlankTime = vblanktime;
        uint64_t last_frametime = s_uLastBasePlaneUpdateVBlankTime;
        uint64_t frametime = vblanktime - last_frametime;
		s_uLastBasePlaneUpdateVBlankTime = vblanktime;
		s_uLastBasePlaneCommitID = g_uCurrentBasePlaneCommitID;
        if ( last_frametime > vblanktime )
            return;
		mangoapp_update( frametime, uint64_t(~0ull), uint64_t(~0ull) );
	}
}
