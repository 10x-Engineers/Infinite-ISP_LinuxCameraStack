#include "GstViewer.hpp"

#include <mutex>
#include <sstream>
#include <utility>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

std::once_flag gstInitialization;

} // namespace

GstViewer::GstViewer(unsigned int sourceWidth, unsigned int sourceHeight,
                     unsigned int displayWidth,
                     unsigned int displayHeight, unsigned int displayFps,
                     std::atomic<bool> &applicationRunning)
    : sourceWidth_(sourceWidth), sourceHeight_(sourceHeight),
      displayWidth_(displayWidth),
      displayHeight_(displayHeight), displayFps_(displayFps),
      applicationRunning_(applicationRunning) {}

GstViewer::~GstViewer() {
    stop();
}

bool GstViewer::start() {
    if (running_)
        return true;

    std::call_once(gstInitialization, [] { gst_init(nullptr, nullptr); });

    /*
     * ISP capture remains in the application so it can always dequeue/requeue
     * promptly. pushBgrFrame() fuses 2x downsampling and BGR24 -> RGB565; this
     * avoids GStreamer's two costly full-frame scale/convert passes. The leaky
     * queue discards stale preview frames. ximagesink is intentional because
     * GL is software-rendered on the current board.
     */
    std::ostringstream description;
    description
        << "appsrc name=preview_source is-live=true format=time block=false "
           "do-timestamp=true caps=video/x-raw,format=RGB16,width="
        << displayWidth_ << ",height=" << displayHeight_
        << ",framerate=" << displayFps_ << "/1 ! "
        << "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 "
           "leaky=downstream ! "
        << "videoconvert n-threads=1 ! "
        << "ximagesink sync=false qos=false force-aspect-ratio=true";

    GError *parseError = nullptr;
    pipeline_ = gst_parse_launch(description.str().c_str(), &parseError);
    if (!pipeline_) {
        setError(parseError ? parseError->message :
                              "cannot create GStreamer pipeline");
        if (parseError)
            g_error_free(parseError);
        return false;
    }
    if (parseError) {
        setError(parseError->message);
        g_error_free(parseError);
        stop();
        return false;
    }

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline_),
                                             "preview_source");
    if (!source) {
        setError("cannot find appsrc in GStreamer pipeline");
        stop();
        return false;
    }
    appSource_ = GST_APP_SRC(source);

    bus_ = gst_element_get_bus(pipeline_);
    running_ = true;
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        setError("cannot start GStreamer display pipeline");
        stop();
        return false;
    }

    busThread_ = std::thread(&GstViewer::monitorBus, this);
    return true;
}

bool GstViewer::pushBgrFrame(const void *data, unsigned int stride) {
    if (!running_ || !appSource_)
        return false;

    const gsize outputBytes = static_cast<gsize>(displayWidth_) *
                              displayHeight_ * sizeof(std::uint16_t);
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, outputBytes, nullptr);
    if (!buffer) {
        setError("cannot allocate GStreamer preview buffer");
        return false;
    }

    GstMapInfo mapping{};
    if (!gst_buffer_map(buffer, &mapping, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        setError("cannot map GStreamer preview buffer");
        return false;
    }

    const auto *source = static_cast<const std::uint8_t *>(data);
    auto *destination = reinterpret_cast<std::uint16_t *>(mapping.data);
    if (sourceWidth_ == 2 * displayWidth_ &&
        sourceHeight_ == 2 * displayHeight_) {
        for (unsigned int y = 0; y < displayHeight_; ++y) {
            const auto *sourceRow = source +
                static_cast<std::size_t>(2 * y) * stride;
            auto *destinationRow = destination +
                static_cast<std::size_t>(y) * displayWidth_;
            unsigned int x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
            for (; x + 8 <= displayWidth_; x += 8) {
                /* Read 16 BGR pixels and retain the eight even pixels. */
                const uint8x16x3_t bgr = vld3q_u8(sourceRow + 6 * x);
                const uint8x8_t blue = vget_low_u8(
                    vuzp1q_u8(bgr.val[0], bgr.val[0]));
                const uint8x8_t green = vget_low_u8(
                    vuzp1q_u8(bgr.val[1], bgr.val[1]));
                const uint8x8_t red = vget_low_u8(
                    vuzp1q_u8(bgr.val[2], bgr.val[2]));
                const uint16x8_t packedRed = vshlq_n_u16(
                    vmovl_u8(vshr_n_u8(red, 3)), 11);
                const uint16x8_t packedGreen = vshlq_n_u16(
                    vmovl_u8(vshr_n_u8(green, 2)), 5);
                const uint16x8_t packedBlue = vmovl_u8(
                    vshr_n_u8(blue, 3));
                vst1q_u16(destinationRow + x,
                          vorrq_u16(vorrq_u16(packedRed, packedGreen),
                                    packedBlue));
            }
#endif
            for (; x < displayWidth_; ++x) {
                const auto *pixel = sourceRow + 6 * x;
                destinationRow[x] = static_cast<std::uint16_t>(
                    ((pixel[2] >> 3) << 11) |
                    ((pixel[1] >> 2) << 5) |
                    (pixel[0] >> 3));
            }
        }
    } else {
        for (unsigned int y = 0; y < displayHeight_; ++y) {
            const unsigned int sourceY =
                y * sourceHeight_ / displayHeight_;
            const auto *sourceRow = source +
                static_cast<std::size_t>(sourceY) * stride;
            auto *destinationRow = destination +
                static_cast<std::size_t>(y) * displayWidth_;
            for (unsigned int x = 0; x < displayWidth_; ++x) {
                const unsigned int sourceX =
                    x * sourceWidth_ / displayWidth_;
                const auto *pixel = sourceRow + 3 * sourceX;
                destinationRow[x] = static_cast<std::uint16_t>(
                    ((pixel[2] >> 3) << 11) |
                    ((pixel[1] >> 2) << 5) |
                    (pixel[0] >> 3));
            }
        }
    }
    gst_buffer_unmap(buffer, &mapping);

    const GstClockTime duration = GST_SECOND / displayFps_;
    const std::uint64_t frame = frameNumber_.fetch_add(
        1, std::memory_order_relaxed);
    GST_BUFFER_PTS(buffer) = frame * duration;
    GST_BUFFER_DURATION(buffer) = duration;
    const GstFlowReturn result = gst_app_src_push_buffer(appSource_, buffer);
    if (result != GST_FLOW_OK && result != GST_FLOW_FLUSHING) {
        setError("GStreamer rejected a preview frame");
        return false;
    }
    return result == GST_FLOW_OK;
}

void GstViewer::stop() {
    running_ = false;
    if (pipeline_)
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (busThread_.joinable())
        busThread_.join();
    if (appSource_)
        gst_object_unref(appSource_);
    appSource_ = nullptr;
    if (bus_)
        gst_object_unref(bus_);
    bus_ = nullptr;
    if (pipeline_)
        gst_object_unref(pipeline_);
    pipeline_ = nullptr;
}

std::string GstViewer::lastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

void GstViewer::monitorBus() {
    while (running_) {
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus_, 200 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message)
            continue;

        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            setError(error ? error->message : "unknown GStreamer error");
            if (error)
                g_error_free(error);
            g_free(debug);
        } else {
            setError("GStreamer display reached end of stream");
        }
        gst_message_unref(message);
        applicationRunning_ = false;
        running_ = false;
    }
}

void GstViewer::setError(std::string error) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    error_ = std::move(error);
}
