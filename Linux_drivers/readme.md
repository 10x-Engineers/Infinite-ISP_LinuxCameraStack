# Build and Deployment Guide

## Environment Setup

To build this project, you need to prepare the development environment for your target platform. This guide covers both native compilation on **KV260** boards and **cross-compilation** on x86-based PCs, all using **Ubuntu 22.04**.
For Windows users, **WSL2** is also supported.

------

### Native Compilation on KV260

Install the required build tools directly on the KV260 board:

```
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

------

###  Cross-Compilation on x86 (for KV260 target)

#### 1. Install Dependencies

```
sudo apt update
sudo apt install gcc-11-aarch64-linux-gnu g++-11-aarch64-linux-gnu \
    build-essential device-tree-compiler
```

Configure `gcc` alternatives (optional but recommended):

```
sudo update-alternatives --install /usr/bin/aarch64-linux-gnu-gcc aarch64-linux-gnu-gcc /usr/bin/aarch64-linux-gnu-gcc-11 110
sudo update-alternatives --install /usr/bin/aarch64-linux-gnu-g++ aarch64-linux-gnu-g++ /usr/bin/aarch64-linux-gnu-g++-11 110
```

#### 2. Extract Linux Headers

Since x86 hosts cannot install ARM `.deb` packages directly, you need to extract them manually:

```
dpkg-deb -x ./debs/linux-headers-5.15.0-1046-xilinx-zynqmp_5.15.0-1046.50_arm64.deb linux_headers
dpkg-deb -x ./debs/linux-xilinx-zynqmp-headers-5.15.0-1046_5.15.0-1046.50_all.deb linux_headers
```

> ⚠️ These `.deb` packages are specific to kernel version `5.15.0-1046-xilinx-zynqmp`.
>  To confirm your board’s kernel version, run `uname -r` on the KV260.

If you're using a different kernel version, refer to [How to Obtain Kernel Headers](#How to Obtain Kernel Headers) below.

------



## Building and Installing Kernel Modules & Device Tree Overlays

To compile and install kernel modules (`.ko`) and device tree overlays (`.dtbo`):

```
sudo make -j8
sudo make -j8 install
```

Both the native driver build and installation are expected to run as root on
the KV260 target.

> ⚠️ On KV260, running `make install` as root on the **aarch64 target** will **remove all overlays** under `/sys/kernel/config/device-tree/overlays/`, effectively **unloading any preloaded bitstream** (typically loaded via DTBO). 
>
> Please ensure this behavior is acceptable in your workflow before proceeding.

------

## Reloading Kernel Modules Without Rebooting

For driver-only changes, keep the FPGA image and device-tree overlay loaded and
replace the kernel modules with:

```bash
cd Linux_drivers
sudo make reload
```

Stop the test application and any process using `/dev/media*`, `/dev/video*`,
or `/dev/v4l-subdev*` before running the command. The target checks for open
device nodes and refuses to continue when the media pipeline is busy. Do not
use forced module removal.

The Xilinx 5.15 `xilinx-video` composite driver does not clear its saved
subdevice pointers when an individual subdevice driver is unregistered. A
direct `rmmod`/`insmod` cycle can therefore fail with `duplicate subdev` and
leave the media graph incomplete. The `reload` target avoids this by:

1. Building the kernel modules as root.
2. Unbinding the two `xilinx-video` composite devices to destroy their V4L2
   async graphs.
3. Removing and reinserting the IMX219, AR1335, MIPI RX, VIP, and ISP modules.
4. Binding the ISP graph first and the IMX219 capture graph second. This keeps
   the ISP graph on `/dev/media0` and the capture graph on `/dev/media1`.
5. Waiting for udev to recreate the media, video, and subdevice nodes.

The media-bus formats are reset during reload. Reapply the `media-ctl` commands
in the capture section below before restarting the test application. Verify the
two media graphs and check `dmesg` after each cycle. If removal reports an oops,
a hung task, or an incomplete graph, stop reloading and reboot the board to
restore the default state.

### Shared ISP/VIP interrupt dispatcher

The FPGA exposes one level-sensitive parent interrupt containing status from
the ISP, VIP1, and VIP2 blocks. `xil-isp-irq.ko` owns that interrupt and
dispatches it to the three child register-bank handlers. Each child reads and
acknowledges only its local status register; the parent handler polls every
registered bank so that all contributors to the shared level are cleared.

The device tree therefore assigns the physical interrupt to one dispatcher
node, instead of assigning the same interrupt to each processing block:

```dts
isp_irq_dispatcher: isp_irq_dispatcher {
    compatible = "xlnx,infinite-isp-irq-dispatcher";
    interrupt-names = "parent";
    interrupt-parent = <&axi_intc>;
    interrupts = <3 2>;
};
```

VIP instances select their dispatcher slot with `xlnx,irq-source = <1>` for
VIP1 and `<2>` for VIP2. The ISP uses slot 0. For an older, already-loaded
overlay that still places `interrupts` on the ISP node, the ISP driver attaches
that IRQ to the same dispatcher API. This compatibility path permits driver
reload testing without replacing the overlay or rebooting.

The dispatcher source is enabled when the statistics stream starts. ISP frame
start/frame done events are acknowledged by the ISP child; metadata is
completed at frame done. VIP interrupts remain masked until its subdevice
stream starts. A healthy run should show the dispatcher as the only IRQ owner
and no unhandled interrupt count:

```bash
grep xil-isp-irq /proc/interrupts
cat /proc/irq/67/spurious
```

### ISP statistics and standard controls

The `xil-isp-lite_stat` node is a V4L2 metadata-capture device using fourcc
`XISP`. It returns one fixed-size, versioned record per ISP frame done. ABI
version 1 is 96 bytes and contains the sequence/timestamp, accumulated IRQ
status, AE response and skewness, effective AWB gains, DGAIN index, manual WB
gains, validity flags, and the number of dropped metadata records. The record
layout is declared in `linux/xil-isp-lite.h`; consumers must check both
`abi_version` and `record_size`.

The statistics node and ISP subdevice expose standard V4L2 controls for
`white_balance_automatic`, `gain_automatic`, `red_balance`, `blue_balance`, and
`digital_gain`. The statistics driver exports the latched RTL AE decision (the
raw `ae_response` signal is only a one-pixel-clock pulse) as 0 normal, 1
overexposed, 2 hold, and 3 underexposed. Hardware AWB gains already feed the
effective WB path, so applications must not copy those gains back into the
manual WB controls while automatic white balance is enabled.

------



## Capturing Images From IMX219

After successfully running `sudo make install`, you can capture an image from the camera using the following steps:

### 1. Capture RAW10 Image
Execute the following command to capture a RAW10 format image:
```bash
media-ctl -d /dev/media1 --set-v4l2 '"xlnx-imx219 6-0010":0[fmt:SRGGB10_1X10/1992x1152 field:none]'
media-ctl -d /dev/media1 --set-v4l2 '"a0030000.mipi_rx_to_video":0[fmt:SRGGB10_1X10/1992x1152 field:none]'
media-ctl -d /dev/media1 --set-v4l2 '"a0030000.mipi_rx_to_video":1[fmt:SRGGB10_1X10/1992x1152 field:none]'
media-ctl -d /dev/media1 --set-v4l2 '"axi:camif_rpi_axis_subsetconv":0[fmt:SRGGB10_1X10/1992x1152 field:none]'
media-ctl -d /dev/media1 --set-v4l2 '"axi:camif_rpi_axis_subsetconv":1[fmt:Y10_1X10/1992x1152 field:none]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1992,height=1152,pixelformat=XY10,bytesperline=2656 --stream-mmap=3 --stream-skip=30 --stream-count=1 --stream-poll --stream-to=camera.raw10
```
This will generate a raw image file (`camera.raw10` by default) in XY10 format.

### 2. Convert and View Image
Use the provided `parse_raw10.py` script to convert the RAW10 file to viewable PNG format:
```bash
python3 parse_raw10.py
```
This will:
1. Process the RAW10 image and display both RAW and demosaiced RGB versions
2. Save two output files:
   - `raw8.png`: Processed 8-bit grayscale image
   - `rgb.png`: Demosaiced color image

> **Note:** The script requires:
> - Python 3
> - OpenCV (`opencv-python`)
> - NumPy
> - Matplotlib
>
> Install dependencies with:
> ```
> pip3 install numpy opencv-python matplotlib
> ```
> 
>**Important:** Before running:
> 1. Modify `width` and `height` variables in the script to match your camera resolution
> 2. Adjust `raw_file` variable if using a different input filename

---

## ISP Pipeline Test Application
A C++ test application has been implemented. It supports continuous frame
acquisition from the ISP output, metadata-driven tuning, headless image capture,
and either a low-overhead GStreamer preview or an OpenCV compatibility preview.

### Quick Demo on KV260

With the drivers and overlay already installed, the shortest path to the live
demo is:

```sh
cd Linux_drivers
make setup-media
sudo cmake -S test -B test/build
sudo cmake --build test/build -j8
DISPLAY=:0 ./test/build/isp_pipeline
```

Run the build as root because the shared build tree can contain root-owned
artifacts; run `isp_pipeline` as the desktop `ubuntu` user. The default demo
opens a resizable 960x540 Mali-400 zero-copy preview at 30 FPS, enables
latest-frame low-latency display, and uses sensor analogue gain for AE with
hardware AWB. Press `Ctrl+C` or close the window to stop it.

If this is a fresh board rather than an already installed system, first install
the dependencies below and follow the driver/overlay installation section
above. After every `sudo make reload`, rerun `make setup-media` because media
formats are reset when the graph is recreated.

### Common Scenarios

Run these commands from `Linux_drivers` after `make setup-media`:

```bash
# Stable image/color baseline without automatic tuning.
DISPLAY=:0 ISP_TUNING_MODE=off ./test/build/isp_pipeline

