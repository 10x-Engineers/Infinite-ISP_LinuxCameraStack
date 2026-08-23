#pragma once
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/v4l2-controls.h>      /* for V4L2_CID_USER_BASE, ext_control */

/*
 * ISPTuner — reads ISP AE/AWB statistics from the metadata video node
 * and writes back computed WB gains / digital gain via V4L2 controls.
 *
 * Hardware constraints:
 *  - The current xil_isp_lite AXI wrapper does NOT expose per-channel
 *    histograms via registers. Only scalar results are available.
 *  - AE provides:   ae_response [1:0] (0=under,1=proper,2=over),
 *                   ae_skewness [15:0], ae_done
 *  - AWB provides:  final_r_gain [11:0], final_b_gain [11:0]
 *  - AWB_DONE interrupt is not connected in HW — AWB runs continuously.
 *    Gains can be read at any time after the first few frames.
 */

/* ── Local copy of ISP stat structures (matches kernel xil-isp-lite.h) ── */

#define V4L2_META_FMT_XIL_ISP_LITE_STAT  v4l2_fourcc('X','I','S','P')

struct xil_isp_lite_stat_ae_result {
	uint64_t timestamp_ns;
	uint32_t frame_sequence;
	uint32_t ae_response;       /* [1:0] 0=under,1=proper,2=over */
	uint32_t ae_skewness;       /* [15:0] */
	uint32_t ae_done;
	uint32_t reserved[8];
};

struct xil_isp_lite_stat_awb_result {
	uint64_t timestamp_ns;
	uint32_t frame_sequence;
	uint32_t final_r_gain;      /* [11:0] */
	uint32_t final_b_gain;      /* [11:0] */
	uint32_t reserved[8];
};

struct xil_isp_lite_stat_result {
	struct xil_isp_lite_stat_ae_result  ae;
	struct xil_isp_lite_stat_awb_result awb;
};

/* --- custom control IDs (must match kernel driver) --- */
#define XIL_ISP_CID_BASE    (V4L2_CID_USER_BASE + 0x10e0)
#define CID_ISP_ALL         (XIL_ISP_CID_BASE + 0)
#define CID_ISP_CONFIG      (XIL_ISP_CID_BASE + 1)
#define CID_ISP_DPC         (XIL_ISP_CID_BASE + 2)
#define CID_ISP_BLC         (XIL_ISP_CID_BASE + 3)
#define CID_ISP_AE          (XIL_ISP_CID_BASE + 4)
#define CID_ISP_DGAIN       (XIL_ISP_CID_BASE + 5)
#define CID_ISP_LSC         (XIL_ISP_CID_BASE + 6)
#define CID_ISP_AWB         (XIL_ISP_CID_BASE + 7)
#define CID_ISP_WB          (XIL_ISP_CID_BASE + 8)
#define CID_ISP_CFA         (XIL_ISP_CID_BASE + 9)
#define CID_ISP_CCM         (XIL_ISP_CID_BASE + 10)
#define CID_ISP_CSC         (XIL_ISP_CID_BASE + 11)
#define CID_ISP_LDCI        (XIL_ISP_CID_BASE + 12)
#define CID_ISP_SHARP       (XIL_ISP_CID_BASE + 13)
#define CID_ISP_BNR         (XIL_ISP_CID_BASE + 14)
#define CID_ISP_2DNR        (XIL_ISP_CID_BASE + 15)

/* perror_ln() and xioctl() are provided by V4L2Stream.hpp */

