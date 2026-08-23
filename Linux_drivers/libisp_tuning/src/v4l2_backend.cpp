/* SPDX-License-Identifier: BSD-3-Clause */
#include "infinite_isp/v4l2_backend.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/xil-isp-lite.h>

namespace infinite_isp {
namespace {

int retryIoctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

} // namespace

struct V4L2Backend::Impl {
    struct Buffer {
        void *address = nullptr;
        std::size_t length = 0;
    };

    explicit Impl(std::string path)
        : device_path(std::move(path))
    {
    }

    void setErrno(const char *operation)
    {
        error = std::string(operation) + ": " + std::strerror(errno);
    }

    bool setControl(std::uint32_t id, std::int32_t value)
    {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if (retryIoctl(fd, VIDIOC_S_CTRL, &control) < 0) {
            setErrno("VIDIOC_S_CTRL");
            return false;
        }
        return true;
    }

    bool setColorCorrectionMatrix(
        const std::array<std::int32_t, 9> &matrix)
    {
        /* REG_CCM occupies one 128-word ISP register block. */
        std::array<std::uint32_t, 128> payload{};
        for (std::size_t i = 0; i < matrix.size(); ++i)
            payload[i] = static_cast<std::uint32_t>(matrix[i]);

        v4l2_ext_control control{};
        control.id = V4L2_CID_USER_XIL_ISP_LITE_CCM;
        control.size = sizeof(payload);
        control.ptr = payload.data();

        v4l2_ext_controls controls{};
        controls.which = V4L2_CTRL_WHICH_CUR_VAL;
        controls.count = 1;
        controls.controls = &control;
        if (retryIoctl(fd, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
            setErrno("VIDIOC_S_EXT_CTRLS(CCM)");
            return false;
        }
        return true;
    }

    std::string device_path;
    std::string error;
    int fd = -1;
    bool streaming = false;
    std::vector<Buffer> buffers;
};

struct V4L2SensorBackend::Impl {
    explicit Impl(std::string path)
        : device_path(std::move(path))
    {
    }

    void setErrno(const char *operation)
    {
        error = std::string(operation) + ": " + std::strerror(errno);
    }

