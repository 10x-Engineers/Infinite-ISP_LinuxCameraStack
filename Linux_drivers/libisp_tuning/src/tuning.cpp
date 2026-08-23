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
