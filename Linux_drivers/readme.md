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
cmake .. && make
./isp_pipeline
```

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
