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
#include "ISPTuner.hpp"

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

/*
 * ISP Tuning Thread
 *
 * The hardware AWB continuously computes optimal R/B gains and exposes
 * them via read-only registers (REG_AWB_FINAL_RGAIN, FINAL_BGAIN).
 * However, these are NOT automatically applied to the WB pipeline.
 * This thread reads the AWB results and writes them to the WB module.
 *
 * The hardware AE computes an exposure response (0=under, 1=proper, 2=over)
 * which this thread uses to adjust the digital gain (DGAIN).
 *
 * The stat metadata node (/dev/video with name "xil-isp-lite_stat")
 * provides per-frame AE/AWB statistics via V4L2 meta capture buffers.
 * We use this for timing (each buffer = one frame completed).
 *
 * As a fallback, we can also read AE/AWB results directly via V4L2
 * EXT_CTRLS on the ISP subdev node.
 */
static void tuningThread(IspControl *ctrl, IspStatReader *statReader) {
    int tuningInterval = 0;

    std::cout << "[Tuning] Thread started (poll mode via EXT_CTRLS)" << std::endl;
    int debugPrint = 0;

    while (g_running) {
        /* Poll AWB/AE registers directly every ~100ms (~10 Hz) */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        /*
         * 1. Apply HW AWB result gains to WB pipeline.
         *    The AWB block continuously computes optimal gains;
         *    we read them and write to WB registers so they take effect.
         */
        if (ctrl->applyAwbToWb()) {
            g_tuning_frames.fetch_add(1, std::memory_order_relaxed);
        }

        /* Debug: print AWB/AE values every 2 seconds */
        if (++debugPrint >= 20) {
            debugPrint = 0;
            uint32_t rGain = 0, bGain = 0;
            ctrl->getAwbResult(rGain, bGain);
            uint32_t aeResp = 0, aeSkew = 0, aeDone = 0;
            ctrl->getAeStatus(aeResp, aeSkew, aeDone);
            uint32_t dgIdx = 0;
            ctrl->getDGainIndex(dgIdx);
            uint32_t wbR = 0, wbB = 0;
            ctrl->getWbGains(wbR, wbB);
            printf("[Tuning] AWB(out): R=%u B=%u | AE: resp=%u skew=%u done=%u | "
                   "DGAIN idx=%u | WB(applied): R=%u B=%u\n",
                   rGain, bGain, aeResp, aeSkew, aeDone, dgIdx, wbR, wbB);
        }

        /*
         * 2. Adjust digital gain based on AE response.
         *    Run every 4 cycles (~2.5 Hz) to avoid oscillation.
         */
        if (++tuningInterval >= 4) {
            tuningInterval = 0;
            ctrl->adjustExposure();
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

    /* ── Tuning Setup ───────────────────────────────────── */
    /*
     * Dynamic ISP tuning via AE/AWB stats.
     * DISABLED by default — the static init values from isp_init.h
     * (matching the Vitis firmware) produce correct images.
     *
     * To enable: #define ENABLE_ISP_TUNING 1
     *   - reads HW AWB gains → writes WB block
     *   - reads AE response  → adjusts digital gain
     */
    #define ENABLE_ISP_TUNING 0

    #if ENABLE_ISP_TUNING
    /*
     * V4L2 custom controls (CID_ISP_WB, CID_ISP_DGAIN, etc.) are
     * registered on the stat metadata node (xil-isp-lite_stat), NOT
     * on the subdev.  Open the stat node for control I/O and for
     * metadata streaming.
     */
    std::string statDevPath = findIspStatDev();
    std::string ispSubdevPath = findIspSubdev();

    std::unique_ptr<IspControl> ispCtrl;
    std::unique_ptr<IspStatReader> statReader;
    std::thread tuning;

    /* Prefer stat node for controls; fall back to subdev */
    std::string ctrlDev;
    if (!statDevPath.empty()) {
        ctrlDev = statDevPath;
    } else if (!ispSubdevPath.empty()) {
        ctrlDev = ispSubdevPath;
    }

    if (!ctrlDev.empty()) {
        std::cout << "[Tuning] ISP ctrl device: " << ctrlDev << std::endl;
        ispCtrl = std::make_unique<IspControl>(ctrlDev);
        if (!ispCtrl->ok()) {
            std::cerr << "[Tuning] WARNING: Cannot open ISP control" << std::endl;
            ispCtrl.reset();
        }
    }

    if (!statDevPath.empty()) {
        std::cout << "[Tuning] Found stat node: " << statDevPath << std::endl;
        statReader = std::make_unique<IspStatReader>(statDevPath);
        if (statReader->openDevice() && statReader->initMMap(4)) {
            statReader->queueAll();
            statReader->startStreaming();
        } else {
            std::cerr << "[Tuning] WARNING: Cannot init stat reader, "
                      << "falling back to poll mode" << std::endl;
            statReader.reset();
        }
    } else {
        std::cerr << "[Tuning] WARNING: Stat node not found. "
                  << "Tuning will poll via EXT_CTRLS." << std::endl;
    }

    if (ispCtrl) {
        tuning = std::thread(tuningThread, ispCtrl.get(), statReader.get());
    } else {
        std::cerr << "[Tuning] WARNING: No ISP control device found. "
                  << "Tuning disabled." << std::endl;
    }
    #else
    std::cout << "[Tuning] Dynamic tuning disabled (ENABLE_ISP_TUNING=0). "
              << "Using static init values from isp_init.h." << std::endl;
    #endif
    /* ──────────────────────────────────────────────────── */

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
    #if ENABLE_ISP_TUNING
    if (statReader) {
        statReader->stopStreaming();
    }
    #endif
    ready_q.stop();
    t0.join();
    t1.join();
    t2.join();
    monitor.join();
    #if ENABLE_ISP_TUNING
    if (tuning.joinable())
        tuning.join();
    #endif
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
