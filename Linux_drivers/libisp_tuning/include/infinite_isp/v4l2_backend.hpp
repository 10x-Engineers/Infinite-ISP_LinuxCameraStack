/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <memory>
#include <string>

#include "infinite_isp/tuning.hpp"

namespace infinite_isp {

enum class ReadResult {
    Frame,
    Timeout,
    Error,
};

/* Linux transport for the reusable tuning policy. */
class V4L2Backend {
public:
    explicit V4L2Backend(std::string device_path);
    ~V4L2Backend();

    V4L2Backend(const V4L2Backend &) = delete;
    V4L2Backend &operator=(const V4L2Backend &) = delete;

    bool start(unsigned int buffer_count = 4);
    void stop();
    ReadResult read(FrameStatistics &statistics, int timeout_ms = 500);
    bool apply(const ControlUpdate &controls);

    const std::string &devicePath() const;
    const std::string &lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string findStatDevice();

} // namespace infinite_isp