/* Find the ISP subdev device by scanning /dev/v4l-subdev* */
static std::string findIspSubdev() {
    for (int i = 0; i < 8; i++) {
        std::string path = "/dev/v4l-subdev" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) continue;

        /* Read name from sysfs */
        std::string sysfs = "/sys/class/video4linux/v4l-subdev" +
                            std::to_string(i) + "/name";
        FILE *f = fopen(sysfs.c_str(), "r");
        if (f) {
            char buf[256] = {0};
            fgets(buf, sizeof(buf), f);
            fclose(f);
            buf[strcspn(buf, "\n")] = 0;
            std::string name(buf);
            if (name.find("infinite_isp") != std::string::npos ||
                name.find("xil-isp") != std::string::npos ||
                name.find("a0060000") != std::string::npos) {
                close(fd);
                return path;
            }
        }
        close(fd);
    }
    return "";
}

/* Find the ISP stat metadata video node */
static std::string findIspStatDev() {
    for (int i = 0; i < 16; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        std::string sysfs = "/sys/class/video4linux/video" +
                            std::to_string(i) + "/name";
        FILE *f = fopen(sysfs.c_str(), "r");
        if (!f) continue;

        char buf[256] = {0};
        fgets(buf, sizeof(buf), f);
        fclose(f);
        buf[strcspn(buf, "\n")] = 0;
        std::string name(buf);
        if (name.find("xil-isp-lite_stat") != std::string::npos) {
            return path;
        }
    }
    return "";
}

/* --- Register map sub-structures for control I/O (must match kernel) --- */

#pragma pack(push, 1)

struct isp_reg_wb {
    uint32_t WB_RGAIN;
    uint32_t WB_BGAIN;
    uint32_t reserved[126];
};

struct isp_reg_dgain {
    uint32_t dgain_isManual;
    uint32_t dgain_man_index;
    uint32_t dgain_index_out;
    uint32_t reserved1[13];
    uint32_t dgain_array[100];
    uint32_t reserved2[12];
};

struct isp_reg_awb_cfg {
    uint32_t AWB_UNDEREXPOSED_LIMIT;
    uint32_t AWB_OVEREXPOSED_LIMIT;
    uint32_t AWB_FRAMES;
    uint32_t FINAL_RGAIN;
    uint32_t FINAL_BGAIN;
    uint32_t reserved[123];
};

struct isp_reg_ae_cfg {
    uint32_t center_illuminance;
    uint32_t skewness;
    uint32_t ae_crop_left;
    uint32_t ae_crop_right;
    uint32_t ae_crop_top;
    uint32_t ae_crop_bottom;
    uint32_t ae_response;
    uint32_t ae_result_skewness;
    uint32_t ae_response_debug;
    uint32_t ae_done;
    uint32_t reserved[118];
};

#pragma pack(pop)

/* --- ISP Control Helper --- */

class IspControl {
public:
    IspControl(const std::string &dev) : fd(-1) {
        fd = open(dev.c_str(), O_RDWR);
        if (fd < 0) perror_ln(("open isp ctrl " + dev).c_str());
    }
    ~IspControl() { if (fd >= 0) close(fd); }
    bool ok() const { return fd >= 0; }

