#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <evdi_lib.h>

#define CTRL_MSG_CONSUMER_HELLO  1
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_FDS_READY      10
#define DATA_MSG_BUFS_READY      200

#define MAX_BUFS 8

struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

struct data_msg {
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

struct buf_info {
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
} __attribute__((packed));

// Global state
int fence_fd = -1;
volatile uint32_t *shm_ptr = NULL;

static void dpms_handler(int dpms_mode, void *user_data) {
    printf("[evdi-bridge] DPMS changed: %d\n", dpms_mode);
}

static void mode_changed_handler(struct evdi_mode mode, void *user_data) {
    printf("[evdi-bridge] Mode changed: %dx%d@%d\n", mode.width, mode.height, mode.refresh_rate);
}

static void update_ready_handler(int buffer_to_be_updated, void *user_data) {
    if (fence_fd >= 0) {
        char dummy = 1;
        struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
        struct msghdr fmsg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = NULL, .msg_controllen = 0 };
        sendmsg(fence_fd, &fmsg, 0);
    }
}

static void crtc_state_handler(int state, void *user_data) {
    printf("[evdi-bridge] CRTC state changed: %d\n", state);
}

int recv_fds(int sock, void *data, size_t data_len, int *fds, int fd_count, int *fds_received) {
    struct iovec iov = { .iov_base = data, .iov_len  = data_len };
    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf) };

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    *fds_received = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (count > fd_count) count = fd_count;
        memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * count);
        *fds_received = count;
    }
    return (int)n;
}

