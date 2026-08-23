/* SPDX-License-Identifier: BSD-3-Clause */
#include "infinite_isp/tuning.hpp"

#include <algorithm>

namespace infinite_isp {

bool ControlUpdate::empty() const
{
    return !auto_white_balance && !auto_gain && !red_balance &&
           !blue_balance && !digital_gain;
}

AutoTuner::AutoTuner(TuningConfig config)
    : config_(config)
{
    config_.max_dgain_index = std::max(config_.max_dgain_index,
                                       config_.min_dgain_index);
    config_.dgain_step = std::max(1U, config_.dgain_step);
    config_.decision_frames = std::max(1U, config_.decision_frames);
}

ControlUpdate AutoTuner::initialControls() const
{
    ControlUpdate controls;
    controls.auto_white_balance = config_.hardware_awb;
    controls.auto_gain = config_.ae_mode == AeMode::Hardware;
    if (config_.ae_mode == AeMode::Sensor)
        controls.digital_gain = config_.fixed_dgain_index;
    return controls;
}

ControlUpdate AutoTuner::process(const FrameStatistics &statistics)
{
    ControlUpdate controls;

    if (config_.ae_mode != AeMode::Software || !statistics.aeValid() ||
        !statistics.dgainValid())
        return controls;

    if (statistics.ae_response != last_response_) {
        last_response_ = statistics.ae_response;
        response_frames_ = 1;
    } else {
        response_frames_++;
    }

    if (statistics.ae_response == AeResponse::Normal ||
        statistics.ae_response == AeResponse::Hold) {
        response_frames_ = 0;
        return controls;
    }

    if (response_frames_ < config_.decision_frames)
        return controls;
    response_frames_ = 0;

    std::uint32_t gain = statistics.dgain_index;
    if (statistics.ae_response == AeResponse::Underexposed) {
        if (gain < config_.max_dgain_index) {
            gain += std::min(config_.dgain_step,
                             config_.max_dgain_index - gain);
        }
    } else if (statistics.ae_response == AeResponse::Overexposed) {
        if (gain > config_.min_dgain_index) {
            gain -= std::min(config_.dgain_step,
                             gain - config_.min_dgain_index);
        }
    }

    if (gain != statistics.dgain_index)
        controls.digital_gain = gain;
    return controls;
}

SensorAeTuner::SensorAeTuner(SensorAeConfig config)
    : config_(config)
{
    config_.max_analogue_gain = std::max(config_.max_analogue_gain,
                                         config_.min_analogue_gain);
    config_.initial_analogue_gain = std::clamp(
        config_.initial_analogue_gain, config_.min_analogue_gain,
        config_.max_analogue_gain);
    config_.decision_frames = std::max(1U, config_.decision_frames);
    current_gain_ = config_.initial_analogue_gain;
}

SensorControlUpdate SensorAeTuner::initialControls() const
{
    SensorControlUpdate controls;
    controls.analogue_gain = current_gain_;
    controls.exposure = config_.exposure;
    return controls;
}

SensorControlUpdate SensorAeTuner::process(const FrameStatistics &statistics)
{
    SensorControlUpdate controls;
    if (!statistics.aeValid())
        return controls;

    if (statistics.ae_response != last_response_) {
        last_response_ = statistics.ae_response;
        response_frames_ = 1;
    } else {
        response_frames_++;
    }

    if (statistics.ae_response == AeResponse::Normal ||
        statistics.ae_response == AeResponse::Hold) {
        response_frames_ = 0;
        return controls;
    }

    if (response_frames_ < config_.decision_frames)
        return controls;
    response_frames_ = 0;

    std::uint32_t step = 1;
    if (statistics.ae_skewness > 1024)
        step = 8;
    else if (statistics.ae_skewness > 512)
        step = 4;
    else if (statistics.ae_skewness > 256)
        step = 2;

    const std::uint32_t old_gain = current_gain_;
    if (statistics.ae_response == AeResponse::Underexposed) {
        current_gain_ += std::min(step,
                                  config_.max_analogue_gain - current_gain_);
    } else if (statistics.ae_response == AeResponse::Overexposed) {
        current_gain_ -= std::min(step,
                                  current_gain_ - config_.min_analogue_gain);
    }

    if (current_gain_ != old_gain)
        controls.analogue_gain = current_gain_;
    return controls;
}

const char *toString(AeResponse response)
{
    switch (response) {
    case AeResponse::Normal:
        return "normal";
    case AeResponse::Overexposed:
        return "overexposed";
    case AeResponse::Hold:
        return "hold";
    case AeResponse::Underexposed:
        return "underexposed";
    }
    return "unknown";
}

} // namespace infinite_isp
