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
`digital_gain`. The current RTL reports AE decisions as 0 normal, 1
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

## ISP Pipeline Test Application (OpenCV)
A C++ test application based on OpenCV has been implemented. This application supports continuous frame acquisition from the ISP output and validates the complete user-space data path.

### Build Dependencies
```sh
sudo apt update
sudo apt install libopencv-dev
```

### Build and Run
```sh
cd Linux_drivers/test
mkdir build
cd build
sudo cmake ..
sudo cmake --build . -j8
./isp_pipeline
```

The test application builds and links `libisp_tuning`, starts the XISP metadata
stream, and runs tuning from frame statistics rather than polling private
register payloads. Select a policy with `ISP_TUNING_MODE`:

```bash
# Default: let RTL AE/AWB update the effective gains and observe statistics.
ISP_TUNING_MODE=hardware ./isp_pipeline

# User-space DGAIN loop driven by the RTL AE decision; hardware AWB remains on.
ISP_TUNING_MODE=software-ae ./isp_pipeline

# Image pipeline only.
ISP_TUNING_MODE=off ./isp_pipeline
```

`software-ae` disables hardware automatic gain while running and restores it
on exit. `libisp_tuning/include/infinite_isp/tuning.hpp` contains the portable,
V4L2-independent policy intended for later reuse by a libcamera IPA/backend;
`v4l2_backend.hpp` is the Linux transport and control adapter. Run the policy
unit test from the build directory with `ctest --output-on-failure`.

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