# Headless capture after 150 ISP output frames.
ISP_HEADLESS=1 ISP_CAPTURE_FRAME=150 \
ISP_CAPTURE_PREFIX=/tmp/isp-frame ./test/build/isp_pipeline

# Record image/tuning statistics for about six seconds, using a central ROI.
ISP_HEADLESS=1 ISP_CAPTURE_FRAME=300 \
ISP_MEASURE_CSV=/tmp/isp-tuning.csv \
ISP_MEASURE_ROI=240,135,1440,810 \
ISP_CAPTURE_PREFIX=/tmp/isp-measure ./test/build/isp_pipeline

# Compare the compatibility display backends.
DISPLAY=:0 ISP_DISPLAY_BACKEND=gstreamer ./test/build/isp_pipeline
DISPLAY=:0 ISP_DISPLAY_BACKEND=opencv ./test/build/isp_pipeline
```

The headless commands create PNG/RAW files with the selected prefix. The CSV
scenario records BGR/luma measurements together with AE, AWB-related metadata,
DGAIN, and sensor analogue gain. Mali display, tuning overrides, low-rate
preview, framebuffer capture, and CCM examples are documented in the detailed
sections below.

### Build Dependencies

```sh
sudo apt update
sudo apt install libopencv-dev libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev gstreamer1.0-x \
    libegl1-mesa-dev libgles2-mesa-dev libx11-dev libdrm-dev
