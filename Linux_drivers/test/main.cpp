#include <iostream>
#include <csignal>
#include <cerrno>
#include <array>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <deque>
#include <limits>
#include <sstream>
#include <thread>
#include <chrono>
#include <ctime>
#include <opencv2/opencv.hpp>
#include "V4L2Subdev.hpp"
#include "V4L2Stream.hpp"
#include "infinite_isp/tuning.hpp"
#include "infinite_isp/v4l2_backend.hpp"
#ifdef ISP_HAVE_GSTREAMER
#include "GstViewer.hpp"
#endif
#ifdef ISP_HAVE_MALI_EGL
#include "MaliEglDmaBufViewer.hpp"
#endif

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_sensor_frames{0};
static std::atomic<uint64_t> g_ispin_frames{0};
static std::atomic<uint64_t> g_ispout_frames{0};
static std::atomic<uint64_t> g_display_frames{0};
static std::atomic<uint64_t> g_tuning_frames{0};
static std::atomic<uint64_t> g_display_stale_frames{0};
static std::atomic<uint64_t> g_display_age_samples{0};
static std::atomic<uint64_t> g_display_age_total_us{0};
static std::atomic<uint64_t> g_display_age_max_us{0};
static std::atomic<uint32_t> g_latest_tuning_sequence{0};
static std::atomic<uint32_t> g_latest_ae_response{2};
static std::atomic<uint32_t> g_latest_ae_skewness{0};
static std::atomic<uint32_t> g_latest_dgain_index{0};
static std::atomic<uint32_t> g_latest_sensor_again{0};
static void sigint_handler(int){ g_running = false; }

struct PipelineGeometry {
    const char *name;
    std::uint32_t sensorWidth;
    std::uint32_t sensorHeight;
    std::uint32_t ispInputWidth;
    std::uint32_t ispInputHeight;
    std::uint32_t ispOutputWidth;
    std::uint32_t ispOutputHeight;
    std::uint32_t rawStrideBytes;
};

static bool pipelineGeometryFromEnvironment(PipelineGeometry &geometry) {
    const std::string mode = std::getenv("ISP_FOV_MODE")
        ? std::getenv("ISP_FOV_MODE") : "wide";

    if (mode == "wide") {
        geometry = {"wide", 1640, 1152, 1992, 1152, 1640, 1080, 3840};
        return true;
    }
    if (mode == "standard" || mode == "crop" || mode == "legacy") {
        geometry = {"standard", 1992, 1152, 1992, 1152, 1920, 1080, 3840};
        return true;
    }

    std::cerr << "Unknown ISP_FOV_MODE='" << mode
              << "'; expected wide or standard" << std::endl;
    return false;
}

static void recordDisplayAge(const DequeueInfo &info) {
    if (info.timestamp.tv_sec == 0)
        return;

    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    const std::uint64_t nowUs = static_cast<std::uint64_t>(now.tv_sec) *
        1000000ULL + static_cast<std::uint64_t>(now.tv_nsec) / 1000ULL;
    const std::uint64_t bufferUs =
        static_cast<std::uint64_t>(info.timestamp.tv_sec) * 1000000ULL +
        static_cast<std::uint64_t>(info.timestamp.tv_usec);
    if (bufferUs > nowUs || nowUs - bufferUs > 5000000ULL)
        return;

    const std::uint64_t ageUs = nowUs - bufferUs;
    g_display_age_total_us.fetch_add(ageUs, std::memory_order_relaxed);
    g_display_age_samples.fetch_add(1, std::memory_order_relaxed);
    auto maximum = g_display_age_max_us.load(std::memory_order_relaxed);
    while (maximum < ageUs &&
           !g_display_age_max_us.compare_exchange_weak(
               maximum, ageUs, std::memory_order_relaxed)) {
    }
}

static cv::Rect measurementRoiFromEnvironment(int width, int height) {
    const cv::Rect fullFrame(0, 0, width, height);
    const char *value = std::getenv("ISP_MEASURE_ROI");
    if (!value)
        return fullFrame;

    int x = 0;
    int y = 0;
    int roiWidth = 0;
    int roiHeight = 0;
    if (std::sscanf(value, "%d,%d,%d,%d", &x, &y, &roiWidth,
                    &roiHeight) != 4 || roiWidth <= 0 || roiHeight <= 0) {
        std::cerr << "Invalid ISP_MEASURE_ROI='" << value
                  << "'; expected x,y,width,height. Using full frame."
                  << std::endl;
        return fullFrame;
    }

    const cv::Rect requested(x, y, roiWidth, roiHeight);
    const cv::Rect clipped = requested & fullFrame;
    if (clipped.empty()) {
        std::cerr << "ISP_MEASURE_ROI is outside the frame; using full frame."
                  << std::endl;
        return fullFrame;
    }
    return clipped;
}

static cv::Scalar sampledMeanBgr(const cv::Mat &bgr, const cv::Rect &roi,
                                 int sampleStep = 8) {
    std::uint64_t sums[3] = {};
    std::uint64_t samples = 0;
    for (int y = roi.y; y < roi.y + roi.height; y += sampleStep) {
        const auto *row = bgr.ptr<std::uint8_t>(y);
        for (int x = roi.x; x < roi.x + roi.width; x += sampleStep) {
            const auto *pixel = row + 3 * x;
            sums[0] += pixel[0];
            sums[1] += pixel[1];
            sums[2] += pixel[2];
            ++samples;
        }
    }
    return cv::Scalar(static_cast<double>(sums[0]) / samples,
                      static_cast<double>(sums[1]) / samples,
                      static_cast<double>(sums[2]) / samples);
}

