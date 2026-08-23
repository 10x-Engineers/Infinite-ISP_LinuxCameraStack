#include <vector>
#include <queue>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <opencv2/opencv.hpp>

/* --------------------------------------------------------- */
/* utils                                                     */
/* --------------------------------------------------------- */

static void perror_ln(const char *msg) {
    std::cerr << msg << " : " << strerror(errno) << "\n";
}

static int xioctl(int fd, int req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); }
    while (r < 0 && errno == EINTR);
    return r;
}

/* --------------------------------------------------------- */
/* Buffer / V4L2Stream                                       */
/* --------------------------------------------------------- */

struct Buffer {
    void   *start = nullptr;   // only for MMAP
    size_t  length = 0;
    int     fd = -1;            // exported DMABUF
    bool    ownsFd = false;
};

struct ExternalDmaBuffer {
    int fd = -1;
    size_t length = 0;
};

struct DequeueInfo {
    uint32_t sequence = 0;
    struct timeval timestamp{};
    uint32_t flags = 0;
};

class V4L2Stream {
public:
    V4L2Stream(const std::string &dev, bool output, int bufcnt)
        : devpath(dev), isOutput(output), reqCount(bufcnt) {}

    ~V4L2Stream() { closeDevice(); }

    bool openDevice() {
        fd = open(devpath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            perror_ln(("open " + devpath).c_str());
            return false;
        }
        return true;
    }

    void closeDevice() {
        stopStreaming();
        for (auto &b : buffers) {
            if (b.start)
                munmap(b.start, b.length);
            if (b.fd >= 0 && b.ownsFd)
                close(b.fd);
        }
        buffers.clear();
        if (fd >= 0)
            close(fd);
        fd = -1;
    }