```

### Tuning Modes and Controls

The test application builds and links `libisp_tuning`, starts the XISP metadata
stream, and runs tuning from frame statistics rather than polling private
register payloads. Select a policy with `ISP_TUNING_MODE`:

```bash
# Default: hold ISP DGAIN at 1x and adjust IMX219 analogue gain from ISP AE
# statistics. Hardware AWB remains enabled.
ISP_TUNING_MODE=sensor-ae ./test/build/isp_pipeline

# Legacy RTL loop. Its integer DGAIN table can visibly alternate between two
# adjacent gain entries near the target brightness.
ISP_TUNING_MODE=hardware ./test/build/isp_pipeline

# User-space DGAIN loop driven by the RTL AE decision; hardware AWB remains on.
ISP_TUNING_MODE=software-ae ./test/build/isp_pipeline

# Image pipeline only.
ISP_TUNING_MODE=off ./test/build/isp_pipeline
```

`sensor-ae` and `software-ae` disable hardware automatic ISP gain while running
and restore it on exit. Sensor AE starts at IMX219 analogue-gain code 227,
fixes exposure at 1587 lines (approximately 30.00 ms, aligned to 50 Hz
lighting), and requires ten consistent ISP decisions before making a small
adjustment. These values are tunable without rebuilding:

```bash
ISP_SENSOR_AGAIN=227 ISP_SENSOR_EXPOSURE=1587 \
ISP_SENSOR_AE_FRAMES=10 ./test/build/isp_pipeline
```

The sensor gain value is the IMX219 V4L2 control code (range 0 to 232), not a
linear multiplier. The ISP uses the central 80% of the 1992x1152 input for AE
statistics so bright objects at the frame edge do not dominate metering.

### IMX219 frame rate

The 1992x1152 cropped mode now uses VTS 1763 instead of 3526. With the
182.4 MHz pixel rate and 3448-pixel line length this produces 30 FPS; the V4L2
vertical-blanking control reads 611 lines. The default 1587-line exposure still
fits below the 1759-line limit and remains approximately 30 ms for 50 Hz
anti-flicker operation.

After rebuilding and reloading the sensor module, the capture node can be
checked independently of the ISP:

```bash
v4l2-ctl -d /dev/video4 --stream-mmap=4 --stream-count=300 \
    --stream-to=/dev/null --stream-poll