int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int main(int argc, char **argv) {
    printf("[evdi-bridge] Starting EVDI bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/var/display_daemon.sock", sizeof(addr.sun_path) - 1);
    unlink("/var/display_daemon.sock");
    
    unlink("/tmp/display_daemon.sock");
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        strncpy(addr.sun_path, "/tmp/display_daemon.sock", sizeof(addr.sun_path) - 1);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("[evdi-bridge] Failed to bind");
            return 1;
        }
    }
    chmod(addr.sun_path, 0777);
    listen(sock, 1);
    
    printf("[evdi-bridge] Listening on %s. Waiting for Android app...\n", addr.sun_path);
    int client_sock = accept(sock, NULL, NULL);
    if (client_sock < 0) return 1;
    printf("[evdi-bridge] Connected to Android app!\n");

    struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    write(client_sock, &hello, sizeof(hello));

    // Wait for CONSUMER_HELLO and FDs
    struct ctrl_msg msg_buf;
    int conn_fds[8];
    int conn_fds_received = 0;
    if (recv_fds(client_sock, &msg_buf, sizeof(msg_buf), conn_fds, 8, &conn_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive CONSUMER_HELLO\n");
        return 1;
    }
    
    int efd       = conn_fds[0];
    fence_fd      = conn_fds[1];
    int data_fd   = conn_fds[2];
    int shm_fd    = conn_fds[3];

    printf("[evdi-bridge] Got connection FDs. data_fd=%d, shm_fd=%d, fence_fd=%d\n", data_fd, shm_fd, fence_fd);
    
    shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("[evdi-bridge] Failed to mmap shm");
        return 1;
    }

    struct ctrl_msg fds_ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
    write(client_sock, &fds_ready, sizeof(fds_ready));

    // Wait for DMA-BUFs on data_fd
    struct data_msg dmsg;
    int dma_fds[MAX_BUFS];
    int dma_fds_received = 0;
    
    printf("[evdi-bridge] Waiting for DMA-BUFs on data_fd...\n");
    if (recv_fds(data_fd, &dmsg, sizeof(dmsg), dma_fds, MAX_BUFS, &dma_fds_received) <= 0) {
        printf("[evdi-bridge] Failed to receive DMA-BUFs\n");
        return 1;
    }

    struct buf_info infos[MAX_BUFS];
    if (recv_all(data_fd, infos, dmsg.size) < 0) {
        printf("[evdi-bridge] Failed to read buf_infos\n");
        return 1;
    }

    printf("[evdi-bridge] Received %d DMA-BUFs! Real Size: %dx%d (Stride: %d bytes)\n", 
            dma_fds_received, infos[0].width, infos[0].height, infos[0].stride);

    // Mmap DMA-BUFs
    uint32_t *mapped_bufs[MAX_BUFS];
    for (int i = 0; i < dma_fds_received; i++) {
        size_t calc_size = infos[i].stride * infos[i].height;
        off_t real_size = lseek(dma_fds[i], 0, SEEK_END);
        size_t map_size = (real_size > 0) ? (size_t)real_size : calc_size;
        
        mapped_bufs[i] = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fds[i], 0);
        if (mapped_bufs[i] == MAP_FAILED) {
            perror("[evdi-bridge] Failed to mmap DMA-BUF");
            return 1;
        }
    }
    
    // Now send the dynamic SCREEN_INFO back to Android so it matches what we want!
    // But Android has already created the DMA-BUFs. We'll just echo it back just in case.
    struct {
        struct ctrl_msg hdr;
        struct screen_info info;
    } __attribute__((packed)) sinfo = {
        .hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) },
        .info = { .width = infos[0].width, .height = infos[0].height, .format = 1, .refresh = 60000 }
    };
    write(client_sock, &sinfo, sizeof(sinfo));

    // Initialize EVDI
    evdi_handle evdi = EVDI_INVALID_HANDLE;
    int evdi_idx = -1;
    for (int i = 0; i < 10; i++) {
        evdi = evdi_open(i);
        if (evdi != EVDI_INVALID_HANDLE) {
            evdi_idx = i;
            break;
        }
    }
    
    if (evdi == EVDI_INVALID_HANDLE) {
        printf("[evdi-bridge] FATAL: Failed to open EVDI device. Is lindroid-drm-evdi loaded?\n");
        return 1;
    }
    printf("[evdi-bridge] EVDI device %d opened successfully!\n", evdi_idx);

    // Connect EVDI (using Lindroid parameters)
    evdi_connect(evdi, infos[0].width, infos[0].height, 60, 1);

    // Register our DMA-BUFs as EVDI buffers
    struct evdi_rect rects[16];
    for (int i = 0; i < dma_fds_received; i++) {
        struct evdi_buffer ev_buf;
        ev_buf.id = i;
        ev_buf.buffer = mapped_bufs[i];
        ev_buf.width = infos[i].width;
        ev_buf.height = infos[i].height;
        ev_buf.stride = infos[i].stride;
        ev_buf.rects = rects;
        ev_buf.rect_count = 16;
        
        evdi_register_buffer(evdi, ev_buf);
        printf("[evdi-bridge] Registered EVDI buffer %d (%dx%d stride %d)\n", i, ev_buf.width, ev_buf.height, ev_buf.stride);
    }

    struct evdi_event_context evtctx = {
        .dpms_handler = dpms_handler,
        .mode_changed_handler = mode_changed_handler,
        .update_ready_handler = update_ready_handler,
        .crtc_state_handler = crtc_state_handler,
        .user_data = NULL
    };

    printf("[evdi-bridge] Bridge loop started. Waiting for EVDI/Android events...\n");

    int evdi_fd = evdi_get_event_ready(evdi);
    
    struct pollfd pfds[2];
    pfds[0].fd = efd;       // Android tells us which buffer is free
    pfds[0].events = POLLIN;
    pfds[1].fd = evdi_fd;   // EVDI tells us when frame is ready
    pfds[1].events = POLLIN;

    while (1) {
        if (poll(pfds, 2, -1) < 0) {
            perror("[evdi-bridge] Poll error");
            break;
        }

        if (pfds[1].revents & POLLIN) {
            evdi_handle_events(evdi, &evtctx);
        }

        if (pfds[0].revents & POLLIN) {
            uint64_t efd_val;
            if (read(efd, &efd_val, sizeof(efd_val)) > 0) {
                uint32_t selected_idx = *shm_ptr;
                // Kernel minta update. Kasih frame terbaru.
                // Lindroid EVDI tidak mendukung request_update (mereturn EINVAL).
                // Kita langsung grab_pixels saja seperti create-disp.
                // if (!evdi_request_update(evdi, selected_idx)) {
                //     update_ready_handler(selected_idx, NULL);
                // }
                struct evdi_rect r[16];
                int n_rects = 16;
                evdi_grab_pixels(evdi, r, &n_rects);
                
                if (n_rects == 0) { // grabpix failed
                    // Let's find out what the actual DRM resolution is!
                    int fd = open("/dev/dri/card1", O_RDWR);
                    if (fd >= 0) {
                        drmModeRes *res = drmModeGetResources(fd);
                        if (res) {
                            for (int i = 0; i < res->count_crtcs; i++) {
                                drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[i]);
                                if (crtc && crtc->mode_valid) {
                                    printf("[evdi-bridge] KWayland is using CRTC %d: %dx%d (mode: %s)\n", 
                                           crtc->crtc_id, crtc->width, crtc->height, crtc->mode.name);
                                    drmModeFreeCrtc(crtc);
                                }
                            }
                            drmModeFreeResources(res);
                        }
                        close(fd);
                    }
                }
                
                // Beritahu Android bahwa kita (sudah mencoba) mengisi frame
                uint64_t val = 1;
                write(infos[selected_idx].fence_fd, &val, sizeof(val));
            }
        }
    }

    evdi_disconnect(evdi);
    evdi_close(evdi);
    return 0;
}
