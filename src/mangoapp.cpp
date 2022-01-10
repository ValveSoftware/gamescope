#include <sys/ipc.h>
#include <unistd.h>
#include <sys/msg.h>
#include <cstring>

#include "steamcompmgr.hpp"
static bool inited = false;
static int msgid = 0;
uint64_t now, last_frametime = 0;

struct mangoapp_msg_header {
    long msg_type;  // Message queue ID, never change
    uint32_t version;  // for major changes in the way things work //
} __attribute__((packed));

struct mangoapp_msg_v1 {
    struct mangoapp_msg_header hdr;
    
    uint32_t pid;
    uint64_t frametime_ns;
    // WARNING: Always ADD fields, never remove or repurpose fields
} __attribute__((packed)) mangoapp_msg_v1;

void init_mangoapp(){
    int key = ftok("mangoapp", 65);
    msgid = msgget(key, 0666 | IPC_CREAT);
    mangoapp_msg_v1.hdr.msg_type = 1;
    mangoapp_msg_v1.hdr.version = 1;
    inited = true;
}

void mangoapp_update(){
    if (!inited)
        init_mangoapp();

    now = get_time_in_nanos();
    mangoapp_msg_v1.frametime_ns = now - last_frametime;
    last_frametime = now;
    msgsnd(msgid, &mangoapp_msg_v1, sizeof(mangoapp_msg_v1), IPC_NOWAIT);
}
