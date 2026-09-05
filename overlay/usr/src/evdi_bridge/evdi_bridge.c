#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

// Protocol definitions from Anland (display_protocol.h)
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_PICKUP_FDS      9

struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct screen_info {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t refresh;
} __attribute__((packed));

int main(int argc, char **argv) {
    printf("[evdi-bridge] Starting EVDI to Anland bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/display_daemon.sock", sizeof(addr.sun_path) - 1);

    unlink("/tmp/display_daemon.sock");
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[evdi-bridge] Failed to bind to /tmp/display_daemon.sock");
        return 1;
    }
    
    if (listen(sock, 1) < 0) {
        perror("[evdi-bridge] Failed to listen");
        return 1;
    }
    
    printf("[evdi-bridge] Listening on /tmp/display_daemon.sock. Waiting for Android app...\n");
    
    int client_sock = accept(sock, NULL, NULL);
    if (client_sock < 0) {
        perror("[evdi-bridge] Failed to accept");
        return 1;
    }

    printf("[evdi-bridge] Connected to Android app via display_daemon.sock!\n");

    // 1. Send PRODUCER_HELLO
    struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    write(client_sock, &hello, sizeof(hello));

    // 2. Send SCREEN_INFO (Tell Android what resolution EVDI is running at)
    struct {
        struct ctrl_msg hdr;
        struct screen_info info;
    } __attribute__((packed)) sinfo = {
        .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
        .info = { .width = 1080, .height = 2400, .format = 1, .refresh = 60000 }
    };
    write(client_sock, &sinfo, sizeof(sinfo));

    printf("[evdi-bridge] Handshake sent. Waiting for DMA-BUF FDs from Android...\n");

    // TODO: Implement recvmsg with SCM_RIGHTS to receive the FDs from Android
    // TODO: Implement libdrm/evdi code to read /dev/dri/card1 and copy to the received DMA-BUFs

    while(1) {
        sleep(1);
    }

    return 0;
}