```

The independent capture test measured 30.01 FPS. The application keeps four
DMABUFs in flight rather than waiting synchronously for each buffer. A board
run with this four-buffer setting held `sensor` and `ispin` at 30 FPS, ISP
output and tuning at 48-49 FPS, display at 30 FPS, and zero metadata drops.

### Live display and CPU load

When EGL/GLES2/X11 development files are available, the default live preview is
the custom `mali` backend at 960x540@30 FPS. The output path is genuinely
zero-copy: the VIP owns four MMAP buffers, exports each as a DMA-BUF, and Mali
samples those same buffers through EGLImages. `EGL_KHR_fence_sync` prevents a
buffer from being queued back to VIP until its GLES draw has completed.

The ARM r9p0 library cannot import `DRM_FORMAT_RGB888` or
`DRM_FORMAT_BGR888`. The FPGA framebuffer writer also cannot be switched to a
32-bit format in software: its generated hardware has `HAS_BGRX8=0` and
`HAS_RGBX8=0`, so adding `xrgb8888` only to `xlnx,vid-formats` is invalid and
produces `Framebuffer not configured for fourcc 0x34325258` in `dmesg`.

The working path keeps the native tightly packed BGR24 stream. Each line is
viewed as three 960x1080 RGB565 EGLImages with offsets 0, 1920, and 3840 bytes
and a common 5760-byte pitch. In the fragment shader, three RGB565 words are
losslessly unpacked back into every two BGR24 pixels before scaling. Splitting
the view three ways keeps texture indices within the Mali-400 fragment
shader's mediump precision range; one 2880-pixel packed view causes vertical
color stripes.

The Mali path continuously drains the roughly 49 FPS ISP output and displays
only the newest completed buffer at each display deadline. Older or duplicate
buffers are immediately returned to V4L2 instead of being replayed through a
slow FIFO. The FPS monitor reports the output-completion-to-swap age as
`display-age` and counts discarded old buffers as `stale-skipped`. On the board,
the former 15 FPS FIFO averaged about 302 ms of display age; at the same 15 FPS,
latest-frame mode averaged 0.8 ms with a 1.1 ms measured maximum. At the default
30 FPS it averaged 3-8 ms with a maximum below 24 ms, while sensor/ispin
remained 30 FPS, tuning remained 48-49 FPS, and metadata drops remained zero.
The complete process used about 3.4% instantaneous CPU.
A GPU readback of the final 960x540 shader output verified image orientation
and RGB channel order. The earlier
GStreamer/NEON backend used about 56% of one core and displayed 12-13 FPS; the
original full-resolution OpenCV path used about 207%.

Desktop GLX still reports Mesa `llvmpipe`; Mali acceleration is available only
through ARM EGL 1.4/OpenGL ES 2.0. The viewer therefore selects an EGL config
matching the desktop's 16-bit root visual. Do not substitute `glimagesink`: its
default GLX path is software-rendered, and its forced EGL path selects an
incompatible X11 visual.

Display settings and the compatibility backend are selectable without a
rebuild:

```bash
# Default Mali zero-copy, latest-frame preview, 960x540@30.
DISPLAY=:0 ./test/build/isp_pipeline

