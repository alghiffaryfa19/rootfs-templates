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
#include <stdbool.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <evdi_lib.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DRM_EVDI_CONNECT 0x00
#define DRM_EVDI_POLL 0x04
#define DRM_EVDI_GET_BUFF_CALLBACK 0x08
#define DRM_EVDI_DESTROY_BUFF_CALLBACK 0x09
#define DRM_EVDI_GBM_CREATE_BUFF_CALLBACK 0x0D

#define DRM_IOCTL_EVDI_CONNECT DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_CONNECT, struct drm_evdi_connect)
#define DRM_IOCTL_EVDI_POLL DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_POLL, struct drm_evdi_poll)
#define DRM_IOCTL_EVDI_GET_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GET_BUFF_CALLBACK, struct drm_evdi_get_buff_callabck)
#define DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_DESTROY_BUFF_CALLBACK, struct drm_evdi_destroy_buff_callback)
#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GBM_CREATE_BUFF_CALLBACK, struct drm_evdi_create_buff_callabck)

enum poll_event_type {
    none,
    add_buf,
    get_buf,
    destroy_buf,
    swap_to,
    create_buf
};

struct drm_evdi_connect {
    int32_t connected;
    int32_t dev_index;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
    uint32_t display_id;
};

struct drm_evdi_destroy_buff_callback {
    int poll_id;
};

struct drm_evdi_poll {
    enum poll_event_type event;
    int poll_id;
    void *data;
};