    /* Read a custom control block */
    bool readCtrl(uint32_t cid, void *data, uint32_t size) {
        struct v4l2_ext_controls ctrls {};
        struct v4l2_ext_control ctrl {};

        ctrl.id    = cid;
        ctrl.size  = size;
        ctrl.p_u8  = (uint8_t *)data;   /* use pointer to raw data */

        ctrls.count     = 1;
        ctrls.controls  = &ctrl;
        ctrls.which     = V4L2_CTRL_WHICH_CUR_VAL;

        if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) < 0) {
            fprintf(stderr, "VIDIOC_G_EXT_CTRLS(cid=0x%x) failed: %s\n",
                    cid, strerror(errno));
            return false;
        }
        return true;
    }

    /* Write a custom control block */
    bool writeCtrl(uint32_t cid, const void *data, uint32_t size) {
        struct v4l2_ext_controls ctrls {};
        struct v4l2_ext_control ctrl {};

        ctrl.id    = cid;
        ctrl.size  = size;
        ctrl.p_u8  = (uint8_t *)data;   /* driver reads from here */

        ctrls.count     = 1;
        ctrls.controls  = &ctrl;
        ctrls.which     = V4L2_CTRL_WHICH_CUR_VAL;

        if (xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
            fprintf(stderr, "VIDIOC_S_EXT_CTRLS(cid=0x%x) failed: %s\n",
                    cid, strerror(errno));
            return false;
        }
        return true;
    }

    /* Read WB gains */
    bool getWbGains(uint32_t &rGain, uint32_t &bGain) {
        isp_reg_wb wb {};
        if (!readCtrl(CID_ISP_WB, &wb, sizeof(wb)))
            return false;
        rGain = wb.WB_RGAIN;
        bGain = wb.WB_BGAIN;
        return true;
    }

    /* Write WB gains */
    bool setWbGains(uint32_t rGain, uint32_t bGain) {
        isp_reg_wb wb {};
        wb.WB_RGAIN = rGain;
        wb.WB_BGAIN = bGain;
        return writeCtrl(CID_ISP_WB, &wb, sizeof(wb));
    }

    /* Read DGain index */
    bool getDGainIndex(uint32_t &index) {
        isp_reg_dgain dg {};
        if (!readCtrl(CID_ISP_DGAIN, &dg, sizeof(dg)))
            return false;
        index = dg.dgain_man_index;
        return true;
    }

    /* Set DGain index (manual mode) */
    bool setDGainIndex(uint32_t index) {
        isp_reg_dgain dg {};
        /* Read current first to preserve array */
        if (!readCtrl(CID_ISP_DGAIN, &dg, sizeof(dg)))
            return false;
        dg.dgain_isManual = 1;
        dg.dgain_man_index = index;
        return writeCtrl(CID_ISP_DGAIN, &dg, sizeof(dg));
    }

    /* Read AE config (for ae_response, skewness) */
    bool getAeStatus(uint32_t &aeResponse, uint32_t &aeSkewness, uint32_t &aeDone) {
        isp_reg_ae_cfg ae {};
        if (!readCtrl(CID_ISP_AE, &ae, sizeof(ae)))
            return false;
        aeResponse = ae.ae_response;
        aeSkewness = ae.ae_result_skewness;
        aeDone     = ae.ae_done;
        return true;
    }

    /* Read AWB result gains (computed by HW AWB) */
    bool getAwbResult(uint32_t &rGain, uint32_t &bGain) {
        isp_reg_awb_cfg awb {};
        if (!readCtrl(CID_ISP_AWB, &awb, sizeof(awb)))
            return false;
        rGain = awb.FINAL_RGAIN;
        bGain = awb.FINAL_BGAIN;
        return true;
    }

    /* Apply AWB results to WB gains (the tuning loop) */
    bool applyAwbToWb() {
        uint32_t rGain, bGain;
        if (!getAwbResult(rGain, bGain))
            return false;

        /* Guard: AWB may output 0 before first computation */
        if (rGain == 0 && bGain == 0)
            return true;

        /* Clamp to reasonable range (0.25x to 4.0x in U8.4) */
        if (rGain < 64)  rGain = 64;
        if (rGain > 1024) rGain = 1024;
        if (bGain < 64)  bGain = 64;
        if (bGain > 1024) bGain = 1024;

        return setWbGains(rGain, bGain);
    }

    /* AE tuning: adjust digital gain based on ae_response */
    bool adjustExposure() {
        uint32_t aeResp, aeSkew, aeDone;
        if (!getAeStatus(aeResp, aeSkew, aeDone))
            return false;

        if (!aeDone)
            return true;   // AE hasn't finished this frame yet

        uint32_t curIdx;
        if (!getDGainIndex(curIdx))
            return false;

        int newIdx = (int)curIdx;
        /*
         * ae_response:
         *   0 = underexposed → increase gain
         *   1 = properly exposed → hold
         *   2 = overexposed  → decrease gain
         */
        if (aeResp == 0) {
            newIdx = std::min(newIdx + 2, 99);
        } else if (aeResp == 2) {
            newIdx = std::max(newIdx - 2, 0);
        } // else aeResp==1: hold

        if (newIdx != (int)curIdx) {
            return setDGainIndex((uint32_t)newIdx);
        }
        return true;
    }