# Reduce the preview rate if desired; low-latency mode still drops stale frames.
DISPLAY=:0 ISP_DISPLAY_FPS=15 ./test/build/isp_pipeline

# Restore FIFO playback only for A/B diagnosis. This adds about 300 ms of
# measured display-queue latency at 15 FPS and should not be used normally.
DISPLAY=:0 ISP_DISPLAY_FPS=15 ISP_MALI_LOW_LATENCY=0 \
./test/build/isp_pipeline

# Change the initial GPU output window size. Resizing or maximizing the X11
# window updates the GLES viewport dynamically and fills the new client area.
DISPLAY=:0 ISP_DISPLAY_WIDTH=960 ./test/build/isp_pipeline

# X11 vsync is enabled by default to prevent horizontal tearing during motion.
# Disable it only for driver/performance diagnosis.
DISPLAY=:0 ISP_MALI_VSYNC=0 ./test/build/isp_pipeline

# One-shot debug capture of the final GLES framebuffer.
DISPLAY=:0 ISP_MALI_CAPTURE_PPM=/tmp/mali-preview.ppm \
./test/build/isp_pipeline

# Retain compatibility backends for comparison and CPU image measurement.
DISPLAY=:0 ISP_DISPLAY_BACKEND=gstreamer ./test/build/isp_pipeline
DISPLAY=:0 ISP_DISPLAY_BACKEND=opencv ISP_OPENCV_THREADS=4 \
./test/build/isp_pipeline
```

If Mali build dependencies are missing, CMake omits this backend and the program
falls back to GStreamer or OpenCV. OpenCV HighGUI is already built with GTK3,
so direct GTK does not remove the BGR24 scaling and 16-bit X11 conversion.
`ISP_MEASURE_CSV` requires `gstreamer`, `opencv`, or headless mode because the
Mali loop deliberately performs no CPU image reads. Headless capture is
unchanged.

### ColorChecker CCM tuning

The default CCM is a signed Q10 matrix fitted from the 24-patch Aurora chart
under the current board test lighting. Hardware AWB remains enabled so it can
adapt the WB stage to the illuminant; CCM is responsible for sensor color
cross-talk and saturation:

```text
 2804  -1357   -424
 -648   2163   -490
 -320  -1414   2747