static std::string findVideoNodeByName(const std::string &expectedName) {
    namespace fs = std::filesystem;
    const fs::path classDir("/sys/class/video4linux");
    std::error_code ec;

    for (const auto &entry : fs::directory_iterator(classDir, ec)) {
        const std::string node = entry.path().filename().string();
        if (node.rfind("video", 0) != 0 && node.rfind("v4l-subdev", 0) != 0)
            continue;

        std::ifstream nameFile(entry.path() / "name");
        std::string actualName;
        std::getline(nameFile, actualName);
        if (actualName == expectedName)
            return "/dev/" + node;
    }

    std::cerr << "Cannot find V4L2 node named '" << expectedName << "'\n";
    return {};
}

int setSubDevFmt(const PipelineGeometry &geometry) {
    std::cout << "=== V4L2 Subdev Format Setter ===" << std::endl;
    std::cout << "[Pipeline] FOV mode=" << geometry.name
              << ", Sensor=" << geometry.sensorWidth << 'x'
              << geometry.sensorHeight << ", ISP input="
              << geometry.ispInputWidth << 'x' << geometry.ispInputHeight
              << ", output=" << geometry.ispOutputWidth << 'x'
              << geometry.ispOutputHeight << std::endl;

    // -------------------------
    // 1. Sensor (imx219)
    // -------------------------
    V4L2Subdev sensor(findVideoNodeByName("xlnx-imx219 6-0010"));
    if (!sensor.isValid()) return -1;

    std::cout << "Sensor name: " << sensor.getName() << std::endl;

    if (!sensor.setFormat(
        0,                              // pad
        MEDIA_BUS_FMT_SRGGB10_1X10,     // raw Bayer 10bit
        geometry.sensorWidth, geometry.sensorHeight,
        V4L2_FIELD_NONE
    )) return -1;
    std::cout << "Sensor format set OK." << std::endl;

    // -------------------------
    // 2. MIPI RX
    // -------------------------
    V4L2Subdev mipi(findVideoNodeByName("a0030000.mipi_rx_to_video"));
    if (!mipi.isValid()) return -1;

    std::cout << "MIPI name: " << mipi.getName() << std::endl;

    // Configure both MIPI pads, matching the media-ctl setup sequence.
    if (!mipi.setFormat(0, MEDIA_BUS_FMT_SRGGB10_1X10,
                        geometry.sensorWidth, geometry.sensorHeight) ||
        !mipi.setFormat(1, MEDIA_BUS_FMT_SRGGB10_1X10,
                        geometry.sensorWidth, geometry.sensorHeight))
        return -1;
    v4l2_mbus_framefmt mipiSource {};
    if (!mipi.getFormat(1, mipiSource) ||
        mipiSource.width != geometry.sensorWidth ||
        mipiSource.height != geometry.sensorHeight) {
        std::cerr << "Unexpected MIPI source format "
                  << mipiSource.width << 'x' << mipiSource.height
                  << ", expected " << geometry.sensorWidth << 'x'
                  << geometry.sensorHeight
                  << std::endl;
        return -1;
    }
    std::cout << "MIPI formats set OK." << std::endl;

    // -------------------------
    // 3. Subset Converter (ISP block)
    // -------------------------
    V4L2Subdev conv(findVideoNodeByName("axi:camif_rpi_axis_subsetconv"));
    if (!conv.isValid()) return -1;

    std::cout << "SubsetConv name: " << conv.getName() << std::endl;

    // pad0: input RAW
    if (!conv.setFormat(0, MEDIA_BUS_FMT_SRGGB10_1X10,
                        geometry.sensorWidth, geometry.sensorHeight))
        return -1;
    // pad1: output Y10
    if (!conv.setFormat(1, MEDIA_BUS_FMT_Y10_1X10,
                        geometry.sensorWidth, geometry.sensorHeight))
        return -1;
    std::cout << "SubsetConv formats set OK." << std::endl;

    V4L2Subdev infiniteISP(findVideoNodeByName("a0060000.infinite_isp"));
    V4L2Subdev vip4000(findVideoNodeByName("a0064000.xil_vip"));
    V4L2Subdev vip6000(findVideoNodeByName("a0066000.xil_vip"));

    if (!infiniteISP.isValid() || !vip4000.isValid() || !vip6000.isValid()) return -1;

    printf("Infinite-ISP name : %s\n", infiniteISP.getName().c_str());
    printf("VIP 4000 name     : %s\n", vip4000.getName().c_str());
    printf("VIP 6000 name     : %s\n", vip6000.getName().c_str());

    if (!infiniteISP.setFormat(0, MEDIA_BUS_FMT_Y10_1X10,
                               geometry.ispInputWidth,
                               geometry.ispInputHeight) ||
        !vip4000.setFormat(1, MEDIA_BUS_FMT_RBG888_1X24,
                           geometry.ispOutputWidth,
                           geometry.ispOutputHeight))
        return -1;

    // -------------------------
    // Read back formats
    // -------------------------
    v4l2_mbus_framefmt fmt {};

    std::cout << "\n=== Read back formats ===\n";

    if (sensor.getFormat(0, fmt)) {
        std::cout << "Sensor pad0: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (mipi.getFormat(0, fmt)) {
        std::cout << "MIPI pad0: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (mipi.getFormat(1, fmt)) {
        std::cout << "MIPI pad1 (source): " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (conv.getFormat(1, fmt)) {
        std::cout << "SubsetConv pad1: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (infiniteISP.getFormat(0, fmt)) {
        std::cout << "Infinite-ISP pad0: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (vip4000.getFormat(1, fmt)) {
        std::cout << "VIP4000 pad1: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    std::cout << "=== Done ===" << std::endl;
    return 0;
}

static infinite_isp::TuningConfig tuningConfigFromEnvironment(bool &enabled) {
    infinite_isp::TuningConfig config;
    const std::string fovMode = std::getenv("ISP_FOV_MODE")
        ? std::getenv("ISP_FOV_MODE") : "wide";
    if (fovMode == "wide") {
        config.black_levels =
            std::array<std::uint32_t, 4>{72, 82, 82, 76};
        config.color_correction_matrix =
            std::array<std::int32_t, 9>{1546, -459, -64,
                                        -291, 1899, -583,
                                        -146, -1449, 2608};
    } else {
        /* Explicitly restore the legacy profile after a wide-mode run. */
        config.black_levels =
            std::array<std::uint32_t, 4>{41, 41, 41, 41};
        config.color_correction_matrix =
            std::array<std::int32_t, 9>{2804, -1357, -424,
                                        -648, 2163, -490,
                                        -320, -1414, 2747};
    }
    const std::string mode = std::getenv("ISP_TUNING_MODE")
        ? std::getenv("ISP_TUNING_MODE") : "sensor-ae";

    enabled = mode != "off";
    if (mode == "software-ae") {
        config.ae_mode = infinite_isp::AeMode::Software;
    } else if (mode == "sensor-ae") {
        config.ae_mode = infinite_isp::AeMode::Sensor;
    } else if (mode != "hardware" && mode != "off") {
        std::cerr << "[Tuning] Unknown ISP_TUNING_MODE='" << mode
                  << "', using sensor-ae mode" << std::endl;
        config.ae_mode = infinite_isp::AeMode::Sensor;
    }

    const char *wbR = std::getenv("ISP_WB_R_GAIN");
    const char *wbB = std::getenv("ISP_WB_B_GAIN");
    if (wbR || wbB) {
        if (!wbR || !wbB) {
            std::cerr << "[Tuning] ISP_WB_R_GAIN and ISP_WB_B_GAIN must be "
                         "set together; keeping hardware AWB"
                      << std::endl;
        } else {
            char *rEnd = nullptr;
            char *bEnd = nullptr;
            const unsigned long r = std::strtoul(wbR, &rEnd, 0);
            const unsigned long b = std::strtoul(wbB, &bEnd, 0);
            if (!rEnd || *rEnd != '\0' || !bEnd || *bEnd != '\0' ||
                r > std::numeric_limits<std::uint32_t>::max() ||
                b > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "[Tuning] Invalid manual WB gains; keeping "
                             "hardware AWB"
                          << std::endl;
            } else {
                config.hardware_awb = false;
                config.manual_wb_r_gain = static_cast<std::uint32_t>(r);
                config.manual_wb_b_gain = static_cast<std::uint32_t>(b);
            }
        }
    }

    if (const char *value = std::getenv("ISP_BLC")) {
        std::array<std::uint32_t, 4> levels{};
        std::stringstream stream(value);
        bool valid = true;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            unsigned long level = 0;
            if (!(stream >> level) || level > 1023) {
                valid = false;
                break;
            }
            levels[i] = static_cast<std::uint32_t>(level);
            if (i + 1 < levels.size()) {
                char separator = 0;
                if (!(stream >> separator) || separator != ',') {
                    valid = false;
                    break;
                }
            }
        }
        stream >> std::ws;
        if (!stream.eof())
            valid = false;
        if (valid) {
            config.black_levels = levels;
        } else {
            std::cerr << "[Tuning] Invalid ISP_BLC; expected four comma-"
                         "separated RAW10 values (R,Gr,Gb,B)"
                      << std::endl;
        }
    }

    if (const char *value = std::getenv("ISP_CCM")) {
        std::array<std::int32_t, 9> matrix{};
        std::stringstream stream(value);
        bool valid = true;
        for (std::size_t i = 0; i < matrix.size(); ++i) {
            long coefficient = 0;
            if (!(stream >> coefficient) || coefficient < -32768 ||
                coefficient > 32767) {
                valid = false;
                break;
            }
            matrix[i] = static_cast<std::int32_t>(coefficient);
            if (i + 1 < matrix.size()) {
                char separator = 0;
                if (!(stream >> separator) || separator != ',') {
                    valid = false;
                    break;
                }
            }
        }
        stream >> std::ws;
        if (!stream.eof())
            valid = false;
        if (valid) {
            config.color_correction_matrix = matrix;
        } else {
            std::cerr << "[Tuning] Invalid ISP_CCM; expected nine comma-"
                         "separated signed Q10 coefficients"
                      << std::endl;
        }
    }
    return config;
}

static infinite_isp::SensorAeConfig sensorAeConfigFromEnvironment() {
    infinite_isp::SensorAeConfig config;
    if (const char *value = std::getenv("ISP_SENSOR_AGAIN"))
        config.initial_analogue_gain = static_cast<std::uint32_t>(
            std::max(0, std::atoi(value)));
    if (const char *value = std::getenv("ISP_SENSOR_EXPOSURE"))
        config.exposure = static_cast<std::uint32_t>(
            std::max(4, std::atoi(value)));
    if (const char *value = std::getenv("ISP_SENSOR_AE_FRAMES"))
        config.decision_frames = static_cast<std::uint32_t>(
            std::max(1, std::atoi(value)));
    return config;
}

static void tuningThread(infinite_isp::V4L2Backend *backend,
                         infinite_isp::AutoTuner *tuner,
                         infinite_isp::V4L2SensorBackend *sensorBackend,
                         infinite_isp::SensorAeTuner *sensorTuner) {
    unsigned int print_interval = 0;
    std::string mode = "software ISP AE";
    if (tuner->config().ae_mode == infinite_isp::AeMode::Hardware)
        mode = "hardware AE";
    else if (tuner->config().ae_mode == infinite_isp::AeMode::Sensor)
        mode = "sensor AGAIN AE";
    mode += tuner->config().hardware_awb ? " + hardware AWB" : " + manual WB";
    if (tuner->config().black_levels)
        mode += " + BLC profile";
    if (tuner->config().color_correction_matrix)
        mode += " + CCM profile";
    std::cout << "[Tuning] Metadata-driven thread started (" << mode << ")"
              << std::endl;

    while (g_running) {
        infinite_isp::FrameStatistics statistics;
        const auto result = backend->read(statistics, 500);
        if (result == infinite_isp::ReadResult::Timeout)
            continue;
        if (result == infinite_isp::ReadResult::Error) {
            if (g_running)
                std::cerr << "[Tuning] " << backend->lastError() << std::endl;
            break;
        }

        g_tuning_frames.fetch_add(1, std::memory_order_relaxed);
        g_latest_tuning_sequence.store(statistics.sequence,
                                       std::memory_order_relaxed);
        g_latest_ae_response.store(
            static_cast<std::uint32_t>(statistics.ae_response),
            std::memory_order_relaxed);
        g_latest_ae_skewness.store(statistics.ae_skewness,
                                   std::memory_order_relaxed);
        g_latest_dgain_index.store(statistics.dgain_index,
                                   std::memory_order_relaxed);
        const auto controls = tuner->process(statistics);
        if (!controls.empty() && !backend->apply(controls))
            std::cerr << "[Tuning] apply failed: " << backend->lastError()
                      << std::endl;

        if (sensorTuner && sensorBackend) {
            const auto sensorControls = sensorTuner->process(statistics);
            if (!sensorControls.empty() &&
                !sensorBackend->apply(sensorControls)) {
                std::cerr << "[Tuning] sensor apply failed: "
                          << sensorBackend->lastError() << std::endl;
            }
            g_latest_sensor_again.store(sensorTuner->currentAnalogueGain(),
                                        std::memory_order_relaxed);
        }

        if (++print_interval >= 30) {
            print_interval = 0;
            std::cout << "[Tuning] seq=" << statistics.sequence
                      << " irq=0x" << std::hex << statistics.irq_status << std::dec
                      << " AE=" << infinite_isp::toString(statistics.ae_response)
                      << " skew=" << statistics.ae_skewness
                      << " AWB(effective)=" << statistics.awb_r_gain
                      << "/" << statistics.awb_b_gain
                      << " DGAIN=" << statistics.dgain_index;
            if (sensorTuner)
                std::cout << " AGAIN=" << sensorTuner->currentAnalogueGain();
            std::cout
                      << " dropped=" << statistics.dropped_frames << std::endl;
        }
    }

    std::cout << "[Tuning] Thread stopped" << std::endl;
}

int ispPipelineRun(const PipelineGeometry &geometry) {
    constexpr int kPipelineBufferCount = 4;
    const std::string sensorCapture =
        findVideoNodeByName("vcap_mipi_csi2_rx_rpi output 0");
    const std::string ispInput =
        findVideoNodeByName("isp_pipe_video_dev input 0");
    const std::string ispOutput =
        findVideoNodeByName("isp_pipe_video_dev output 1");
    if (sensorCapture.empty() || ispInput.empty() || ispOutput.empty())
        return -1;

    std::cout << "Sensor capture: " << sensorCapture << '\n'
              << "ISP input:      " << ispInput << '\n'
              << "ISP output:     " << ispOutput << std::endl;

    const bool headless = std::getenv("ISP_HEADLESS") != nullptr;
#ifdef ISP_HAVE_MALI_EGL
    constexpr const char *kDefaultDisplayBackend = "mali";
#else
    constexpr const char *kDefaultDisplayBackend = "gstreamer";
#endif
    std::string displayBackend = std::getenv("ISP_DISPLAY_BACKEND")
        ? std::getenv("ISP_DISPLAY_BACKEND") : kDefaultDisplayBackend;
#ifndef ISP_HAVE_GSTREAMER
    if (!headless && displayBackend == "gstreamer") {
        std::cerr << "[Display] GStreamer development files were not found "
                     "at build time; falling back to OpenCV"
                  << std::endl;
        displayBackend = "opencv";
    }
#endif
#ifndef ISP_HAVE_MALI_EGL
    if (!headless && displayBackend == "mali") {
        std::cerr << "[Display] EGL/GLES2/X11 development files were not "
                     "found at build time; falling back"
                  << std::endl;
#ifdef ISP_HAVE_GSTREAMER
        displayBackend = "gstreamer";
#else
        displayBackend = "opencv";
#endif
    }
#endif
    if (displayBackend != "mali" && displayBackend != "gstreamer" &&
        displayBackend != "opencv") {
        std::cerr << "[Display] Unknown ISP_DISPLAY_BACKEND='"
                  << displayBackend << "'; using " << kDefaultDisplayBackend
                  << std::endl;
        displayBackend = kDefaultDisplayBackend;
    }
    const bool maliDisplay = !headless && displayBackend == "mali";
    const bool gstreamerDisplay = !headless && displayBackend == "gstreamer";
    const bool opencvDisplay = !headless && displayBackend == "opencv";

    int displayWidth = 960;
    if (const char *value = std::getenv("ISP_DISPLAY_WIDTH"))
        displayWidth = std::max(160, std::atoi(value));
    displayWidth = std::min(displayWidth,
                            static_cast<int>(geometry.ispOutputWidth));
    const int displayHeight = geometry.ispOutputHeight * displayWidth /
                              geometry.ispOutputWidth;
    int displayFps = maliDisplay ? 30 : 15;
    if (const char *value = std::getenv("ISP_DISPLAY_FPS"))
        displayFps = std::clamp(std::atoi(value), 1, 60);
    bool maliLowLatency = true;
    if (const char *value = std::getenv("ISP_MALI_LOW_LATENCY"))
        maliLowLatency = std::atoi(value) != 0;
    int opencvThreads = 4;
    if (const char *value = std::getenv("ISP_OPENCV_THREADS"))
        opencvThreads = std::clamp(std::atoi(value), 1, 4);
    if (opencvDisplay)
        cv::setNumThreads(opencvThreads);
    if (!headless) {
        std::cout << "[Display] backend=" << displayBackend << ", "
                  << displayWidth << 'x' << displayHeight << " @ "
                  << displayFps << " FPS";
        if (opencvDisplay)
            std::cout << ", OpenCV threads=" << opencvThreads;
        if (maliDisplay)
            std::cout << ", low-latency="
                      << (maliLowLatency ? "on" : "off");
        std::cout << std::endl;
    }

    V4L2Stream cap0(sensorCapture, false, kPipelineBufferCount);
    V4L2Stream out3(ispInput, true, kPipelineBufferCount);
    V4L2Stream cap2(ispOutput, false, kPipelineBufferCount);
    // GstKmsViewer viewer("/dev/video2", 1920, 1080, "YUY2");

    if (!cap0.openDevice() || !out3.openDevice() || !cap2.openDevice())
        return -1;

    if (!cap0.setFormat(geometry.ispInputWidth, geometry.ispInputHeight,
                        v4l2_fourcc('X','Y','1','0'),
                        geometry.rawStrideBytes) ||
        !cap0.setCompose(0, 0, geometry.sensorWidth,
                        geometry.sensorHeight) ||
        !out3.setFormat(geometry.ispInputWidth, geometry.ispInputHeight,
                        v4l2_fourcc('X','Y','1','0'),
                        geometry.rawStrideBytes))
        return -1;
    /* Match Vitis: RGB video bus written to memory in BGR24 order. */
    if (!cap2.setFormat(geometry.ispOutputWidth, geometry.ispOutputHeight,
                        V4L2_PIX_FMT_BGR24) ||
        !cap2.setCompose(0, 0, geometry.ispOutputWidth,
                         geometry.ispOutputHeight))
        return -1;

    if (!cap0.initMMap())
        return -1;
    // Wide mode writes a smaller compose rectangle into the fixed ISP canvas.
    cap0.clearMappedBuffers();
    std::cout << "[Pipeline] DMA canvas=" << geometry.ispInputWidth << 'x'
              << geometry.ispInputHeight << ", active="
              << geometry.sensorWidth << 'x' << geometry.sensorHeight
              << " at x=0, stride="
              << cap0.frameStride() << " bytes" << std::endl;

#ifdef ISP_HAVE_MALI_EGL
    std::unique_ptr<MaliEglDmaBufViewer> maliViewer;
    if (maliDisplay) {
        if (!cap2.initMMap() || !cap2.exportAllDMABuf())
            return -1;
        maliViewer = std::make_unique<MaliEglDmaBufViewer>(
            geometry.ispOutputWidth, geometry.ispOutputHeight,
            displayWidth, displayHeight);
        std::vector<int> displayFds;
        displayFds.reserve(cap2.buffers.size());
        for (const auto &buffer : cap2.buffers)
            displayFds.push_back(buffer.fd);
        if (!maliViewer->setBuffers(displayFds.data(), displayFds.size(),
                                    cap2.frameStride())) {
            std::cerr << "[Display] VIP DMA-BUF setup failed: "
                      << maliViewer->lastError() << std::endl;
            return -1;
        }
        std::cout << "[Display] zero-copy buffers="
                  << displayFds.size() << ", stride="
                  << cap2.frameStride() << std::endl;
    } else
#endif
    if (!cap2.initMMap())
        return -1;

    if (!cap0.exportAllDMABuf() ||
        !out3.initDMABufImport(cap0.buffers.size()))
        return -1;

    if (!cap0.queueAllCapture() || !cap2.queueAllCapture())
        return -1;

    if (!cap0.startStreaming() || !cap2.startStreaming())
        return -1;
    // viewer.start();

    bool tuningEnabled = false;
    const auto tuningConfig = tuningConfigFromEnvironment(tuningEnabled);
    std::unique_ptr<infinite_isp::AutoTuner> tuner;
    std::unique_ptr<infinite_isp::V4L2Backend> tuningBackend;
    std::unique_ptr<infinite_isp::SensorAeTuner> sensorTuner;
    std::unique_ptr<infinite_isp::V4L2SensorBackend> sensorBackend;
    std::thread tuning;

    if (tuningEnabled) {
        const std::string statDevice = infinite_isp::findStatDevice();
        if (statDevice.empty()) {
            std::cerr << "[Tuning] Statistics node not found; tuning disabled"
                      << std::endl;
        } else {
            tuner = std::make_unique<infinite_isp::AutoTuner>(tuningConfig);
            tuningBackend =
                std::make_unique<infinite_isp::V4L2Backend>(statDevice);
            if (!tuningBackend->start(4)) {
                std::cerr << "[Tuning] " << tuningBackend->lastError()
                          << std::endl;
                tuningBackend.reset();
                tuner.reset();
            } else if (tuningConfig.ae_mode == infinite_isp::AeMode::Sensor) {
                const std::string sensorDevice =
                    findVideoNodeByName("xlnx-imx219 6-0010");
                sensorTuner = std::make_unique<infinite_isp::SensorAeTuner>(
                    sensorAeConfigFromEnvironment());
                sensorBackend =
                    std::make_unique<infinite_isp::V4L2SensorBackend>(
                        sensorDevice);
                if (sensorDevice.empty() || !sensorBackend->open() ||
                    !sensorBackend->apply(sensorTuner->initialControls())) {
                    std::cerr << "[Tuning] Sensor AE initialization failed: "
                              << sensorBackend->lastError() << std::endl;
                    sensorBackend.reset();
                    sensorTuner.reset();
                    tuningBackend.reset();
                    tuner.reset();
                } else {
                    g_latest_sensor_again.store(
                        sensorTuner->currentAnalogueGain(),
                        std::memory_order_relaxed);
                    std::cout << "[Tuning] Sensor control device: "
                              << sensorDevice << std::endl;
                }
            }

            if (tuningBackend &&
                !tuningBackend->apply(tuner->initialControls())) {
                std::cerr << "[Tuning] Initial controls failed: "
                          << tuningBackend->lastError() << std::endl;
                sensorBackend.reset();
                sensorTuner.reset();
                tuningBackend.reset();
                tuner.reset();
            } else if (tuningBackend) {
                std::cout << "[Tuning] Statistics/control device: "
                          << statDevice << std::endl;
                tuning = std::thread(tuningThread, tuningBackend.get(),
                                     tuner.get(), sensorBackend.get(),
                                     sensorTuner.get());
            }
        }
    } else {
        std::cout << "[Tuning] Disabled by ISP_TUNING_MODE=off" << std::endl;
    }

    IndexQueue ready_q(8);

    /* cap0 DQ thread */
    std::thread t0([&](){
        while (g_running) {
            int idx = cap0.dequeue(500);
            if (idx >= 0)
            {
                g_sensor_frames.fetch_add(1, std::memory_order_relaxed);
                ready_q.push(idx);
            }
        }
    });

    /* out3 worker */
    std::thread t1([&](){
        bool started = false;
        size_t inFlight = 0;
        while (g_running) {
            int idx;
            if (!started) {
                if (!ready_q.pop(idx))
                    continue;
                if (!out3.queueDMABuf(
                    idx,
                    cap0.buffers[idx].fd,
                    cap0.bufferLen(idx)
                )) {
                    g_running = false;
                    cap0.queueCapture(idx);
                    break;
                }
                ++inFlight;
                g_ispin_frames.fetch_add(1, std::memory_order_relaxed);
                if (!out3.startStreaming()) {
                    g_running = false;
                    break;
                }
                started = true;
            }

            while (inFlight < out3.buffers.size() && ready_q.tryPop(idx)) {
                if (!out3.queueDMABuf(
                    idx,
                    cap0.buffers[idx].fd,
                    cap0.bufferLen(idx)
                )) {
                    g_running = false;
                    cap0.queueCapture(idx);
                    break;
                }
                ++inFlight;
                g_ispin_frames.fetch_add(1, std::memory_order_relaxed);
            }
            if (!g_running)
                break;

            int done = out3.dequeue(100);
            if (done >= 0) {
                --inFlight;
                if (!cap0.queueCapture(done)) {
                    g_running = false;
                    break;
                }
            }
        }
    });

    int saveAfterFrames = 30;
    if (const char *value = std::getenv("ISP_CAPTURE_FRAME"))
        saveAfterFrames = std::max(1, std::atoi(value));
    int captureSeries = 1;
    if (const char *value = std::getenv("ISP_CAPTURE_SERIES"))
        captureSeries = std::clamp(std::atoi(value), 1, 100);
    const std::string capturePrefix = std::getenv("ISP_CAPTURE_PREFIX") ?
        std::getenv("ISP_CAPTURE_PREFIX") : "isp_capture";
    std::vector<cv::Mat> capturedFrames;
    if (headless)
        capturedFrames.reserve(captureSeries);
    const cv::Rect measureRoi = measurementRoiFromEnvironment(
        geometry.ispOutputWidth, geometry.ispOutputHeight);
    std::ofstream measureCsv;
    if (const char *value = std::getenv("ISP_MEASURE_CSV")) {
        if (maliDisplay) {
            std::cerr << "[Measure] ISP_MEASURE_CSV requires the gstreamer, "
                         "opencv, or headless backend; image sampling is "
                         "disabled for the Mali zero-copy path"
                      << std::endl;
        } else {
            measureCsv.open(value);
        }
        if (!measureCsv) {
            if (!maliDisplay) {
                std::cerr << "Cannot open ISP_MEASURE_CSV='" << value << "'"
                          << std::endl;
            }
        } else {
            measureCsv << "frame,timestamp_us,mean_b,mean_g,mean_r,mean_y,"
                          "roi_b,roi_g,roi_r,roi_y,tuning_seq,ae_response,"
                          "ae_skewness,dgain_index,sensor_again\n";
            std::cout << "[Measure] CSV: " << value << ", ROI="
                      << measureRoi.x << ',' << measureRoi.y << ','
                      << measureRoi.width << ',' << measureRoi.height
                      << std::endl;
        }
    }

#ifdef ISP_HAVE_GSTREAMER
    std::unique_ptr<GstViewer> gstViewer;
    if (gstreamerDisplay) {
        gstViewer = std::make_unique<GstViewer>(
            geometry.ispOutputWidth, geometry.ispOutputHeight,
            displayWidth, displayHeight,
            displayFps, g_running);
        if (!gstViewer->start()) {
            std::cerr << "[Display] GStreamer start failed: "
                      << gstViewer->lastError() << std::endl;
            g_running = false;
        }
    }
#endif
    /* cap2 capture plus GStreamer or OpenCV preview */
    std::thread t2([&](){
#ifdef ISP_HAVE_MALI_EGL
        if (maliDisplay && !maliViewer->start()) {
            std::cerr << "[Display] Mali EGL start failed: "
                      << maliViewer->lastError() << std::endl;
            g_running = false;
            return;
        }
#endif
        if (opencvDisplay) {
            cv::namedWindow("ISP", cv::WINDOW_NORMAL);
            cv::resizeWindow("ISP", displayWidth, displayHeight);
        }
        const auto displayPeriod = std::chrono::microseconds(
            1000000 / displayFps);
        auto nextDisplay = std::chrono::steady_clock::now();
        if (maliDisplay && maliLowLatency)
            nextDisplay += displayPeriod;
        std::deque<int> pendingMaliBuffers;
        int frameNumber = 0;
        while (g_running) {
#ifdef ISP_HAVE_MALI_EGL
            if (maliDisplay) {
                for (auto it = pendingMaliBuffers.begin();
                     it != pendingMaliBuffers.end();) {
                    if (maliViewer->bufferComplete(*it)) {
                        if (!cap2.queueCapture(*it))
                            g_running = false;
                        it = pendingMaliBuffers.erase(it);
                    } else {
                        if (!maliViewer->lastError().empty()) {
                            std::cerr << "[Display] Mali fence query failed: "
                                      << maliViewer->lastError() << std::endl;
                            g_running = false;
                        }
                        ++it;
                    }
                }
            }
#endif
            DequeueInfo dequeueInfo{};
            int idx = cap2.dequeue(500, &dequeueInfo);
            if (idx >= 0) {
                g_ispout_frames.fetch_add(1, std::memory_order_relaxed);
                ++frameNumber;
#ifdef ISP_HAVE_MALI_EGL
                if (maliDisplay) {
                    auto now = std::chrono::steady_clock::now();
                    if (maliLowLatency && now < nextDisplay) {
                        g_display_stale_frames.fetch_add(
                            1, std::memory_order_relaxed);
                        if (!cap2.queueCapture(idx))
                            g_running = false;
                        continue;
                    }
                    if (!maliLowLatency && now < nextDisplay) {
                        std::this_thread::sleep_until(nextDisplay);
                        now = std::chrono::steady_clock::now();
                    }
                    if (!maliViewer->render(idx)) {
                        maliViewer->waitForBuffer(idx);
                        if (!maliViewer->lastError().empty()) {
                            std::cerr << "[Display] Mali EGL render failed: "
                                      << maliViewer->lastError() << std::endl;
                        }
                        g_running = false;
                    } else {
                        pendingMaliBuffers.push_back(idx);
                        idx = -1;
                        g_display_frames.fetch_add(
                            1, std::memory_order_relaxed);
                        recordDisplayAge(dequeueInfo);
                    }
                    nextDisplay += displayPeriod;
                    now = std::chrono::steady_clock::now();
                    if (nextDisplay <= now)
                        nextDisplay = now + displayPeriod;
                    if (idx >= 0 && !cap2.queueCapture(idx))
                        g_running = false;
                    continue;
                }
#endif
                cv::Mat bgr(cap2.frameHeight(), cap2.frameWidth(), CV_8UC3,
                               cap2.bufferPtr(idx), cap2.frameStride());

                if (measureCsv) {
                    const cv::Scalar meanBgr = sampledMeanBgr(
                        bgr, cv::Rect(0, 0, bgr.cols, bgr.rows));
                    const cv::Scalar roiBgr = sampledMeanBgr(bgr, measureRoi);
                    const double meanY = 0.114 * meanBgr[0] +
                                         0.587 * meanBgr[1] +
                                         0.299 * meanBgr[2];
                    const double roiY = 0.114 * roiBgr[0] +
                                        0.587 * roiBgr[1] +
                                        0.299 * roiBgr[2];
                    const auto timestampUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    measureCsv << frameNumber << ',' << timestampUs << ','
                               << meanBgr[0] << ',' << meanBgr[1] << ','
                               << meanBgr[2] << ',' << meanY << ','
                               << roiBgr[0] << ',' << roiBgr[1] << ','
                               << roiBgr[2] << ',' << roiY << ','
                               << g_latest_tuning_sequence.load(
                                      std::memory_order_relaxed)
                               << ','
                               << g_latest_ae_response.load(
                                      std::memory_order_relaxed)
                               << ','
                               << g_latest_ae_skewness.load(
                                      std::memory_order_relaxed)
                               << ','
                               << g_latest_dgain_index.load(
                                      std::memory_order_relaxed)
                               << ','
                               << g_latest_sensor_again.load(
                                      std::memory_order_relaxed)
                               << '\n';
                }

                if (headless && frameNumber >= saveAfterFrames) {
                    capturedFrames.emplace_back(bgr.clone());
                    if (capturedFrames.size() >=
                        static_cast<std::size_t>(captureSeries))
                        g_running = false;
                } else if (!headless) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= nextDisplay) {
#ifdef ISP_HAVE_GSTREAMER
                        if (gstreamerDisplay) {
                            if (!gstViewer->pushBgrFrame(
                                    cap2.bufferPtr(idx), cap2.frameStride())) {
                                std::cerr << "[Display] GStreamer push failed: "
                                          << gstViewer->lastError()
                                          << std::endl;
                                g_running = false;
                            } else {
                                g_display_frames.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        } else
#endif
                        {
                            cv::Mat preview;
                            cv::resize(bgr, preview,
                                       cv::Size(displayWidth, displayHeight),
                                       0.0, 0.0, cv::INTER_LINEAR);
                            cv::imshow("ISP", preview);
                            cv::waitKey(1);
                            g_display_frames.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        nextDisplay += displayPeriod;
                        if (nextDisplay <= now)
                            nextDisplay = now + displayPeriod;
                    }
                }
                cap2.queueCapture(idx);
            }
        }
#ifdef ISP_HAVE_MALI_EGL
        if (maliDisplay) {
            for (int index : pendingMaliBuffers) {
                if (maliViewer->waitForBuffer(index))
                    cap2.queueCapture(index);
            }
            maliViewer->stop();
        }
#endif
    });

    /* FPS monitor thread */
    std::thread monitor([&](){
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t s = g_sensor_frames.exchange(0, std::memory_order_relaxed);
            uint64_t in = g_ispin_frames.exchange(0, std::memory_order_relaxed);
            uint64_t out = g_ispout_frames.exchange(0, std::memory_order_relaxed);
            uint64_t display = g_display_frames.exchange(
                0, std::memory_order_relaxed);
            uint64_t tune = g_tuning_frames.exchange(0, std::memory_order_relaxed);
            uint64_t stale = g_display_stale_frames.exchange(
                0, std::memory_order_relaxed);
            uint64_t ageSamples = g_display_age_samples.exchange(
                0, std::memory_order_relaxed);
            uint64_t ageTotal = g_display_age_total_us.exchange(
                0, std::memory_order_relaxed);
            uint64_t ageMaximum = g_display_age_max_us.exchange(
                0, std::memory_order_relaxed);
            printf("FPS -> sensor: %3llu, ispin: %3llu, ispout: %3llu, "
                   "display: %3llu, tuning: %3llu",
                   (unsigned long long)s, (unsigned long long)in,
                   (unsigned long long)out, (unsigned long long)display,
                   (unsigned long long)tune);
            if (ageSamples) {
                printf(", display-age: avg=%.1fms max=%.1fms",
                       static_cast<double>(ageTotal) / ageSamples / 1000.0,
                       static_cast<double>(ageMaximum) / 1000.0);
            }
            if (stale)
                printf(", stale-skipped: %llu",
                       (unsigned long long)stale);
            printf("\n");
        }
    });

    while (g_running)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    /* Cleanup */
    ready_q.stop();
    t0.join();
    t1.join();
    t2.join();
#ifdef ISP_HAVE_GSTREAMER
    if (gstViewer) {
        gstViewer->stop();
        if (!gstViewer->lastError().empty())
            std::cerr << "[Display] " << gstViewer->lastError() << std::endl;
    }
#endif
    if (tuning.joinable())
        tuning.join();
    if (tuningBackend) {
        if (tuningConfig.ae_mode != infinite_isp::AeMode::Hardware) {
            infinite_isp::ControlUpdate restore;
            restore.auto_gain = true;
            if (!tuningBackend->apply(restore))
                std::cerr << "[Tuning] Failed to restore hardware AE: "
                          << tuningBackend->lastError() << std::endl;
        }
        tuningBackend->stop();
    }
    monitor.join();

    if (!capturedFrames.empty()) {
        if (capturedFrames.size() == 1) {
            const cv::Mat &bgr = capturedFrames.front();
            cv::Mat swapped;
            cv::cvtColor(bgr, swapped, cv::COLOR_RGB2BGR);
            cv::imwrite(capturePrefix + "_bgr.png", bgr);
            cv::imwrite(capturePrefix + "_rgb.png", swapped);
            std::ofstream raw(capturePrefix + ".raw", std::ios::binary);
            raw.write(reinterpret_cast<const char *>(bgr.data),
                      static_cast<std::streamsize>(bgr.total() *
                                                   bgr.elemSize()));
            std::cout << "Saved " << capturePrefix
                      << "_{bgr,rgb}.png and .raw\n"
                      << "Mean BGR as BGR24: " << cv::mean(bgr) << '\n'
                      << "Mean BGR if swapped: " << cv::mean(swapped)
                      << std::endl;
        } else {
            for (std::size_t i = 0; i < capturedFrames.size(); ++i) {
                std::ostringstream path;
                path << capturePrefix << '_' << std::setfill('0')
                     << std::setw(3) << i << "_bgr.png";
                cv::imwrite(path.str(), capturedFrames[i]);
            }
            std::cout << "Saved " << capturedFrames.size()
                      << " BGR frames as " << capturePrefix
                      << "_NNN_bgr.png" << std::endl;
        }
    }
    // viewer.stop();
    return 0;
}

int main(){
    signal(SIGINT, sigint_handler);

    PipelineGeometry geometry{};
    if (!pipelineGeometryFromEnvironment(geometry))
        return 1;

    if (setSubDevFmt(geometry) < 0)
        return 1;
    if (ispPipelineRun(geometry) < 0)
        return 1;

    std::cout << "Exited\n";
    return 0;
}
