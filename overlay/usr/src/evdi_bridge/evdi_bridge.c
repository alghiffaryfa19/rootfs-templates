#define _GNU_SOURCE

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
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <errno.h>

/*
 * ============================================================
 * EVDI private ioctl ABI
 * ============================================================
 */

#define DRM_EVDI_CONNECT                 0x00
#define DRM_EVDI_POLL                    0x04
#define DRM_EVDI_GET_BUFF_CALLBACK       0x08
#define DRM_EVDI_DESTROY_BUFF_CALLBACK   0x09
#define DRM_EVDI_GBM_CREATE_BUFF_CALLBACK 0x0D

enum poll_event_type {
    none = 0,
    add_buf,
    get_buf,
    destroy_buf,
    swap_to,
    create_buf
};

struct drm_evdi_connect {
    int32_t  connected;
    int32_t  dev_index;
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

#define DRM_IOCTL_EVDI_CONNECT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_CONNECT, \
             struct drm_evdi_connect)

#define DRM_IOCTL_EVDI_POLL \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_POLL, \
             struct drm_evdi_poll)

#define DRM_IOCTL_EVDI_GET_BUFF_CALLBACK \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GET_BUFF_CALLBACK, \
             struct drm_evdi_get_buff_callabck)

#define DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_DESTROY_BUFF_CALLBACK, \
             struct drm_evdi_destroy_buff_callback)

#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GBM_CREATE_BUFF_CALLBACK, \
             struct drm_evdi_create_buff_callabck)


/*
 * ============================================================
 * Android <-> bridge protocol
 * ============================================================
 */

#define CTRL_MSG_CONSUMER_HELLO  1
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_FDS_READY      10

#define DATA_MSG_BUFS_READY     200

#define MAX_BUFS 8


struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t payload[];
} __attribute__((packed));


struct data_msg {
    uint32_t type;
    uint32_t size;
    uint8_t payload[];
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


/*
 * ============================================================
 * Global state
 * ============================================================
 */

static volatile int g_connected = 0;

static int g_evdi_fd = -1;
static int g_evdi_idx = -1;

static int fence_fd = -1;

static volatile uint32_t *shm_ptr = NULL;


/*
 * ============================================================
 * Utility
 * ============================================================
 */

static int drm_ioctl_retry(int fd, unsigned long req, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 &&
             (errno == EINTR || errno == EAGAIN));

    return ret;
}


static void close_fd(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}


/*
 * ============================================================
 * EVDI detection
 *
 * Only accept evdi-lindroid.
 * ============================================================
 */

static bool is_evdi_driver(int index)
{
    char path[256];

    snprintf(path,
             sizeof(path),
             "/sys/class/drm/card%d/device/uevent",
             index);

    FILE *f = fopen(path, "r");

    if (!f)
        return false;

    char line[256];
    bool found = false;

    while (fgets(line, sizeof(line), f)) {

        if (strstr(line, "DRIVER=evdi-lindroid") ||
            strstr(line, "DRIVER=evdi")) {

            found = true;
            break;
        }
    }

    fclose(f);

    return found;
}


static int evdi_open_direct(int index)
{
    if (!is_evdi_driver(index))
        return -1;

    char path[64];

    snprintf(path,
             sizeof(path),
             "/dev/dri/card%d",
             index);

    printf("[evdi-bridge] Opening %s...\n", path);

    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr,
                "[evdi-bridge] Failed to open %s: errno=%d (%s)\n",
                path,
                errno,
                strerror(errno));
        return -1;
    }

    printf("[evdi-bridge] Opened %s successfully (fd=%d)\n",
           path,
           fd);

    return fd;
}


static int find_evdi_device(int *out_idx)
{
    for (int i = 0; i < 10; i++) {

        printf("[evdi-bridge] Checking DRM card%d...\n", i);

        if (!is_evdi_driver(i))
            continue;

        int fd = evdi_open_direct(i);

        if (fd >= 0) {

            *out_idx = i;

            printf("[evdi-bridge] Found EVDI device at "
                   "/dev/dri/card%d\n",
                   i);

            return fd;
        }
    }

    return -1;
}


