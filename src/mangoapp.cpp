#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "steamcompmgr.hpp"
#include "refresh_rate.h"
#include "main.hpp"

static bool inited = false;
static int msgid = 0;
static std::mutex s_SnapshotMutex;
static std::unordered_map<uint32_t, MangoappSnapshot_t> s_ConnectorSnapshots;
extern bool g_bAppWantsHDRCached;
extern uint32_t g_focusedBaseAppId;

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
    char engineName[40];

    // WARNING: Always ADD fields, never remove or repurpose fields
} __attribute__((packed));

void init_mangoapp(){
    int key = ftok("mangoapp", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);
    inited = true;
}

void mangoapp_set_connector_snapshots( std::unordered_map<uint32_t, MangoappSnapshot_t> snapshots )
{
    std::unique_lock lock( s_SnapshotMutex );
    s_ConnectorSnapshots = std::move( snapshots );
}

void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns, uint32_t uMsgType ) {
    if (!inited)
        init_mangoapp();

    MangoappSnapshot_t snapshot;
    if ( uMsgType == k_uMangoappLegacyMsgType )
    {
        snapshot.bFSRActive = g_bFSRActive;
        snapshot.uFSRSharpness = (uint8_t) g_upscaleFilterSharpness;
        snapshot.nPid = focusWindow_pid;
        snapshot.uOutputWidth = g_nOutputWidth;
        snapshot.uOutputHeight = g_nOutputHeight;
        snapshot.nOutputRefreshmHz = g_nOutputRefresh;
        snapshot.bAppWantsHDR = g_bAppWantsHDRCached;
        snapshot.bSteamFocused = g_focusedBaseAppId == 769;
        snapshot.pEngineName = focusWindow_engine;
    }
    else
    {
        std::unique_lock lock( s_SnapshotMutex );
        auto iter = s_ConnectorSnapshots.find( uMsgType );
        if ( iter == s_ConnectorSnapshots.end() )
            return;
        snapshot = iter->second;
    }

    struct mangoapp_msg_v1 msg = {};
    msg.hdr.version = 1;
    msg.hdr.msg_type = uMsgType;
    msg.visible_frametime_ns = visible_frametime;
    msg.app_frametime_ns = app_frametime_ns;
    msg.latency_ns = latency_ns;
    msg.fsrUpscale = snapshot.bFSRActive;
    msg.fsrSharpness = snapshot.uFSRSharpness;
    msg.pid = snapshot.nPid;
    msg.outputWidth = snapshot.uOutputWidth;
    msg.outputHeight = snapshot.uOutputHeight;
    msg.displayRefresh = (uint16_t) gamescope::ConvertmHzToHz( snapshot.nOutputRefreshmHz );
    msg.bAppWantsHDR = snapshot.bAppWantsHDR;
    msg.bSteamFocused = snapshot.bSteamFocused;
    if (snapshot.pEngineName)
        snapshot.pEngineName->copy(msg.engineName, sizeof(msg.engineName) / sizeof(char));
    else
        std::string("gamescope").copy(msg.engineName, sizeof(msg.engineName) / sizeof(char));

    msgsnd(msgid, &msg, sizeof(msg) - sizeof(msg.hdr.msg_type), IPC_NOWAIT);
}

void mangoapp_nudge_app_frame( uint32_t uMsgType, uint64_t ulNow )
{
    static std::mutex s_FrameTimeMutex;
    static std::unordered_map<uint32_t, uint64_t> s_LastFrameTimes;

    uint64_t frametime;
    {
        std::unique_lock lock( s_FrameTimeMutex );
        auto iter = s_LastFrameTimes.find( uMsgType );
        frametime = ( iter != s_LastFrameTimes.end() ) ? ulNow - iter->second : 0;
        s_LastFrameTimes[ uMsgType ] = ulNow;
    }
    mangoapp_update( uint64_t(~0ull), frametime, uint64_t(~0ull), uMsgType );
}

extern uint64_t g_uCurrentBasePlaneCommitID;
extern bool g_bCurrentBasePlaneIsFifo;
extern uint32_t g_uCurrentBasePlaneAppID;
extern gamescope::ConVar<bool> cv_mangoapp_use_output_timing;

void mangoapp_output_update( uint64_t vblanktime )
{
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

        if ( cv_mangoapp_use_output_timing )
        {
            wlserver_lock();
            wlserver_app_presented( g_uCurrentBasePlaneAppID, frametime );
            wlserver_unlock();
        }
	}
}