private:
    int fd;
};

/* --- Stat Metadata Reader --- */

class IspStatReader {
public:
    IspStatReader(const std::string &dev) : devpath(dev), fd(-1) {}
    ~IspStatReader() { closeDevice(); }

    bool openDevice() {
        fd = ::open(devpath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            perror_ln(("open stat " + devpath).c_str());
            return false;
        }

        /* Set format: V4L2_META_FMT_XIL_ISP_LITE_STAT */
        struct v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_META_CAPTURE;
        if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
            perror_ln("VIDIOC_G_FMT meta");
            return false;
        }
        fmt.fmt.meta.dataformat = v4l2_fourcc('X','I','S','P');
        fmt.fmt.meta.buffersize = sizeof(xil_isp_lite_stat_result);
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror_ln("VIDIOC_S_FMT meta");
            return false;
        }
        return true;
    }

    void closeDevice() {
        stopStreaming();
        for (auto &b : buffers) {
            if (b.start)
                munmap(b.start, b.length);
        }
        if (fd >= 0) close(fd);
        fd = -1;
    }

    bool initMMap(int bufCount = 4) {
        struct v4l2_requestbuffers req {};
        req.count  = bufCount;
        req.type   = V4L2_BUF_TYPE_META_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror_ln("VIDIOC_REQBUFS meta");
            return false;
        }
        buffers.resize(req.count);

        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf {};
            buf.type   = V4L2_BUF_TYPE_META_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                perror_ln("VIDIOC_QUERYBUF meta");
                return false;
            }
            buffers[i].length = buf.length;
            buffers[i].start = mmap(nullptr, buf.length,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, buf.m.offset);
            if (buffers[i].start == MAP_FAILED) {
                perror_ln("mmap meta");
                return false;
            }
        }
        return true;
    }

    bool queueAll() {
        for (uint32_t i = 0; i < buffers.size(); ++i) {
            struct v4l2_buffer buf {};
            buf.type   = V4L2_BUF_TYPE_META_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                perror_ln("VIDIOC_QBUF meta");
                return false;
            }
        }
        return true;
    }

    bool startStreaming() {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_META_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
            perror_ln("VIDIOC_STREAMON meta");
            return false;
        }
        return true;
    }

    void stopStreaming() {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_META_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
    }

    /*
     * Dequeue one stat buffer. Returns pointer to stat result,
     * or nullptr on timeout / error.
     */
    const xil_isp_lite_stat_result *dequeue(int timeoutMs = 500) {
        struct pollfd pfd { fd, POLLIN, 0 };
        if (poll(&pfd, 1, timeoutMs) <= 0)
            return nullptr;

        struct v4l2_buffer buf {};
        buf.type   = V4L2_BUF_TYPE_META_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror_ln("VIDIOC_DQBUF meta");
            return nullptr;
        }

        lastIdx = buf.index;
        auto *result = (const xil_isp_lite_stat_result *)buffers[lastIdx].start;
        lastSeq = buf.sequence;
        return result;
    }

    /* Re-queue buffer after dequeue */
    bool requeue() {
        struct v4l2_buffer buf {};
        buf.type   = V4L2_BUF_TYPE_META_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = lastIdx;
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror_ln("VIDIOC_QBUF meta requeue");
            return false;
        }
        return true;
    }

    uint32_t getLastSeq() const { return lastSeq; }

private:
    struct Buf { void *start = nullptr; size_t length = 0; };
    std::string devpath;
    int fd;
    std::vector<Buf> buffers;
    uint32_t lastIdx = 0;
    uint32_t lastSeq = 0;
};