```

The matrix rows remain approximately unity-sum, which keeps neutral input
neutral and prevents CCM from changing the AE target. The initial regularized
fit reduced the mean luminance-matched Delta-E 76 of the 18 chromatic patches
from 16.57 to 11.08 in its same-pose comparison. An unconstrained fit reached
9.95 but clipped the yellow patch's blue channel to zero, so it was rejected.

After the chart was moved and relit, a second same-pose A/B pass found excess
green and blue in the saturated red patch. The final matrix above changes only
the green and blue rows by opposite off-diagonal/diagonal amounts, preserving
their row sums. The red patch changed from RGB 169/58/67 to 171/53/61 and its
luminance-matched Delta-E 76 fell from 5.64 to 1.49. Its G/R ratio changed from
0.343 to 0.310 (reference 0.309), while the 18-patch mean improved from 8.53 to
8.15. The comparison used the classic ColorChecker sRGB patch values; an
Aurora clone and non-uniform room lighting are not a spectrophotometric
reference, so this matrix is a board-specific starting point rather than a
universal profile.

Candidate matrices can be tested without rebuilding or reloading the driver.
Pass all nine signed Q10 coefficients in row-major order:

```bash
ISP_CCM='2804,-1357,-424,-648,2163,-490,-320,-1414,2747' \
ISP_HEADLESS=1 ISP_CAPTURE_FRAME=150 \
ISP_CAPTURE_PREFIX=/tmp/chart ./test/build/isp_pipeline
```

For controlled WB experiments, disable hardware AWB by supplying both manual
gains. Supplying only one is rejected:

```bash
ISP_WB_R_GAIN=425 ISP_WB_B_GAIN=355 ./test/build/isp_pipeline
```

The manual-WB trial did not improve this chart under the current non-uniform
lighting, so normal operation should omit both variables. V4L2 controls retain
their last value until another application changes them or the module is
reloaded. Use flat, diffuse illumination, avoid glare, fill most of the frame,
and let sensor AE settle before comparing CCM candidates.

For headless tuning, the application can save the final frame and a lightweight
per-output-frame CSV. The ROI coordinates use the 1920x1080 ISP output:

```bash
ISP_HEADLESS=1 ISP_CAPTURE_FRAME=300 \
ISP_CAPTURE_PREFIX=/tmp/chart \
ISP_MEASURE_CSV=/tmp/chart.csv \
ISP_MEASURE_ROI=170,80,1450,850 ./test/build/isp_pipeline
```

Without `ISP_HEADLESS`, the application displays the live image using the
GStreamer backend described above. `libisp_tuning/include/infinite_isp/tuning.hpp` contains the portable,
V4L2-independent policies intended for later reuse by a libcamera IPA/backend;
`v4l2_backend.hpp` is the Linux transport and control adapter. Run the policy
unit test from the build directory with `sudo ctest --output-on-failure` when
the build directory was created as root.

## *How to Obtain Kernel Headers*

There are two main ways to get the kernel headers:

### Method 1: Download on KV260

```
apt download linux-headers-$(uname -r)
apt download linux-xilinx-zynqmp-headers-$(uname -r | sed 's/-xilinx-zynqmp//')
```

### Method 2: Build from Source on PC

You can also build the kernel headers manually by compiling the Xilinx kernel source. Follow these steps:

#### Step 1: Install Required Packages

```
sudo apt install gcc-11-aarch64-linux-gnu g++-11-aarch64-linux-gnu \
    build-essential libncurses-dev flex bison libssl-dev bc \
    kmod cpio pahole dwarfdump device-tree-compiler
```

#### Step 2: Clone the Xilinx Kernel Source

```
git clone https://git.launchpad.net/~canonical-kernel/ubuntu/+source/linux-xilinx-zynqmp/+git/jammy
cd jammy
git tag
git checkout <matching-tag>  # Example: Ubuntu-xilinx-zynqmp-5.15.0-1046.50
```

> Use the tag that matches your board's kernel version (`uname -r` on KV260).

#### Step 3: Build the Kernel

```
export ARCH=arm64
export $(dpkg-architecture -aarm64)
export CROSS_COMPILE=aarch64-linux-gnu-

fakeroot debian/rules clean
do_tools=false fakeroot debian/rules binary
```
