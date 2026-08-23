/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <cstdint>
#include <optional>

namespace infinite_isp {

constexpr std::uint32_t kStatFlagAeValid = 1U << 0;
constexpr std::uint32_t kStatFlagAwbValid = 1U << 1;
constexpr std::uint32_t kStatFlagDgainValid = 1U << 2;

enum class AeResponse : std::uint32_t {
    Normal = 0,
    Overexposed = 1,
    Hold = 2,
    Underexposed = 3,
};

struct FrameStatistics {
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint32_t irq_status = 0;
    std::uint32_t flags = 0;
    AeResponse ae_response = AeResponse::Hold;
    std::uint32_t ae_skewness = 0;
    bool ae_done = false;
    std::uint32_t awb_r_gain = 0;
    std::uint32_t awb_b_gain = 0;
    std::uint32_t dgain_index = 0;
    std::uint32_t wb_r_gain = 0;
    std::uint32_t wb_b_gain = 0;
    std::uint32_t dropped_frames = 0;

    bool aeValid() const { return flags & kStatFlagAeValid; }
    bool awbValid() const { return flags & kStatFlagAwbValid; }
    bool dgainValid() const { return flags & kStatFlagDgainValid; }
};

struct ControlUpdate {
    std::optional<bool> auto_white_balance;
    std::optional<bool> auto_gain;
    std::optional<std::uint32_t> red_balance;
    std::optional<std::uint32_t> blue_balance;
    std::optional<std::uint32_t> digital_gain;

    bool empty() const;
};

enum class AeMode {
    Hardware,
    Software,
};

struct TuningConfig {
    AeMode ae_mode = AeMode::Hardware;
    bool hardware_awb = true;
    std::uint32_t min_dgain_index = 0;
    std::uint32_t max_dgain_index = 99;
    std::uint32_t dgain_step = 1;
    std::uint32_t decision_frames = 4;
};

/*
 * Pure tuning policy. It has no Linux or V4L2 dependency and can be reused by
 * a future libcamera IPA/backend. Hardware mode observes the RTL AE/AWB loops;
 * software mode only takes over DGAIN and uses the RTL AE decision.
 */
class AutoTuner {
public:
    explicit AutoTuner(TuningConfig config = {});

    ControlUpdate initialControls() const;
    ControlUpdate process(const FrameStatistics &statistics);
    const TuningConfig &config() const { return config_; }

private:
    TuningConfig config_;
    AeResponse last_response_ = AeResponse::Hold;
    std::uint32_t response_frames_ = 0;
};

const char *toString(AeResponse response);

} // namespace infinite_isp
