#include <iostream>
#include <csignal>
#include <cerrno>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <opencv2/opencv.hpp>
#include "V4L2Subdev.hpp"
#include "V4L2Stream.hpp"
#include "infinite_isp/tuning.hpp"
#include "infinite_isp/v4l2_backend.hpp"

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_sensor_frames{0};
static std::atomic<uint64_t> g_ispin_frames{0};
static std::atomic<uint64_t> g_ispout_frames{0};
static std::atomic<uint64_t> g_tuning_frames{0};
static void sigint_handler(int){ g_running = false; }

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

int setSubDevFmt() {
    std::cout << "=== V4L2 Subdev Format Setter ===" << std::endl;

    // -------------------------
    // 1. Sensor (imx219)
    // -------------------------
    V4L2Subdev sensor(findVideoNodeByName("xlnx-imx219 6-0010"));
    if (!sensor.isValid()) return -1;

    std::cout << "Sensor name: " << sensor.getName() << std::endl;

    sensor.setFormat(
        0,                              // pad
        MEDIA_BUS_FMT_SRGGB10_1X10,     // raw Bayer 10bit
        1992, 1152,                     // resolution
        V4L2_FIELD_NONE
    );
    std::cout << "Sensor format set OK." << std::endl;

    // -------------------------
    // 2. MIPI RX
    // -------------------------
    V4L2Subdev mipi(findVideoNodeByName("a0030000.mipi_rx_to_video"));
    if (!mipi.isValid()) return -1;

    std::cout << "MIPI name: " << mipi.getName() << std::endl;

    // pad0 (sink)
    mipi.setFormat(0, MEDIA_BUS_FMT_SRGGB10_1X10, 1992, 1152);
    // pad1 (source)
    mipi.setFormat(1, MEDIA_BUS_FMT_SRGGB10_1X10, 1992, 1152);
    std::cout << "MIPI formats set OK." << std::endl;

    // -------------------------
    // 3. Subset Converter (ISP block)
    // -------------------------
    V4L2Subdev conv(findVideoNodeByName("axi:camif_rpi_axis_subsetconv"));
    if (!conv.isValid()) return -1;

    std::cout << "SubsetConv name: " << conv.getName() << std::endl;

    // pad0: input RAW
    conv.setFormat(0, MEDIA_BUS_FMT_SRGGB10_1X10, 1992, 1152);
    // pad1: output Y10
    conv.setFormat(1, MEDIA_BUS_FMT_Y10_1X10,     1992, 1152);
    std::cout << "SubsetConv formats set OK." << std::endl;

    V4L2Subdev infiniteISP(findVideoNodeByName("a0060000.infinite_isp"));
    V4L2Subdev vip4000(findVideoNodeByName("a0064000.xil_vip"));
    V4L2Subdev vip6000(findVideoNodeByName("a0066000.xil_vip"));

    if (!infiniteISP.isValid() || !vip4000.isValid() || !vip6000.isValid()) return -1;

    printf("Infinite-ISP name : %s\n", infiniteISP.getName().c_str());
    printf("VIP 4000 name     : %s\n", vip4000.getName().c_str());
    printf("VIP 6000 name     : %s\n", vip6000.getName().c_str());

    infiniteISP.setFormat(0, MEDIA_BUS_FMT_Y10_1X10, 1992, 1152);
    vip4000.setFormat(1, MEDIA_BUS_FMT_RBG888_1X24, 1920, 1080);

    // -------------------------
    // Read back formats
    // -------------------------
    v4l2_mbus_framefmt fmt {};

    std::cout << "\n=== Read back formats ===\n";

    if (sensor.getFormat(0, fmt)) {
        std::cout << "Sensor pad0: " << fmt.width << "x" << fmt.height
                  << " code=0x" << std::hex << fmt.code << std::dec << std::endl;
    }

    if (mipi.getFormat(1, fmt)) {
        std::cout << "MIPI pad1: " << fmt.width << "x" << fmt.height
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
    const std::string mode = std::getenv("ISP_TUNING_MODE")
        ? std::getenv("ISP_TUNING_MODE") : "hardware";

    enabled = mode != "off";
    if (mode == "software-ae") {
        config.ae_mode = infinite_isp::AeMode::Software;
    } else if (mode != "hardware" && mode != "off") {
        std::cerr << "[Tuning] Unknown ISP_TUNING_MODE='" << mode
                  << "', using hardware mode" << std::endl;
    }
    return config;
}

static void tuningThread(infinite_isp::V4L2Backend *backend,
                         infinite_isp::AutoTuner *tuner) {
    unsigned int print_interval = 0;
    const char *mode = tuner->config().ae_mode == infinite_isp::AeMode::Hardware
        ? "hardware AE/AWB" : "software AE + hardware AWB";
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
        const auto controls = tuner->process(statistics);
        if (!controls.empty() && !backend->apply(controls))
            std::cerr << "[Tuning] apply failed: " << backend->lastError()
                      << std::endl;

        if (++print_interval >= 30) {
            print_interval = 0;
            std::cout << "[Tuning] seq=" << statistics.sequence
                      << " irq=0x" << std::hex << statistics.irq_status << std::dec
                      << " AE=" << infinite_isp::toString(statistics.ae_response)
                      << " skew=" << statistics.ae_skewness
                      << " AWB(effective)=" << statistics.awb_r_gain
                      << "/" << statistics.awb_b_gain
                      << " DGAIN=" << statistics.dgain_index
                      << " dropped=" << statistics.dropped_frames << std::endl;
        }
    }

    std::cout << "[Tuning] Thread stopped" << std::endl;
}