    std::string device_path;
    std::string error;
    int fd = -1;
};

V4L2Backend::V4L2Backend(std::string device_path)
    : impl_(std::make_unique<Impl>(std::move(device_path)))
{
}

V4L2Backend::~V4L2Backend()
{
    stop();
}

bool V4L2Backend::start(unsigned int buffer_count)
{
    if (impl_->streaming)
        return true;

    impl_->error.clear();
    impl_->fd = ::open(impl_->device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (impl_->fd < 0) {
        impl_->setErrno("open stat device");
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_META_CAPTURE;
    format.fmt.meta.dataformat = V4L2_META_FMT_XIL_ISP_LITE_STAT;
    format.fmt.meta.buffersize = sizeof(xil_isp_lite_stat_result);
    if (retryIoctl(impl_->fd, VIDIOC_S_FMT, &format) < 0) {
        impl_->setErrno("VIDIOC_S_FMT");
        stop();
        return false;
    }

    v4l2_requestbuffers request{};
    request.count = buffer_count;
    request.type = V4L2_BUF_TYPE_META_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (retryIoctl(impl_->fd, VIDIOC_REQBUFS, &request) < 0 || !request.count) {
        impl_->setErrno("VIDIOC_REQBUFS");
        stop();
        return false;
    }

    impl_->buffers.resize(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (retryIoctl(impl_->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            impl_->setErrno("VIDIOC_QUERYBUF");
            stop();
            return false;
        }

        void *address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, impl_->fd, buffer.m.offset);
        if (address == MAP_FAILED) {
            impl_->setErrno("mmap stat buffer");
            stop();
            return false;
        }
        impl_->buffers[i] = {address, buffer.length};

        if (retryIoctl(impl_->fd, VIDIOC_QBUF, &buffer) < 0) {
            impl_->setErrno("VIDIOC_QBUF");
            stop();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
    if (retryIoctl(impl_->fd, VIDIOC_STREAMON, &type) < 0) {
        impl_->setErrno("VIDIOC_STREAMON");
        stop();
        return false;
    }

    impl_->streaming = true;
    return true;
}

void V4L2Backend::stop()
{
    if (impl_->fd < 0)
        return;

    if (impl_->streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
        retryIoctl(impl_->fd, VIDIOC_STREAMOFF, &type);
        impl_->streaming = false;
    }

    for (auto &buffer : impl_->buffers) {
        if (buffer.address && buffer.address != MAP_FAILED)
            ::munmap(buffer.address, buffer.length);
    }
    impl_->buffers.clear();

    ::close(impl_->fd);
    impl_->fd = -1;
}

ReadResult V4L2Backend::read(FrameStatistics &statistics, int timeout_ms)
{
    if (!impl_->streaming) {
        impl_->error = "statistics stream is not running";
        return ReadResult::Error;
    }

    pollfd descriptor{impl_->fd, POLLIN, 0};
    int poll_result;
    do {
        poll_result = ::poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (!poll_result)
        return ReadResult::Timeout;
    if (poll_result < 0) {
        impl_->setErrno("poll stat device");
        return ReadResult::Error;
    }

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (retryIoctl(impl_->fd, VIDIOC_DQBUF, &buffer) < 0) {
        impl_->setErrno("VIDIOC_DQBUF");
        return ReadResult::Error;
    }

    bool valid = buffer.index < impl_->buffers.size() &&
                 buffer.bytesused >= sizeof(xil_isp_lite_stat_result);
    xil_isp_lite_stat_result raw{};
    if (valid)
        std::memcpy(&raw, impl_->buffers[buffer.index].address, sizeof(raw));

    if (retryIoctl(impl_->fd, VIDIOC_QBUF, &buffer) < 0) {
        impl_->setErrno("VIDIOC_QBUF requeue");
        return ReadResult::Error;
    }

    if (!valid || raw.abi_version != XIL_ISP_LITE_STAT_ABI_VERSION ||
        raw.record_size != sizeof(raw)) {
        impl_->error = "unsupported or truncated ISP statistics record";
        return ReadResult::Error;
    }

    statistics.sequence = raw.frame_sequence;
    statistics.timestamp_ns = raw.timestamp_ns;
    statistics.irq_status = raw.irq_status;
    statistics.flags = raw.flags;
    statistics.ae_response = static_cast<AeResponse>(raw.ae_response);
    statistics.ae_skewness = raw.ae_skewness;
    statistics.ae_done = raw.ae_done != 0;
    statistics.awb_r_gain = raw.awb_r_gain;
    statistics.awb_b_gain = raw.awb_b_gain;
    statistics.dgain_index = raw.dgain_index;
    statistics.wb_r_gain = raw.wb_r_gain;
    statistics.wb_b_gain = raw.wb_b_gain;
    statistics.dropped_frames = raw.dropped_frames;
    return ReadResult::Frame;
}

bool V4L2Backend::apply(const ControlUpdate &controls)
{
    if (impl_->fd < 0) {
        impl_->error = "control device is not open";
        return false;
    }

    if (controls.auto_white_balance &&
        !impl_->setControl(V4L2_CID_AUTO_WHITE_BALANCE,
                           *controls.auto_white_balance))
        return false;
    if (controls.red_balance &&
        !impl_->setControl(V4L2_CID_RED_BALANCE, *controls.red_balance))
        return false;
    if (controls.blue_balance &&
        !impl_->setControl(V4L2_CID_BLUE_BALANCE, *controls.blue_balance))
        return false;
    if (controls.auto_gain &&
        !impl_->setControl(V4L2_CID_AUTOGAIN, *controls.auto_gain))
        return false;
    if (controls.digital_gain &&
        !impl_->setControl(V4L2_CID_DIGITAL_GAIN, *controls.digital_gain))
        return false;
    if (controls.color_correction_matrix &&
        !impl_->setColorCorrectionMatrix(*controls.color_correction_matrix))
        return false;
    return true;
}

const std::string &V4L2Backend::devicePath() const
{
    return impl_->device_path;
}

const std::string &V4L2Backend::lastError() const
{
    return impl_->error;
}

V4L2SensorBackend::V4L2SensorBackend(std::string device_path)
    : impl_(std::make_unique<Impl>(std::move(device_path)))
{
}

V4L2SensorBackend::~V4L2SensorBackend()
{
    close();
}

bool V4L2SensorBackend::open()
{
    if (impl_->fd >= 0)
        return true;

    impl_->error.clear();
    impl_->fd = ::open(impl_->device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (impl_->fd < 0) {
        impl_->setErrno("open sensor subdevice");
        return false;
    }
    return true;
}

void V4L2SensorBackend::close()
{
    if (impl_->fd < 0)
        return;
    ::close(impl_->fd);
    impl_->fd = -1;
}

bool V4L2SensorBackend::apply(const SensorControlUpdate &controls)
{
    if (impl_->fd < 0) {
        impl_->error = "sensor subdevice is not open";
        return false;
    }
    v4l2_control control{};
    if (controls.exposure) {
        control.id = V4L2_CID_EXPOSURE;
        control.value = static_cast<std::int32_t>(*controls.exposure);
        if (retryIoctl(impl_->fd, VIDIOC_S_CTRL, &control) < 0) {
            impl_->setErrno("VIDIOC_S_CTRL exposure");
            return false;
        }
    }
    if (controls.analogue_gain) {
        control.id = V4L2_CID_ANALOGUE_GAIN;
        control.value = static_cast<std::int32_t>(*controls.analogue_gain);
        if (retryIoctl(impl_->fd, VIDIOC_S_CTRL, &control) < 0) {
            impl_->setErrno("VIDIOC_S_CTRL analogue gain");
            return false;
        }
    }
    return true;
}

const std::string &V4L2SensorBackend::devicePath() const
{
    return impl_->device_path;
}

const std::string &V4L2SensorBackend::lastError() const
{
    return impl_->error;
}

std::string findStatDevice()
{
    for (unsigned int i = 0; i < 64; ++i) {
        const std::string node = "video" + std::to_string(i);
        std::ifstream name_file("/sys/class/video4linux/" + node + "/name");
        std::string name;
        if (name_file && std::getline(name_file, name) &&
            name == "xil-isp-lite_stat")
            return "/dev/" + node;
    }
    return {};
}

} // namespace infinite_isp
