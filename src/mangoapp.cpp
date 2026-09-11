#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
    uint8_t upscaler; // GamescopeUpscaleFilter that produced the frame, LINEAR when none did
    uint8_t wantedUpscaler; // GamescopeUpscaleFilter the user selected

    // WARNING: Always ADD fields, never remove or repurpose fields
} __attribute__((packed));

void init_mangoapp(){
    int key = ftok("mangoapp", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);
    inited = true;
}

static std::mutex s_FrameTimeMutex;
static std::unordered_map<uint32_t, uint64_t> s_LastFrameTimes;

// A queue message, mtype first, sized past any control message mangoapp defines.
struct MangoappRawMsg_t
{
    long mtype;
    uint8_t data[1016];
};
// The control layout is folded straight out of one of these.
static_assert(offsetof(MangoappRawMsg_t, data) == sizeof(long));
static_assert(sizeof(MangoappRawMsg_t) >= sizeof(mangoapp_ctrl_msgid1_v1));

// The queue outlives gamescope and types start over every run, so leave nothing behind.
void mangoapp_drop_stream( uint32_t uMsgType )
{
    if (!inited)
        init_mangoapp();

    {
        // Ahead of the drain, so commits finishing later find nothing to send.
        std::unique_lock lock( s_SnapshotMutex );
        s_ConnectorSnapshots.erase( uMsgType );
    }

    MangoappRawMsg_t rawMsg;
    for (uint32_t uType : { uMsgType, MangoappControlMsgType(uMsgType) })
        while (msgrcv(msgid, &rawMsg, sizeof(rawMsg.data), uType, IPC_NOWAIT | MSG_NOERROR) >= 0)
            ;

    std::unique_lock lock( s_FrameTimeMutex );
    s_LastFrameTimes.erase( uMsgType );
}

void mangoapp_set_connector_snapshots( std::unordered_map<uint32_t, MangoappSnapshot_t> snapshots )
{
    std::unique_lock lock( s_SnapshotMutex );
    s_ConnectorSnapshots = std::move( snapshots );
}

// Sends that did not fit, finished in order before the next message. Steamcompmgr thread only.
struct MangoappPendingControl_t
{
    uint32_t uMsgType;
    size_t size;
    MangoappRawMsg_t msg;
};
static std::vector<MangoappPendingControl_t> s_PendingControl;

// System V hands each message to one reader, so fan it out to every instance.
// Drains even with no instance, so the caller still sees what was asked.
// Sends still waiting for room, in order. Returns how many remain.
uint32_t mangoapp_flush_control( const std::vector<uint32_t> &msgTypes )
{
    if (!inited)
        init_mangoapp();

    while (!s_PendingControl.empty())
    {
        const MangoappPendingControl_t &pending = s_PendingControl.front();
        bool bConnectorGone = std::find(msgTypes.begin(), msgTypes.end(), pending.uMsgType) == msgTypes.end();
        if (!bConnectorGone && msgsnd(msgid, &pending.msg, pending.size, IPC_NOWAIT) < 0)
            return (uint32_t) s_PendingControl.size();
        s_PendingControl.erase(s_PendingControl.begin());
    }
    return 0;
}

MangoappControlRelay_t mangoapp_relay_control( const std::vector<uint32_t> &msgTypes )
{
    MangoappControlRelay_t relay;

    // Finish the last fan-out first, so no instance sees commands out of order.
    relay.uHeldBack = mangoapp_flush_control( msgTypes );
    if (relay.uHeldBack)
        return relay;

    uint32_t uDeferred = 0;
    MangoappRawMsg_t rawMsg;
    for (;;)
    {
        ssize_t size = msgrcv(msgid, &rawMsg, sizeof(rawMsg.data), k_uMangoappControlMsgType, IPC_NOWAIT | MSG_NOERROR);
        if (size < 0)
            break;

        // Truncated, so fanning it out would hand every instance a corrupt message.
        if (size == ssize_t(sizeof(rawMsg.data)))
            continue;

        mangoapp_fold_control(&rawMsg, sizeof(long) + size, relay);

        // A type nobody reads yet holds the message until that instance starts.
        for (uint32_t uMsgType : msgTypes)
        {
            rawMsg.mtype = MangoappControlMsgType(uMsgType);
            // Once one send does not fit, hold the rest rather than deliver them ahead of it.
            if (uDeferred || msgsnd(msgid, &rawMsg, size, IPC_NOWAIT) < 0)
            {
                s_PendingControl.push_back( MangoappPendingControl_t{ uMsgType, size_t(size), rawMsg } );
                uDeferred++;
            }
        }

        if (uDeferred)
            break;
    }
    relay.uHeldBack = uDeferred;
    return relay;
}

// A fresh instance starts on the file alone, so what mangohudctl asked goes over its own control type.
void mangoapp_post_control( uint32_t uMsgType, uint8_t uNoDisplay, bool bStartLogging )
{
    if (!inited)
        init_mangoapp();

    mangoapp_ctrl_msgid1_v1 ctrl = {};
    ctrl.hdr.msg_type = MangoappControlMsgType(uMsgType);
    ctrl.hdr.ctrl_msg_type = 1;
    ctrl.hdr.version = 1;
    ctrl.no_display = uNoDisplay;
    ctrl.log_session = bStartLogging ? 1 : 0;

    MangoappPendingControl_t pending = { .uMsgType = uMsgType, .size = sizeof(ctrl) - sizeof(long) };
    memcpy(&pending.msg, &ctrl, sizeof(ctrl));
    // Behind anything still held back, so no instance sees commands out of order.
    if (!s_PendingControl.empty() || msgsnd(msgid, &pending.msg, pending.size, IPC_NOWAIT) < 0)
        s_PendingControl.push_back(pending);

    // An untagged mangoapp reads the shared type instead. A tagged one never does, so the relay
    // hands this copy back to it, and both commands are sets, so the repeat changes nothing.
    pending.msg.mtype = k_uMangoappControlMsgType;
    if (!s_PendingControl.empty() || msgsnd(msgid, &pending.msg, pending.size, IPC_NOWAIT) < 0)
        s_PendingControl.push_back(pending);
}

void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns, uint32_t uMsgType ) {
    if (!inited)
        init_mangoapp();

    MangoappSnapshot_t snapshot;
    if ( uMsgType == k_uMangoappLegacyMsgType )
    {
        snapshot.eActiveUpscaler = g_eActiveUpscaler;
        snapshot.eWantedUpscaler = g_eWantedUpscaler;
        snapshot.uFSRSharpness = (uint8_t) g_nActiveUpscaleSharpness;
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
    msg.fsrUpscale = snapshot.eActiveUpscaler == GamescopeUpscaleFilter::FSR;
    msg.upscaler = uint8_t( snapshot.eActiveUpscaler );
    msg.wantedUpscaler = uint8_t( snapshot.eWantedUpscaler );
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