/*
 * ============================================================
 * Receive FDs via SCM_RIGHTS
 * ============================================================
 */

static int recv_fds(
    int sock,
    void *data,
    size_t data_len,
    int *fds,
    int fd_count,
    int *fds_received)
{
    struct iovec iov = {
        .iov_base = data,
        .iov_len = data_len
    };

    char cmsg_buf[CMSG_SPACE(sizeof(int) * fd_count)];

    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n;

    do {
        n = recvmsg(sock, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n <= 0)
        return -1;

    *fds_received = 0;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {

        if (cmsg->cmsg_level != SOL_SOCKET)
            continue;

        if (cmsg->cmsg_type != SCM_RIGHTS)
            continue;

        size_t bytes =
            cmsg->cmsg_len - CMSG_LEN(0);

        int count =
            (int)(bytes / sizeof(int));

        if (count > fd_count)
            count = fd_count;

        memcpy(
            fds,
            CMSG_DATA(cmsg),
            sizeof(int) * count
        );

        *fds_received = count;

        break;
    }

    return (int)n;
}


/*
 * ============================================================
 * Read exactly len bytes
 * ============================================================
 */

static int recv_all(
    int fd,
    void *buf,
    size_t len)
{
    uint8_t *p = buf;

    while (len > 0) {

        ssize_t n = read(fd, p, len);

        if (n < 0) {

            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        p += n;
        len -= n;
    }

    return 0;
}


/*
 * ============================================================
 * Watch Android client
 * ============================================================
 */

static void *client_watchdog(void *arg)
{
    int client_sock = *(int *)arg;

    char dummy[64];

    while (g_connected) {

        ssize_t n =
            recv(client_sock,
                 dummy,
                 sizeof(dummy),
                 0);

        if (n <= 0)
            break;
    }

    printf("[evdi-bridge] Android app disconnected "
           "or connection lost.\n");

    g_connected = 0;

    return NULL;
}


/*
 * ============================================================
 * EVDI disconnect
 * ============================================================
 */

static void evdi_disconnect(int fd, int dev_index)
{
    if (fd < 0)
        return;

    struct drm_evdi_connect cmd;

    memset(&cmd, 0, sizeof(cmd));

    cmd.connected = 0;
    cmd.dev_index = dev_index;
    cmd.display_id = 0;

    if (drm_ioctl_retry(
            fd,
            DRM_IOCTL_EVDI_CONNECT,
            &cmd) < 0) {

        fprintf(stderr,
                "[evdi-bridge] EVDI disconnect "
                "failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
    } else {

        printf("[evdi-bridge] EVDI display disconnected\n");
    }
}


/*
 * ============================================================
 * Main
 * ============================================================
 */

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf(
        "[evdi-bridge] Starting EVDI to Android "
        "display bridge...\n"
    );


    /*
     * --------------------------------------------------------
     * UNIX socket
     * --------------------------------------------------------
     */

    int sock =
        socket(AF_UNIX,
               SOCK_STREAM | SOCK_CLOEXEC,
               0);

    if (sock < 0) {

        perror(
            "[evdi-bridge] socket creation failed"
        );

        return 1;
    }


    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(
        addr.sun_path,
        "/tmp/display_daemon.sock",
        sizeof(addr.sun_path) - 1
    );

    unlink(addr.sun_path);


    if (bind(
            sock,
            (struct sockaddr *)&addr,
            sizeof(addr)) < 0) {

        perror(
            "[evdi-bridge] Failed to bind "
            "to /tmp/display_daemon.sock"
        );

        close(sock);

        return 1;
    }


    chmod(addr.sun_path, 0777);


    if (listen(sock, 1) < 0) {

        perror(
            "[evdi-bridge] listen failed"
        );

        close(sock);
        unlink(addr.sun_path);

        return 1;
    }


    printf(
        "[evdi-bridge] Listening on %s. "
        "Waiting for Android app...\n",
        addr.sun_path
    );


    /*
     * ========================================================
     * Accept loop
     * ========================================================
     */

    while (1) {

        int client_sock =
            accept4(
                sock,
                NULL,
                NULL,
                SOCK_CLOEXEC
            );

        if (client_sock < 0) {

            if (errno == EINTR)
                continue;

            perror(
                "[evdi-bridge] accept failed"
            );

            continue;
        }


        printf(
            "[evdi-bridge] Connected to Android app!\n"
        );


        /*
         * ----------------------------------------------------
         * Reset state
         * ----------------------------------------------------
         */

        g_connected = 0;
        g_evdi_fd = -1;
        g_evdi_idx = -1;
        fence_fd = -1;
        shm_ptr = NULL;


        /*
         * ----------------------------------------------------
         * Producer hello
         * ----------------------------------------------------
         */

        struct ctrl_msg hello;

        memset(&hello, 0, sizeof(hello));

        hello.type =
            CTRL_MSG_PRODUCER_HELLO;

        hello.size = 0;


        if (write(
                client_sock,
                &hello,
                sizeof(hello)) < 0) {

            perror(
                "[evdi-bridge] producer hello failed"
            );

            close(client_sock);
            continue;
        }


        /*
         * ----------------------------------------------------
         * Receive Android FDs
         * ----------------------------------------------------
         */

        struct ctrl_msg msg_buf;

        memset(
            &msg_buf,
            0,
            sizeof(msg_buf)
        );


        int conn_fds[8];

        for (int i = 0; i < 8; i++)
            conn_fds[i] = -1;


        int conn_fds_received = 0;


        if (recv_fds(
                client_sock,
                &msg_buf,
                sizeof(msg_buf),
                conn_fds,
                8,
                &conn_fds_received) <= 0 ||
            conn_fds_received < 4) {

            printf(
                "[evdi-bridge] Failed to receive "
                "CONSUMER_HELLO "
                "(received %d FDs)\n",
                conn_fds_received
            );

            for (int i = 0;
                 i < conn_fds_received;
                 i++) {

                close_fd(&conn_fds[i]);
            }

            close(client_sock);

            continue;
        }


        /*
         * Existing protocol:
         *
         * fd[0] = event fd
         * fd[1] = fence fd
         * fd[2] = data fd
         * fd[3] = shm fd
         * fd[4] = audio fd (optional)
         */

        int efd      = conn_fds[0];
        fence_fd     = conn_fds[1];
        int data_fd  = conn_fds[2];
        int shm_fd   = conn_fds[3];
        int audio_fd =
            (conn_fds_received > 4)
                ? conn_fds[4]
                : -1;


        printf(
            "[evdi-bridge] Got connection FDs: "
            "data_fd=%d, shm_fd=%d, "
            "fence_fd=%d, audio_fd=%d\n",
            data_fd,
            shm_fd,
            fence_fd,
            audio_fd
        );


        /*
         * ----------------------------------------------------
         * mmap shared memory
         * ----------------------------------------------------
         */

        shm_ptr =
            mmap(
                NULL,
                sizeof(uint32_t),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                shm_fd,
                0
            );


        if (shm_ptr == MAP_FAILED) {

            shm_ptr = NULL;

            perror(
                "[evdi-bridge] Failed to mmap shm_fd"
            );

            close_fd(&client_sock);
            close_fd(&data_fd);
            close_fd(&shm_fd);
            close_fd(&fence_fd);
            close_fd(&efd);
            close_fd(&audio_fd);

            continue;
        }


        /*
         * ----------------------------------------------------
         * Receive SCREEN_INFO
         * ----------------------------------------------------
         */

        struct {
            struct ctrl_msg hdr;
            struct screen_info info;
        } __attribute__((packed)) sinfo_msg;


        memset(
            &sinfo_msg,
            0,
            sizeof(sinfo_msg)
        );


        if (recv_all(
                client_sock,
                &sinfo_msg,
                sizeof(sinfo_msg)) < 0) {

            printf(
                "[evdi-bridge] Failed to read "
                "SCREEN_INFO\n"
            );

            goto session_cleanup;
        }


        printf(
            "[evdi-bridge] Screen info: "
            "%ux%u "
            "(format: %u, refresh: %u mHz)\n",
            sinfo_msg.info.width,
            sinfo_msg.info.height,
            sinfo_msg.info.format,
            sinfo_msg.info.refresh
        );


        /*
         * ----------------------------------------------------
         * Tell Android to send DMA-BUFs
         * ----------------------------------------------------
         */

        struct ctrl_msg fds_ready;

        memset(
            &fds_ready,
            0,
            sizeof(fds_ready)
        );

        fds_ready.type =
            CTRL_MSG_FDS_READY;


        if (write(
                client_sock,
                &fds_ready,
                sizeof(fds_ready)) < 0) {

            perror(
                "[evdi-bridge] FDS_READY failed"
            );

            goto session_cleanup;
        }


        /*
         * ----------------------------------------------------
         * Receive DMA-BUF FDs
         * ----------------------------------------------------
         */

        struct data_msg dmsg;

        memset(
            &dmsg,
            0,
            sizeof(dmsg)
        );


        int dma_fds[MAX_BUFS];

        for (int i = 0; i < MAX_BUFS; i++)
            dma_fds[i] = -1;


        int dma_fds_received = 0;


        printf(
            "[evdi-bridge] Waiting for DMA-BUFs "
            "on data_fd...\n"
        );


        if (recv_fds(
                data_fd,
                &dmsg,
                sizeof(dmsg),
                dma_fds,
                MAX_BUFS,
                &dma_fds_received) <= 0 ||
            dma_fds_received <= 0) {

            printf(
                "[evdi-bridge] Failed to receive "
                "DMA-BUFs "
                "(received %d FDs)\n",
                dma_fds_received
            );

            goto session_cleanup;
        }


        /*
         * ----------------------------------------------------
         * Receive buffer metadata
         * ----------------------------------------------------
         */

        struct buf_info infos[MAX_BUFS];

        memset(
            infos,
            0,
            sizeof(infos)
        );


        if (dmsg.size == 0 ||
            dmsg.size > sizeof(infos)) {

            printf(
                "[evdi-bridge] Invalid buf_info "
                "size: %u\n",
                dmsg.size
            );

            goto session_cleanup;
        }


        if (recv_all(
                data_fd,
                infos,
                dmsg.size) < 0) {

            printf(
                "[evdi-bridge] Failed to read "
                "buf_infos\n"
            );

            goto session_cleanup;
        }


        printf(
            "[evdi-bridge] Received %d DMA-BUFs!\n",
            dma_fds_received
        );


        printf(
            "[evdi-bridge] Buffer 0: "
            "%ux%u stride=%u format=0x%x "
            "modifier=0x%lx\n",
            infos[0].width,
            infos[0].height,
            infos[0].stride,
            infos[0].format,
            (unsigned long)infos[0].modifier
        );


        /*
         * ----------------------------------------------------
         * Map DMA-BUFs
         * ----------------------------------------------------
         */

        uint32_t *mapped_bufs[MAX_BUFS];

        size_t map_sizes[MAX_BUFS];

        int meta_fds[MAX_BUFS];


        memset(
            mapped_bufs,
            0,
            sizeof(mapped_bufs)
        );

        memset(
            map_sizes,
            0,
            sizeof(map_sizes)
        );


        for (int i = 0; i < MAX_BUFS; i++)
            meta_fds[i] = -1;


        for (int i = 0;
             i < dma_fds_received;
             i++) {

            map_sizes[i] =
                (size_t)infos[i].stride *
                infos[i].height;


            mapped_bufs[i] =
                mmap(
                    NULL,
                    map_sizes[i],
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    dma_fds[i],
                    0
                );


            if (mapped_bufs[i] == MAP_FAILED) {

                mapped_bufs[i] = NULL;

                fprintf(
                    stderr,
                    "[evdi-bridge] DMA-BUF %d mmap "
                    "failed: errno=%d (%s)\n",
                    i,
                    errno,
                    strerror(errno)
                );
            }


            /*
             * Qualcomm gralloc metadata fd
             */

            meta_fds[i] =
                memfd_create(
                    "gralloc_meta",
                    MFD_CLOEXEC
                );


            if (meta_fds[i] < 0) {

                fprintf(
                    stderr,
                    "[evdi-bridge] memfd_create "
                    "failed for buffer %d: "
                    "errno=%d (%s)\n",
                    i,
                    errno,
                    strerror(errno)
                );

            } else {

                if (ftruncate(
                        meta_fds[i],
                        65536) < 0) {

                    fprintf(
                        stderr,
                        "[evdi-bridge] ftruncate "
                        "meta_fd failed: "
                        "errno=%d (%s)\n",
                        errno,
                        strerror(errno)
                    );

                } else {

                    void *p =
                        mmap(
                            NULL,
                            65536,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            meta_fds[i],
                            0
                        );

                    if (p != MAP_FAILED) {

                        memset(
                            p,
                            0,
                            65536
                        );

                        munmap(
                            p,
                            65536
                        );
                    }
                }
            }
        }


        /*
         * ====================================================
         * Open EVDI
         * ====================================================
         */

        int evdi_fd =
            find_evdi_device(&g_evdi_idx);


        if (evdi_fd < 0) {

            printf(
                "[evdi-bridge] FATAL: "
                "Failed to open EVDI device.\n"
            );

            goto session_cleanup;
        }


        g_evdi_fd = evdi_fd;


        printf(
            "[evdi-bridge] Using "
            "/dev/dri/card%d "
            "(fd=%d)\n",
            g_evdi_idx,
            evdi_fd
        );


        /*
         * ====================================================
         * Determine DISPLAY mode
         *
         * IMPORTANT:
         *
         * stride != display width
         *
         * Example:
         *
         * display width = 3048
         * stride        = 13312 bytes
         * aligned width = 3328 pixels
         *
         * DRM/EVDI mode must use 3048,
         * not 3328.
         * ====================================================
         */

        uint32_t disp_w =
            (sinfo_msg.info.width > 0)
                ? sinfo_msg.info.width
                : infos[0].width;


        uint32_t disp_h =
            (sinfo_msg.info.height > 0)
                ? sinfo_msg.info.height
                : infos[0].height;


        uint32_t disp_hz =
            (sinfo_msg.info.refresh > 0)
                ? (sinfo_msg.info.refresh / 1000)
                : 60;


        if (disp_w == 0)
            disp_w = infos[0].width;


        if (disp_h == 0)
            disp_h = infos[0].height;


        if (disp_hz == 0)
            disp_hz = 60;


        /*
         * EVDI currently configured to max 60 Hz.
         */

        if (disp_hz > 60)
            disp_hz = 60;


        uint32_t stride_aligned_w =
            (infos[0].stride > 0)
                ? infos[0].stride / 4
                : disp_w;


        printf(
            "[evdi-bridge] Display mode:\n"
            "    logical : %ux%u@%uHz\n"
            "    stride  : %u bytes\n"
            "    buffer  : %ux%u\n",
            disp_w,
            disp_h,
            disp_hz,
            infos[0].stride,
            stride_aligned_w,
            infos[0].height
        );


        /*
         * ====================================================
         * Disconnect stale EVDI state
         * ====================================================
         */

        struct drm_evdi_connect dis;

        memset(
            &dis,
            0,
            sizeof(dis)
        );

        dis.connected = 0;
        dis.dev_index = g_evdi_idx;
        dis.display_id = 0;


        if (drm_ioctl_retry(
                evdi_fd,
                DRM_IOCTL_EVDI_CONNECT,
                &dis) < 0) {

            fprintf(
                stderr,
                "[evdi-bridge] Initial EVDI "
                "disconnect warning: "
                "errno=%d (%s)\n",
                errno,
                strerror(errno)
            );
        }


        usleep(50000);


        /*
         * ====================================================
         * CONNECT EVDI
         *
         * IMPORTANT:
         *
         * width = disp_w
         *
         * NOT stride / 4.
         * ====================================================
         */

        struct drm_evdi_connect cmd;

        memset(
            &cmd,
            0,
            sizeof(cmd)
        );


        cmd.connected    = 1;
        cmd.dev_index    = g_evdi_idx;
        cmd.width        = disp_w;
        cmd.height       = disp_h;
        cmd.refresh_rate = disp_hz;
        cmd.display_id   = 0;


        printf(
            "[evdi-bridge] Connecting EVDI: "
            "%ux%u@%uHz\n",
            cmd.width,
            cmd.height,
            cmd.refresh_rate
        );


        if (drm_ioctl_retry(
                evdi_fd,
                DRM_IOCTL_EVDI_CONNECT,
                &cmd) < 0) {

            fprintf(
                stderr,
                "[evdi-bridge] EVDI_CONNECT failed: "
                "errno=%d (%s)\n",
                errno,
                strerror(errno)
            );

            goto session_cleanup;
        }


        printf(
            "[evdi-bridge] Connected virtual display "
            "%ux%u@%uHz\n",
            cmd.width,
            cmd.height,
            cmd.refresh_rate
        );


        /*
         * ====================================================
         * Drop DRM master ONCE
         * ====================================================
         */

        if (drmDropMaster(evdi_fd) < 0) {

            fprintf(
                stderr,
                "[evdi-bridge] drmDropMaster warning: "
                "errno=%d (%s)\n",
                errno,
                strerror(errno)
            );

        } else {

            printf(
                "[evdi-bridge] DRM master dropped "
                "successfully\n"
            );
        }


        printf(
            "[evdi-bridge] Waiting for EVDI "
            "connector to become active...\n"
        );


        /*
         * ====================================================
         * Start watchdog
         * ====================================================
         */

        g_connected = 1;

        pthread_t watchdog_th;

        if (pthread_create(
                &watchdog_th,
                NULL,
                client_watchdog,
                &client_sock) != 0) {

            perror(
                "[evdi-bridge] pthread_create failed"
            );

            g_connected = 0;

            goto session_cleanup;
        }


        /*
         * ====================================================
         * EVDI event loop
         * ====================================================
         */

        int buffer_assignment_index = 0;


        printf(
            "[evdi-bridge] Bridge loop running. "
            "Waiting for EVDI events...\n"
        );


        while (g_connected) {

            struct drm_evdi_poll poll_cmd;

            memset(
                &poll_cmd,
                0,
                sizeof(poll_cmd)
            );


            uint8_t poll_payload[32];

            memset(
                poll_payload,
                0,
                sizeof(poll_payload)
            );


            poll_cmd.data =
                poll_payload;


            if (drm_ioctl_retry(
                    evdi_fd,
                    DRM_IOCTL_EVDI_POLL,
                    &poll_cmd) < 0) {

                if (errno == EINTR)
                    continue;


                fprintf(
                    stderr,
                    "[evdi-bridge] "
                    "EVDI_POLL failed: "
                    "errno=%d (%s)\n",
                    errno,
                    strerror(errno)
                );

                break;
            }


            if (!g_connected)
                break;


            /*
             * ------------------------------------------------
             * CREATE BUFFER
             * ------------------------------------------------
             */

            if (poll_cmd.event == create_buf) {

                struct drm_evdi_gbm_create_buff params;

                memset(
                    &params,
                    0,
                    sizeof(params)
                );


                memcpy(
                    &params,
                    poll_payload,
                    sizeof(params)
                );


                int assigned_id =
                    buffer_assignment_index %
                    dma_fds_received;


                buffer_assignment_index++;


                int bo_id =
                    assigned_id + 1;


                printf(
                    "[evdi-bridge] Event: create_buf "
                    "(format=0x%x, size=%ux%u) "
                    "-> bo_id=%d, stride=%u\n",
                    params.format,
                    params.width,
                    params.height,
                    bo_id,
                    infos[assigned_id].stride
                );


                struct drm_evdi_create_buff_callabck cb;

                memset(
                    &cb,
                    0,
                    sizeof(cb)
                );


                cb.poll_id =
                    poll_cmd.poll_id;

                cb.id =
                    bo_id;

                cb.stride =
                    infos[assigned_id].stride;


                if (drm_ioctl_retry(
                        evdi_fd,
                        DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK,
                        &cb) < 0) {

                    fprintf(
                        stderr,
                        "[evdi-bridge] "
                        "CREATE_BUFF_CALLBACK failed: "
                        "errno=%d (%s)\n",
                        errno,
                        strerror(errno)
                    );
                }
            }


            /*
             * ------------------------------------------------
             * GET BUFFER
             * ------------------------------------------------
             */

            else if (poll_cmd.event == get_buf) {

                int requested_id = -1;


                memcpy(
                    &requested_id,
                    poll_payload,
                    sizeof(requested_id)
                );


                int idx =
                    (requested_id > 0)
                        ? requested_id - 1
                        : requested_id;


                printf(
                    "[evdi-bridge] Event: get_buf "
                    "(bo_id=%d -> idx=%d)\n",
                    requested_id,
                    idx
                );


                if (idx >= 0 &&
                    idx < dma_fds_received) {

                    int fd_ints[2] = {
                        dma_fds[idx],
                        meta_fds[idx]
                    };


                    int buf_size =
                        (int)map_sizes[idx];


                    int buf_aligned_w =
                        (infos[idx].stride > 0)
                            ? (int)(
                                infos[idx].stride / 4
                              )
                            : (int)infos[idx].width;


                    int buf_aligned_h =
                        (int)infos[idx].height;


                    int unaligned_w =
                        (int)infos[idx].width;


                    int unaligned_h =
                        (int)infos[idx].height;


                    /*
                     * Qualcomm / msm gralloc compatible
                     * payload.
                     */

                    int data_ints[24] = {

                        0x676d736d, /* magic */

                        0,          /* flags */

                        buf_aligned_w,

                        buf_aligned_h,

                        unaligned_w,

                        unaligned_h,

                        1,          /* RGBA_8888 */

                        1,          /* layer count */

                        0,          /* reserved */

                        idx + 1,    /* id low */

                        0,          /* id high */

                        0x00000b00, /* usage low */

                        0,          /* usage high */

                        buf_size,

                        0,          /* offset */

                        0,          /* offset metadata */

                        0,          /* base low */

                        0,          /* base high */

                        0,          /* base metadata low */

                        0,          /* base metadata high */

                        0,          /* modifier low */

                        0,          /* modifier high */

                        0,          /* reserved size */

                        0           /* custom reserved */
                    };


                    struct drm_evdi_get_buff_callabck cb;

                    memset(
                        &cb,
                        0,
                        sizeof(cb)
                    );


                    cb.poll_id =
                        poll_cmd.poll_id;

                    cb.version =
                        12;

                    cb.numFds =
                        2;

                    cb.numInts =
                        24;

                    cb.fd_ints =
                        fd_ints;

                    cb.data_ints =
                        data_ints;


                    if (drm_ioctl_retry(
                            evdi_fd,
                            DRM_IOCTL_EVDI_GET_BUFF_CALLBACK,
                            &cb) < 0) {

                        fprintf(
                            stderr,
                            "[evdi-bridge] "
                            "GET_BUFF_CALLBACK failed: "
                            "errno=%d (%s)\n",
                            errno,
                            strerror(errno)
                        );

                    } else {

                        printf(
                            "[evdi-bridge] Sent handle "
                            "(fd=%d, meta=%d, size=%d)\n",
                            fd_ints[0],
                            fd_ints[1],
                            buf_size
                        );
                    }

                } else {

                    printf(
                        "[evdi-bridge] Warning: "
                        "get_buf invalid idx=%d\n",
                        idx
                    );
                }
            }


            /*
             * ------------------------------------------------
             * DESTROY BUFFER
             * ------------------------------------------------
             */

            else if (
                poll_cmd.event == destroy_buf) {

                printf(
                    "[evdi-bridge] Event: destroy_buf "
                    "(poll_id=%d)\n",
                    poll_cmd.poll_id
                );


                struct drm_evdi_destroy_buff_callback cb;

                memset(
                    &cb,
                    0,
                    sizeof(cb)
                );


                cb.poll_id =
                    poll_cmd.poll_id;


                if (drm_ioctl_retry(
                        evdi_fd,
                        DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK,
                        &cb) < 0) {

                    fprintf(
                        stderr,
                        "[evdi-bridge] "
                        "DESTROY_BUFF_CALLBACK failed: "
                        "errno=%d (%s)\n",
                        errno,
                        strerror(errno)
                    );
                }
            }


            /*
             * ------------------------------------------------
             * SWAP
             * ------------------------------------------------
             */

            else if (
                poll_cmd.event == swap_to) {

                int swap_id = -1;


                memcpy(
                    &swap_id,
                    poll_payload,
                    sizeof(swap_id)
                );


                printf(
                    "[evdi-bridge] Event: swap_to "
                    "(bo_id=%d) -> fence_fd=%d\n",
                    swap_id,
                    fence_fd
                );


                if (fence_fd >= 0) {

                    char dummy = 1;


                    struct iovec iov = {
                        .iov_base = &dummy,
                        .iov_len = 1
                    };


                    struct msghdr fmsg;

                    memset(
                        &fmsg,
                        0,
                        sizeof(fmsg)
                    );


                    fmsg.msg_iov =
                        &iov;

                    fmsg.msg_iovlen =
                        1;


                    if (sendmsg(
                            fence_fd,
                            &fmsg,
                            0) < 0) {

                        fprintf(
                            stderr,
                            "[evdi-bridge] "
                            "send fence failed: "
                            "errno=%d (%s)\n",
                            errno,
                            strerror(errno)
                        );
                    }
                }
            }


            /*
             * ------------------------------------------------
             * Unknown event
             * ------------------------------------------------
             */

            else if (
                poll_cmd.event != none) {

                printf(
                    "[evdi-bridge] Event: %d "
                    "(poll_id=%d)\n",
                    poll_cmd.event,
                    poll_cmd.poll_id
                );
            }
        }


        /*
         * ====================================================
         * Stop watchdog
         * ====================================================
         */

        g_connected = 0;

        /*
         * Shutdown client socket to wake watchdog.
         */

        shutdown(
            client_sock,
            SHUT_RDWR
        );


        pthread_join(
            watchdog_th,
            NULL
        );


session_cleanup:

        /*
         * ====================================================
         * Disconnect EVDI
         * ====================================================
         */

        if (g_evdi_fd >= 0) {

            evdi_disconnect(
                g_evdi_fd,
                g_evdi_idx
            );

            close_fd(
                &g_evdi_fd
            );

            g_evdi_idx = -1;
        }


        /*
         * ====================================================
         * Cleanup DMA-BUFs
         * ====================================================
         */

        /*
         * Variables are declared in the session scope,
         * but cleanup must only happen when they were created.
         */

        /*
         * Because C does not allow jumping over declarations
         * with initialization in strict modes, all important
         * cleanup is performed below using the variables that
         * were initialized before session_cleanup.
         *
         * dma_fds / mapped_bufs / meta_fds may not exist if
         * an earlier protocol step failed.
         */

        /*
         * This section is intentionally handled through the
         * variables in the surrounding scope.
         */


        /*
         * The following cleanup is safe for buffers which
         * were successfully initialized.
         */

        /*
         * NOTE:
         * If compilation complains about scope here, move the
         * DMA-BUF arrays to the beginning of the accept-loop
         * before the first goto.
         */


        if (shm_ptr) {

            munmap(
                (void *)shm_ptr,
                sizeof(uint32_t)
            );

            shm_ptr = NULL;
        }


        close_fd(&client_sock);

        close_fd(&data_fd);
        close_fd(&shm_fd);
        close_fd(&fence_fd);
        close_fd(&efd);
        close_fd(&audio_fd);


        printf(
            "[evdi-bridge] Cleaned up session. "
            "Waiting for next connection...\n"
        );
    }


    close(sock);

    unlink(
        "/tmp/display_daemon.sock"
    );

    return 0;
}