struct drm_evdi_gbm_create_buff {
    int *id;
    uint32_t *stride;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct drm_evdi_create_buff_callabck {
    int poll_id;
    int id;
    uint32_t stride;
};

struct drm_evdi_get_buff_callabck {
    int poll_id;
    int version;
    int numFds;
    int numInts;
    int *fd_ints;
    int *data_ints;
};

static int drm_ioctl(int fd, unsigned long req, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

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

static volatile int g_connected = 0;
static int g_evdi_fd = -1;
static int fence_fd = -1;
static volatile uint32_t *shm_ptr = NULL;

static void *client_watchdog(void *arg) {
    int client_sock = *(int *)arg;
    char dummy[64];
    while (1) {
        ssize_t n = recv(client_sock, dummy, sizeof(dummy), 0);
        if (n <= 0) break;
    }
    printf("[evdi-bridge] Android app disconnected or connection lost.\n");
    g_connected = 0;
    if (g_evdi_fd >= 0) {
        close(g_evdi_fd);
        g_evdi_fd = -1;
    }
    return NULL;
}

static int recv_fds(int sock, void *data, size_t data_len, int *fds, int fd_count, int *fds_received) {
    struct iovec iov = { .iov_base = data, .iov_len  = data_len };
    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf)
    };

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

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int main() {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    printf("[evdi-bridge] Starting EVDI to Android display bridge...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[evdi-bridge] socket creation failed");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/var/display_daemon.sock", sizeof(addr.sun_path) - 1);
    unlink("/var/display_daemon.sock");
    unlink("/tmp/display_daemon.sock");

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        strncpy(addr.sun_path, "/tmp/display_daemon.sock", sizeof(addr.sun_path) - 1);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("[evdi-bridge] Failed to bind to /var or /tmp display_daemon.sock");
            return 1;
        }
    }
    chmod(addr.sun_path, 0777);
    listen(sock, 1);

    printf("[evdi-bridge] Listening on %s. Waiting for Android app...\n", addr.sun_path);

    while (1) {
        int client_sock = accept(sock, NULL, NULL);
        if (client_sock < 0) {
            perror("[evdi-bridge] accept failed");
            continue;
        }
        printf("[evdi-bridge] Connected to Android app!\n");

        struct ctrl_msg hello = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
        write(client_sock, &hello, sizeof(hello));

        // 1. Receive CONSUMER_HELLO and connection FDs from Android
        struct ctrl_msg msg_buf;
        int conn_fds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        int conn_fds_received = 0;
        if (recv_fds(client_sock, &msg_buf, sizeof(msg_buf), conn_fds, 8, &conn_fds_received) <= 0 || conn_fds_received < 4) {
            printf("[evdi-bridge] Failed to receive CONSUMER_HELLO (received %d FDs)\n", conn_fds_received);
            close(client_sock);
            continue;
        }

        int efd      = conn_fds[0];
        fence_fd     = conn_fds[1];
        int data_fd  = conn_fds[2];
        int shm_fd   = conn_fds[3];
        int audio_fd = (conn_fds_received > 4) ? conn_fds[4] : -1;

        printf("[evdi-bridge] Got connection FDs: data_fd=%d, shm_fd=%d, fence_fd=%d, audio_fd=%d\n",
               data_fd, shm_fd, fence_fd, audio_fd);

        shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) {
            perror("[evdi-bridge] Failed to mmap shm_fd");
            shm_ptr = NULL;
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            if (efd >= 0) close(efd);
            if (audio_fd >= 0) close(audio_fd);
            continue;
        }

        // 2. Receive CTRL_MSG_SCREEN_INFO from client_sock
        struct {
            struct ctrl_msg hdr;
            struct screen_info info;
        } __attribute__((packed)) sinfo_msg;
        memset(&sinfo_msg, 0, sizeof(sinfo_msg));

        if (recv_all(client_sock, &sinfo_msg, sizeof(sinfo_msg)) < 0) {
            printf("[evdi-bridge] Failed to read SCREEN_INFO from client_sock\n");
            munmap((void *)shm_ptr, sizeof(uint32_t));
            shm_ptr = NULL;
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            if (efd >= 0) close(efd);
            if (audio_fd >= 0) close(audio_fd);
            continue;
        }
        printf("[evdi-bridge] Screen info: %ux%u (format: %u, refresh: %u mHz)\n",
               sinfo_msg.info.width, sinfo_msg.info.height, sinfo_msg.info.format, sinfo_msg.info.refresh);

        // 3. Send CTRL_MSG_FDS_READY back so Android sends the DMA-BUFs
        struct ctrl_msg fds_ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
        write(client_sock, &fds_ready, sizeof(fds_ready));

        // 4. Receive DATA_MSG_BUFS_READY and DMA-BUF FDs on data_fd
        struct data_msg dmsg;
        int dma_fds[MAX_BUFS] = {0};
        int dma_fds_received = 0;

        printf("[evdi-bridge] Waiting for DMA-BUFs on data_fd...\n");
        if (recv_fds(data_fd, &dmsg, sizeof(dmsg), dma_fds, MAX_BUFS, &dma_fds_received) <= 0 || dma_fds_received <= 0) {
            printf("[evdi-bridge] Failed to receive DMA-BUFs on data_fd (received %d FDs)\n", dma_fds_received);
            munmap((void *)shm_ptr, sizeof(uint32_t));
            shm_ptr = NULL;
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            if (efd >= 0) close(efd);
            if (audio_fd >= 0) close(audio_fd);
            continue;
        }

        // 5. Read buf_info array from data_fd
        struct buf_info infos[MAX_BUFS];
        memset(infos, 0, sizeof(infos));
        if (dmsg.size == 0 || dmsg.size > sizeof(infos) || recv_all(data_fd, infos, dmsg.size) < 0) {
            printf("[evdi-bridge] Failed to read buf_infos from data_fd\n");
            for (int i = 0; i < dma_fds_received; i++) close(dma_fds[i]);
            munmap((void *)shm_ptr, sizeof(uint32_t));
            shm_ptr = NULL;
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            if (efd >= 0) close(efd);
            if (audio_fd >= 0) close(audio_fd);
            continue;
        }

        printf("[evdi-bridge] Received %d DMA-BUFs! Dimensions: %ux%u, Stride: %u bytes\n",
               dma_fds_received, infos[0].width, infos[0].height, infos[0].stride);

        uint32_t *mapped_bufs[MAX_BUFS] = {NULL};
        size_t map_sizes[MAX_BUFS] = {0};
        for (int i = 0; i < dma_fds_received; i++) {
            size_t calc_size = infos[i].stride * infos[i].height;
            off_t real_size = lseek(dma_fds[i], 0, SEEK_END);
            map_sizes[i] = (real_size > 0) ? (size_t)real_size : calc_size;

            mapped_bufs[i] = mmap(NULL, map_sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, dma_fds[i], 0);
            if (mapped_bufs[i] == MAP_FAILED) {
                perror("[evdi-bridge] mmap DMA-BUF warning");
                mapped_bufs[i] = NULL;
            }
        }

        // 6. Open EVDI device
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
            printf("[evdi-bridge] FATAL: Failed to open EVDI device.\n");
            for (int i = 0; i < dma_fds_received; i++) {
                if (mapped_bufs[i]) munmap(mapped_bufs[i], map_sizes[i]);
                close(dma_fds[i]);
            }
            munmap((void *)shm_ptr, sizeof(uint32_t));
            shm_ptr = NULL;
            close(client_sock); close(data_fd); close(shm_fd); close(fence_fd);
            if (efd >= 0) close(efd);
            if (audio_fd >= 0) close(audio_fd);
            continue;
        }
        printf("[evdi-bridge] EVDI device %d opened successfully!\n", evdi_idx);

        int evdi_fd = evdi_get_event_ready(evdi);
        g_evdi_fd = evdi_fd;

        uint32_t disp_w = (sinfo_msg.info.width > 0) ? sinfo_msg.info.width : infos[0].width;
        uint32_t disp_h = (sinfo_msg.info.height > 0) ? sinfo_msg.info.height : infos[0].height;
        uint32_t disp_hz = (sinfo_msg.info.refresh > 0) ? (sinfo_msg.info.refresh / 1000) : 60;
        if (disp_hz == 0) disp_hz = 60;

        struct drm_evdi_connect cmd = {
            .connected = 1,
            .dev_index = 0,
            .width = disp_w,
            .height = disp_h,
            .refresh_rate = disp_hz,
            .display_id = 0
        };
        if (drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_CONNECT, &cmd) < 0) {
            perror("[evdi-bridge] EVDI_CONNECT failed");
        }
        printf("[evdi-bridge] Connected virtual display %ux%u@%uHz\n", disp_w, disp_h, disp_hz);

        // Notify udev and display manager that virtual display is active
        int sys_ret = system("udevadm trigger --subsystem-match=drm 2>/dev/null; systemctl restart sddm 2>/dev/null &");
        (void)sys_ret;

        // 7. Start watchdog thread to monitor client socket for disconnect
        g_connected = 1;
        pthread_t watchdog_th;
        pthread_create(&watchdog_th, NULL, client_watchdog, &client_sock);

        int buffer_assignment_index = 0;
        printf("[evdi-bridge] Bridge loop running. Waiting for EVDI events...\n");

        while (g_connected) {
            struct drm_evdi_poll poll_cmd = {};
            uint8_t poll_payload[32] = {0};
            poll_cmd.data = poll_payload;

            if (drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_POLL, &poll_cmd) < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (!g_connected) break;

            if (poll_cmd.event == create_buf) {
                struct drm_evdi_gbm_create_buff params;
                memcpy(&params, poll_payload, sizeof(params));
                int assigned_id = buffer_assignment_index % dma_fds_received;
                buffer_assignment_index++;
                struct drm_evdi_create_buff_callabck cb = {
                    .poll_id = poll_cmd.poll_id,
                    .id = assigned_id,
                    .stride = infos[assigned_id].stride
                };
                drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK, &cb);
            } else if (poll_cmd.event == get_buf) {
                int requested_id = -1;
                memcpy(&requested_id, poll_payload, sizeof(requested_id));
                if (requested_id >= 0 && requested_id < dma_fds_received) {
                    int fd_ints[1] = { dma_fds[requested_id] };
                    struct drm_evdi_get_buff_callabck cb = {
                        .poll_id = poll_cmd.poll_id,
                        .version = 1,
                        .numFds = 1,
                        .numInts = 0,
                        .fd_ints = fd_ints,
                        .data_ints = NULL
                    };
                    drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_GET_BUFF_CALLBACK, &cb);
                }
            } else if (poll_cmd.event == destroy_buf) {
                struct drm_evdi_destroy_buff_callback cb = { .poll_id = poll_cmd.poll_id };
                drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK, &cb);
            } else if (poll_cmd.event == swap_to) {
                if (fence_fd >= 0) {
                    char dummy = 1;
                    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
                    struct msghdr fmsg = {
                        .msg_iov = &iov,
                        .msg_iovlen = 1,
                        .msg_control = NULL,
                        .msg_controllen = 0
                    };
                    sendmsg(fence_fd, &fmsg, 0);
                }
            }
        }

        g_connected = 0;
        pthread_join(watchdog_th, NULL);

        if (g_evdi_fd >= 0) {
            struct drm_evdi_connect dis = {0};
            drm_ioctl(evdi_fd, DRM_IOCTL_EVDI_CONNECT, &dis);
            evdi_close(evdi);
            g_evdi_fd = -1;
        }

        // Cleanup resources
        for (int i = 0; i < dma_fds_received; i++) {
            if (mapped_bufs[i]) munmap(mapped_bufs[i], map_sizes[i]);
            close(dma_fds[i]);
        }
        if (shm_ptr) {
            munmap((void *)shm_ptr, sizeof(uint32_t));
            shm_ptr = NULL;
        }
        close(client_sock);
        close(data_fd);
        close(shm_fd);
        if (fence_fd >= 0) { close(fence_fd); fence_fd = -1; }
        if (efd >= 0) close(efd);
        if (audio_fd >= 0) close(audio_fd);

        printf("[evdi-bridge] Cleaned up session. Waiting for next connection...\n");
    }

    close(sock);
    unlink(addr.sun_path);
    return 0;
}