    bool setFormat(uint32_t w, uint32_t h, uint32_t pixfmt) {
        struct v4l2_format fmt{};
        fmt.type = isOutput ?
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width  = w;
        fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror_ln(("VIDIOC_S_FMT " + devpath).c_str());
            return false;
        }
        width  = fmt.fmt.pix_mp.width;
        height = fmt.fmt.pix_mp.height;
        pixelFormat = fmt.fmt.pix_mp.pixelformat;
        numPlanes = fmt.fmt.pix_mp.num_planes ? fmt.fmt.pix_mp.num_planes : 1;
        bytesPerLine = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (!bytesPerLine) {
            const uint32_t bytesPerPixel =
                (pixelFormat == V4L2_PIX_FMT_RGB24 ||
                 pixelFormat == V4L2_PIX_FMT_BGR24) ? 3 : 2;
            bytesPerLine = width * bytesPerPixel;
        }
        return true;
    }

    /* -------- MMAP capture -------- */

    bool initMMap() {
        struct v4l2_requestbuffers req{};
        req.count  = reqCount;
        req.type   = isOutput ?
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror_ln("VIDIOC_REQBUFS MMAP");
            return false;
        }

        memoryType = V4L2_MEMORY_MMAP;

        buffers.resize(req.count);

        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf{};
            struct v4l2_plane planes[VIDEO_MAX_PLANES]{};

            buf.type   = req.type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            buf.m.planes = planes;
            buf.length = numPlanes;

            if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                perror_ln("VIDIOC_QUERYBUF");
                return false;
            }

            buffers[i].length = buf.m.planes[0].length;
            buffers[i].start = mmap(
                nullptr,
                buf.m.planes[0].length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                buf.m.planes[0].m.mem_offset
            );
            if (buffers[i].start == MAP_FAILED) {
                perror_ln("mmap");
                return false;
            }
        }
        return true;
    }

    bool queueAllCapture() {
        for (size_t i = 0; i < buffers.size(); ++i) {
            if (!queueCapture(i))
                return false;
        }
        return true;
    }

    bool queueCapture(int idx) {
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[VIDEO_MAX_PLANES]{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = memoryType;
        buf.index  = idx;
        buf.m.planes = planes;
        buf.length = numPlanes;
        if (memoryType == V4L2_MEMORY_DMABUF) {
            if (idx < 0 || static_cast<size_t>(idx) >= buffers.size())
                return false;
            planes[0].m.fd = buffers[idx].fd;
            planes[0].length = buffers[idx].length;
        }
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror_ln("VIDIOC_QBUF capture");
            return false;
        }
        return true;
    }

    /* -------- DMABUF export / import -------- */

    bool exportAllDMABuf() {
        for (size_t i = 0; i < buffers.size(); ++i) {
            struct v4l2_exportbuffer exp{};
            exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            exp.index = i;
            exp.plane = 0;
            exp.flags = O_CLOEXEC;

            if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
                perror_ln("VIDIOC_EXPBUF");
                return false;
            }
            buffers[i].fd = exp.fd;
            buffers[i].ownsFd = true;
        }
        return true;
    }

    bool initDMABufCapture(const std::vector<ExternalDmaBuffer> &external) {
        if (isOutput || external.empty())
            return false;

        struct v4l2_requestbuffers req{};
        req.count = external.size();
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror_ln("VIDIOC_REQBUFS capture DMABUF");
            return false;
        }
        if (req.count == 0 || req.count > external.size()) {
            std::cerr << "Unexpected capture DMABUF count " << req.count
                      << "\n";
            return false;
        }

        memoryType = V4L2_MEMORY_DMABUF;
        buffers.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            buffers[i].fd = external[i].fd;
            buffers[i].length = external[i].length;
            buffers[i].ownsFd = false;
        }
        return true;
    }

    bool initDMABufImport(size_t count) {
        struct v4l2_requestbuffers req{};
        req.count  = count;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;

        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror_ln("VIDIOC_REQBUFS DMABUF");
            return false;
        }
        memoryType = V4L2_MEMORY_DMABUF;
        buffers.resize(req.count);
        return true;
    }

    bool queueDMABuf(int idx, int dmabuf_fd, size_t bytes) {
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[VIDEO_MAX_PLANES]{};

        buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.index  = idx;
        buf.m.planes = planes;
        buf.length = numPlanes;

        planes[0].m.fd       = dmabuf_fd;
        planes[0].bytesused = bytes;
        planes[0].length    = bytes;

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror_ln("VIDIOC_QBUF DMABUF");
            return false;
        }
        return true;
    }

    /* -------- streaming -------- */

    bool startStreaming() {
        enum v4l2_buf_type t = isOutput ?
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
            perror_ln("VIDIOC_STREAMON");
            return false;
        }
        streaming = true;
        return true;
    }

    void stopStreaming() {
        if (!streaming) return;
        enum v4l2_buf_type t = isOutput ?
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
        streaming = false;
    }

    int dequeue(int timeout_ms, DequeueInfo *info = nullptr) {
        struct pollfd pfd{fd, (short)(isOutput ? POLLOUT : POLLIN), 0};
        if (poll(&pfd, 1, timeout_ms) <= 0)
            return -1;

        struct v4l2_buffer buf{};
        struct v4l2_plane planes[VIDEO_MAX_PLANES]{};
        buf.type   = isOutput ?
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = memoryType;
        buf.m.planes = planes;
        buf.length = numPlanes;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0)
            return -1;
        if (info) {
            info->sequence = buf.sequence;
            info->timestamp = buf.timestamp;
            info->flags = buf.flags;
        }
        return buf.index;
    }

    void* bufferPtr(int idx) { return buffers[idx].start; }
    size_t bufferLen(int idx){ return buffers[idx].length; }
    uint32_t frameWidth() const { return width; }
    uint32_t frameHeight() const { return height; }
    uint32_t frameStride() const { return bytesPerLine; }

    std::vector<Buffer> buffers;

private:
    std::string devpath;
    bool isOutput;
    int fd{-1};
    size_t reqCount;
    bool streaming{false};
    uint32_t width{0}, height{0};
    uint32_t bytesPerLine{0};
    uint32_t pixelFormat{0};
    unsigned numPlanes{1};
    enum v4l2_memory memoryType{V4L2_MEMORY_MMAP};
};

/* --------------------------------------------------------- */
/* thread-safe queue                                         */
/* --------------------------------------------------------- */

class IndexQueue {
public:
    IndexQueue(size_t cap) : cap(cap) {}

    bool push(int v) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]{ return q.size() < cap || !m_running; });
        if (!m_running) return false;
        q.push(v);
        cv.notify_all();
        return true;
    }

    bool pop(int &v) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]{ return !q.empty() || !m_running; });
        if (q.empty()) return false;
        v = q.front();
        q.pop();
        cv.notify_all();
        return true;
    }

    bool tryPop(int &v) {
        std::lock_guard<std::mutex> lk(m);
        if (q.empty()) return false;
        v = q.front();
        q.pop();
        cv.notify_all();
        return true;
    }

    void stop() {
        m_running = false;
        cv.notify_all();
    }

    ~IndexQueue() {
        stop();
    }

private:
    std::queue<int> q;
    size_t cap;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> m_running{true};
};
