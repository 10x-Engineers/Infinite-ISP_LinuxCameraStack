#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

class GstViewer {
public:
    GstViewer(unsigned int sourceWidth, unsigned int sourceHeight,
              unsigned int displayWidth,
              unsigned int displayHeight, unsigned int displayFps,
              std::atomic<bool> &applicationRunning);
    ~GstViewer();

    bool start();
    bool pushBgrFrame(const void *data, unsigned int stride);
    void stop();
    std::string lastError() const;

private:
    void monitorBus();
    void setError(std::string error);

    unsigned int sourceWidth_;
    unsigned int sourceHeight_;
    unsigned int displayWidth_;
    unsigned int displayHeight_;
    unsigned int displayFps_;
    std::atomic<bool> &applicationRunning_;
    std::atomic<std::uint64_t> frameNumber_{0};
    std::atomic<bool> running_{false};
    GstElement *pipeline_{nullptr};
    GstAppSrc *appSource_{nullptr};
    GstBus *bus_{nullptr};
    std::thread busThread_;
    mutable std::mutex errorMutex_;
    std::string error_;
};
