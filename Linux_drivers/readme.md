# Infinite-ISP Linux Drivers and Camera Demo

This directory contains the Linux drivers and the `isp_pipeline` camera demo
for the KV260 and IMX219.

The recommended workflow is to build directly on the KV260. Once the project
is installed, the demo configures the complete camera and ISP pipeline when it
starts. No separate media-graph setup command is required.

## Quick Start

The following steps start from the `Linux_drivers` directory on the KV260.
Driver and application builds use root permission; run the final camera demo as
the normal desktop user.

### Step 1: Install Build Dependencies

This is required only once:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) \
    device-tree-compiler cmake pkg-config \
    libopencv-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-x \
    libegl1-mesa-dev libgles2-mesa-dev libx11-dev libdrm-dev
```

### Step 2: Build and Install the Drivers

From the `Linux_drivers` directory:

```bash
# Build the kernel modules and device-tree overlay.
sudo make -j8

# Install the FPGA image, modules, and device-tree overlay.
sudo make -j8 install
```

Use `make install` for the first deployment or after changing the FPGA image or
device tree. It replaces the active overlay, so stop any camera application
before running it.

If the correct drivers and overlay are already installed, you can skip this
step and build only the demo application.

### Step 3: Build the Camera Demo

These commands compile `test/build/isp_pipeline`; they do not start the camera
and do not run unit tests:

```bash
sudo cmake -S test -B test/build
sudo cmake --build test/build -j8
```

The first command creates the build directory. For later application changes,
only the second command is normally needed.

### Step 4: Start the Preview

Run the demo as the normal desktop user, without `sudo`:

```bash
DISPLAY=:0 ./test/build/isp_pipeline
```

This starts the default wide-FOV mode and opens a resizable, low-latency Mali
preview. Close the window or press `Ctrl+C` in the terminal to stop it.

The application discovers the V4L2 nodes and configures the Sensor, MIPI, ISP,
and VIP formats on every start. It therefore needs no extra graph-setup step.
The FPGA image, overlay, and drivers must already be loaded by Step 2 or by the
board's startup configuration.

## Select the Field of View

The default mode is `wide`:

| Mode | Sensor mode | Output | Description |
| --- | --- | --- | --- |
| `wide` | 1640x1152, 2x2 binned | 1640x1080 | Wider FOV and lower weak-light noise |
| `standard` | 1992x1152, unbinned crop | 1920x1080 | Original cropped view |

Run the original standard mode with:

```bash
DISPLAY=:0 ISP_FOV_MODE=standard ./test/build/isp_pipeline
```

Switching modes does not require a rebuild, module reload, or reboot.

## Capture an Image

Use the same demo in headless mode. This command waits for 90 output frames so
that AE/AWB can settle, saves one processed image, and exits automatically:

```bash
mkdir -p /tmp/isp-captures

ISP_HEADLESS=1 \
ISP_CAPTURE_FRAME=90 \
ISP_CAPTURE_PREFIX=/tmp/isp-captures/wide \
./test/build/isp_pipeline
```

The command creates:

- `/tmp/isp-captures/wide_bgr.png` — normal processed image.
- `/tmp/isp-captures/wide_rgb.png` — channel-swapped diagnostic image.
- `/tmp/isp-captures/wide.raw` — tightly packed BGR24 data.

To capture the standard mode:

```bash
ISP_HEADLESS=1 \
ISP_FOV_MODE=standard \
ISP_CAPTURE_FRAME=90 \
ISP_CAPTURE_PREFIX=/tmp/isp-captures/standard \
./test/build/isp_pipeline
```

To save several consecutive frames, add `ISP_CAPTURE_SERIES`:

```bash
ISP_HEADLESS=1 \
ISP_CAPTURE_FRAME=150 \
ISP_CAPTURE_SERIES=20 \
ISP_CAPTURE_PREFIX=/tmp/isp-captures/noise \
./test/build/isp_pipeline
```

## Normal Daily Use

After the drivers and application are built, preview requires only:

```bash
DISPLAY=:0 ./test/build/isp_pipeline
```

After changing only the demo application:

```bash
sudo cmake --build test/build -j8
DISPLAY=:0 ./test/build/isp_pipeline
```

## Updating the Linux Drivers

This section is for driver development and is not needed for an ordinary demo
run.

First stop `isp_pipeline` and any other camera application. Then rebuild and
reload the modules without replacing the FPGA image or overlay:

```bash
sudo make -j8
sudo make reload
```

After reload, start `isp_pipeline` normally. It reapplies the complete pipeline
configuration at startup.

The reload target refuses to continue while a media or video device is open.
Do not force-remove camera modules. If reload reports a kernel oops, hung task,
or incomplete media graph, stop and reboot the board.

## Optional Developer Checks

The tuning library includes unit tests. They are not required to run the camera
demo:

```bash
sudo ctest --test-dir test/build --output-on-failure
```

## Useful Runtime Options

| Variable | Example | Purpose |
| --- | --- | --- |
| `ISP_FOV_MODE` | `wide` or `standard` | Select the field of view. |
| `ISP_HEADLESS` | `1` | Disable the preview for image capture. |
| `ISP_CAPTURE_FRAME` | `90` | Wait this many output frames before capture. |
| `ISP_CAPTURE_SERIES` | `20` | Save several consecutive BGR PNG files. |
| `ISP_CAPTURE_PREFIX` | `/tmp/capture` | Set the output path and filename prefix. |
| `ISP_TUNING_MODE` | `sensor-ae` or `off` | Use the default Sensor AE or disable tuning. |
| `ISP_DISPLAY_BACKEND` | `mali`, `gstreamer`, or `opencv` | Select the preview backend. |
| `ISP_DISPLAY_WIDTH` | `960` | Set the initial preview width. |
| `ISP_DISPLAY_FPS` | `30` | Set the requested preview frame rate. |

The default `sensor-ae` tuning mode uses ISP statistics to adjust the IMX219
analogue gain while hardware AWB remains enabled. The `wide` and `standard`
modes automatically select their matching BLC and CCM profiles.

## Troubleshooting

- **No preview window:** run the demo as the logged-in desktop user and set
  `DISPLAY=:0`. Use headless capture when no desktop session is available.
- **A device is busy:** stop the process using the camera, then retry. Use
  `sudo fuser -v /dev/media* /dev/video* /dev/v4l-subdev*` to find it.
- **Camera nodes are missing:** confirm that the FPGA image, overlay, and
  modules are installed, then inspect a bounded `dmesg` tail.
- **Format setup fails:** make sure the installed drivers and the demo binary
  were built from the same checkout. Do not add a separate graph-setup step.

Useful diagnostics:

```bash
ls -l /dev/media* /dev/video* /dev/v4l-subdev*
lsmod | grep -E 'xil_isp|xil_vip|xlnx_imx219|mipi_rx'
sudo dmesg | tail -n 120
```

For AI-assisted build, preview, capture, and recovery instructions, see
[AI_RUNBOOK.md](AI_RUNBOOK.md).