int ispPipelineRun() {
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

    V4L2Stream cap0(sensorCapture, false, 6);
    V4L2Stream out3(ispInput, true, 6);
    V4L2Stream cap2(ispOutput, false, 6);
    // GstKmsViewer viewer("/dev/video2", 1920, 1080, "YUY2");

    cap0.openDevice();
    out3.openDevice();
    cap2.openDevice();

    cap0.setFormat(1992,1152,v4l2_fourcc('X','Y','1','0'));
    out3.setFormat(1992,1152,v4l2_fourcc('X','Y','1','0'));
    /* Match Vitis: RGB video bus written to memory in BGR24 order. */
    cap2.setFormat(1920,1080,V4L2_PIX_FMT_BGR24);

    cap0.initMMap();
    cap2.initMMap();

    cap0.exportAllDMABuf();
    out3.initDMABufImport(cap0.buffers.size());

    cap0.queueAllCapture();
    cap2.queueAllCapture();

    cap0.startStreaming();
    cap2.startStreaming();
    // viewer.start();

    bool tuningEnabled = false;
    const auto tuningConfig = tuningConfigFromEnvironment(tuningEnabled);
    std::unique_ptr<infinite_isp::AutoTuner> tuner;
    std::unique_ptr<infinite_isp::V4L2Backend> tuningBackend;
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
            } else if (!tuningBackend->apply(tuner->initialControls())) {
                std::cerr << "[Tuning] Initial controls failed: "
                          << tuningBackend->lastError() << std::endl;
                tuningBackend.reset();
                tuner.reset();
            } else {
                std::cout << "[Tuning] Statistics/control device: "
                          << statDevice << std::endl;
                tuning = std::thread(tuningThread, tuningBackend.get(),
                                     tuner.get());
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
        while (g_running) {
            int idx;
            if (!ready_q.pop(idx))
                continue;

            if (!started) {
                out3.startStreaming();
                started = true;
            }

            out3.queueDMABuf(
                idx,
                cap0.buffers[idx].fd,
                cap0.bufferLen(idx)
            );

            g_ispin_frames.fetch_add(1, std::memory_order_relaxed);
            int done = out3.dequeue(1000);
            if (done >= 0)
            {
                cap0.queueCapture(done);
            }
        }
    });

    const bool headless = std::getenv("ISP_HEADLESS") != nullptr;
    int saveAfterFrames = 30;
    if (const char *value = std::getenv("ISP_CAPTURE_FRAME"))
        saveAfterFrames = std::max(1, std::atoi(value));
    const std::string capturePrefix = std::getenv("ISP_CAPTURE_PREFIX") ?
        std::getenv("ISP_CAPTURE_PREFIX") : "isp_capture";

    /* cap2 display / headless capture */
    std::thread t2([&](){
        if (!headless)
            cv::namedWindow("ISP", cv::WINDOW_NORMAL);
        int frameNumber = 0;
        while (g_running) {
            int idx = cap2.dequeue(500);
            if (idx >= 0) {
                g_ispout_frames.fetch_add(1, std::memory_order_relaxed);
                cv::Mat bgr(cap2.frameHeight(), cap2.frameWidth(), CV_8UC3,
                               cap2.bufferPtr(idx), cap2.frameStride());

                if (headless && ++frameNumber >= saveAfterFrames) {
                    cv::Mat swapped;
                    cv::cvtColor(bgr, swapped, cv::COLOR_RGB2BGR);
                    cv::imwrite(capturePrefix + "_bgr.png", bgr);
                    cv::imwrite(capturePrefix + "_rgb.png", swapped);

                    const size_t rawBytes = std::min(
                        cap2.bufferLen(idx),
                        static_cast<size_t>(cap2.frameStride()) * cap2.frameHeight());
                    std::ofstream raw(capturePrefix + ".raw", std::ios::binary);
                    raw.write(static_cast<const char *>(cap2.bufferPtr(idx)), rawBytes);

                    const cv::Scalar meanBgr = cv::mean(bgr);
                    const cv::Scalar meanRgb = cv::mean(swapped);
                    std::cout << "Saved " << capturePrefix
                              << "_{bgr,rgb}.png and .raw\n"
                              << "Mean BGR as BGR24: " << meanBgr << '\n'
                              << "Mean BGR if swapped: " << meanRgb << std::endl;
                    g_running = false;
                } else if (!headless) {
                    cv::imshow("ISP", bgr);
                    cv::waitKey(1);
                }
                cap2.queueCapture(idx);
            }
        }
    });

    /* FPS monitor thread */
    std::thread monitor([&](){
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t s = g_sensor_frames.exchange(0, std::memory_order_relaxed);
            uint64_t in = g_ispin_frames.exchange(0, std::memory_order_relaxed);
            uint64_t out = g_ispout_frames.exchange(0, std::memory_order_relaxed);
            uint64_t tune = g_tuning_frames.exchange(0, std::memory_order_relaxed);
            printf("FPS -> sensor: %3llu, ispin: %3llu, ispout: %3llu, tuning: %3llu\n",
                   (unsigned long long)s, (unsigned long long)in,
                   (unsigned long long)out, (unsigned long long)tune);
        }
    });

    while (g_running)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    /* Cleanup */
    ready_q.stop();
    t0.join();
    t1.join();
    t2.join();
    if (tuning.joinable())
        tuning.join();
    if (tuningBackend) {
        if (tuningConfig.ae_mode == infinite_isp::AeMode::Software) {
            infinite_isp::ControlUpdate restore;
            restore.auto_gain = true;
            if (!tuningBackend->apply(restore))
                std::cerr << "[Tuning] Failed to restore hardware AE: "
                          << tuningBackend->lastError() << std::endl;
        }
        tuningBackend->stop();
    }
    monitor.join();
    // viewer.stop();
    return 0;
}

int main(){
    signal(SIGINT, sigint_handler);

    if (setSubDevFmt() < 0)
        return 1;
    if (ispPipelineRun() < 0)
        return 1;

    std::cout << "Exited\n";
    return 0;
}
