/* SPDX-License-Identifier: BSD-3-Clause */
#include "infinite_isp/tuning.hpp"

#include <cassert>

using infinite_isp::AeMode;
using infinite_isp::AeResponse;
using infinite_isp::AutoTuner;
using infinite_isp::FrameStatistics;
using infinite_isp::SensorAeConfig;
using infinite_isp::SensorAeTuner;
using infinite_isp::TuningConfig;

static FrameStatistics statistics(AeResponse response, std::uint32_t gain)
{
    FrameStatistics value;
    value.flags = infinite_isp::kStatFlagAeValid |
                  infinite_isp::kStatFlagDgainValid;
    value.ae_response = response;
    value.dgain_index = gain;
    return value;
}

int main()
{
    AutoTuner hardware;
    const auto hardware_controls = hardware.initialControls();
    assert(hardware_controls.auto_gain == true);
    assert(hardware_controls.auto_white_balance == true);
    assert(hardware.process(statistics(AeResponse::Underexposed, 10)).empty());

    TuningConfig config;
    config.ae_mode = AeMode::Software;
    config.min_dgain_index = 2;
    config.max_dgain_index = 12;
    config.dgain_step = 3;
    config.decision_frames = 2;
    AutoTuner software(config);

    const auto software_controls = software.initialControls();
    assert(software_controls.auto_gain == false);
    assert(software_controls.auto_white_balance == true);

    assert(software.process(statistics(AeResponse::Underexposed, 8)).empty());
    auto update = software.process(statistics(AeResponse::Underexposed, 8));
    assert(update.digital_gain == 11);

    assert(software.process(statistics(AeResponse::Overexposed, 4)).empty());
    update = software.process(statistics(AeResponse::Overexposed, 4));
    assert(update.digital_gain == 2);

    assert(software.process(statistics(AeResponse::Underexposed, 12)).empty());
    assert(software.process(statistics(AeResponse::Underexposed, 12)).empty());

    FrameStatistics invalid = statistics(AeResponse::Underexposed, 5);
    invalid.flags = 0;
    assert(software.process(invalid).empty());

    TuningConfig sensor_isp_config;
    sensor_isp_config.ae_mode = AeMode::Sensor;
    AutoTuner sensor_isp(sensor_isp_config);
    const auto sensor_isp_controls = sensor_isp.initialControls();
    assert(sensor_isp_controls.auto_gain == false);
    assert(sensor_isp_controls.digital_gain == 0);

    SensorAeConfig sensor_config;
    sensor_config.initial_analogue_gain = 192;
    sensor_config.decision_frames = 2;
    SensorAeTuner sensor(sensor_config);
    const auto sensor_initial = sensor.initialControls();
    assert(sensor_initial.analogue_gain == 192);
    assert(sensor_initial.exposure == 1587);

    auto sensor_stats = statistics(AeResponse::Underexposed, 0);
    sensor_stats.ae_skewness = 600;
    assert(sensor.process(sensor_stats).empty());
    auto sensor_update = sensor.process(sensor_stats);
    assert(sensor_update.analogue_gain == 196);

    sensor_stats.ae_response = AeResponse::Overexposed;
    sensor_stats.ae_skewness = 200;
    assert(sensor.process(sensor_stats).empty());
    sensor_update = sensor.process(sensor_stats);
    assert(sensor_update.analogue_gain == 195);

    sensor_stats.ae_response = AeResponse::Normal;
    assert(sensor.process(sensor_stats).empty());
    return 0;
}
