# CBRecorderZero

A lightweight Windows screen recorder built around **DXGI Desktop Duplication + FFmpeg + NVIDIA NVENC**, designed specifically to minimize CPU overhead while recording gameplay.

The project was developed as a lightweight alternative to OBS for systems where recording overhead matters.

## Current Result

CBRecorderZero successfully records:

* **Resolution:** 1600×900
* **Frame rate:** 60 FPS
* **Codec:** H.264 NVENC
* **Pixel format:** YUV420P
* **Chroma:** 4:2:0
* **Bit depth:** 8-bit
* **Profile:** Main@L4.2
* **GOP:** 120 frames
* **Container:** MP4
* **FFmpeg:** 5.1-era libraries
* **NVENC API:** 11.1-compatible NVIDIA driver stack

A 21.8-second test recording produced a valid:

> 1600×900, constant 60.000 FPS H.264 MP4

The recorder also completed a **7 minute 57 second PUBG recording** without the video stopping or becoming corrupted.

The most notable result is resource usage: during testing, CPU utilization was observed at approximately **0–0.3%**, with the NVIDIA video encoder typically around **7–12%**.

This makes CBRecorderZero exceptionally lightweight compared with conventional screen-recording software.

---

# Why CBRecorderZero Exists

The original CBRecorder project worked, but the recording pipeline performed unnecessary image-processing work.

The original pipeline was essentially:

```text
DXGI Desktop Duplication
        ↓
BGRA frame
        ↓
libswscale
        ↓
YUV420P
        ↓
NVENC
        ↓
H.264
        ↓
MP4
```

The goal of CBRecorderZero was to determine how much of that processing could be removed while retaining reliable 1600×900 60-FPS recording.

The original recorder was deliberately left untouched as a baseline.

CBRecorderZero became a separate optimization branch.

---

# The Major Optimization

The largest change was removing **libswscale** from the capture pipeline.

Instead of calling `sws_scale()` for every captured frame, CBRecorderZero performs the required BGRA → YUV420P conversion directly.

The resulting pipeline is:

```text
DXGI Desktop Duplication
        ↓
BGRA frame
        ↓
Direct BGRA → YUV420P conversion
        ↓
NVENC hardware encoder
        ↓
H.264
        ↓
MP4
```

This eliminates the general-purpose `libswscale` stage.

The result was substantially lower observed CPU utilization while maintaining correct H.264 encoding.

---

# Capture

CBRecorderZero uses the Windows **DXGI Desktop Duplication API**.

A D3D11 device captures the desktop output and copies each frame into a staging texture:

```text
DXGI_FORMAT_B8G8R8A8_UNORM
```

The frame is then mapped and converted directly into the encoder's YUV420P frame buffers.

No desktop image is passed through a GUI recording framework.

There is no OBS capture pipeline.

There is no preview window.

There is no unnecessary compositing stage.

---

# Encoding

Encoding is performed by NVIDIA's hardware H.264 encoder:

```text
h264_nvenc
```

The encoder handles the computationally expensive H.264 compression on the GPU.

The current configuration uses:

```text
1600 × 900
60 FPS
H.264
YUV420P
18 Mbps target bitrate
GOP = 120
B-frames = 0
```

The actual bitrate is variable because NVENC is allowed to vary the bitrate according to the encoded content.

---

# Why the FFmpeg Version Matters

One important part of the project was maintaining compatibility with an older NVIDIA driver.

Modern FFmpeg builds can require newer NVENC APIs and therefore newer NVIDIA drivers.

The system used for development has:

```text
NVENC API: 11.1
```

Newer FFmpeg builds produced:

```text
Driver does not support the required nvenc API version.
Required: 13.0
Found: 11.1
```

Those builds required NVIDIA driver 570 or newer.

Rather than upgrading the NVIDIA driver, CBRecorderZero was built against the older FFmpeg 5.1-era libraries.

The working runtime uses:

```text
avcodec-59.dll
avformat-59.dll
avutil-57.dll
```

and reports:

```text
Writing application: Lavf59.27.100
```

This allows the recorder to use the existing NVIDIA driver and NVENC API.

---

# An Important Timestamp Fix

During development, the recorder initially produced a file that MediaInfo reported as:

```text
15,360 FPS
```

even though the capture loop was intended to run at 60 FPS.

The ratio was:

```text
15360 / 60 = 256
```

The problem was incorrect timestamp handling between the encoder and output stream.

The final version explicitly defines the encoder frame rate and stream timing and rescales encoded packet timestamps before writing them to the MP4 container.

The important operation is:

```cpp
av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
```

After the fix, MediaInfo reported:

```text
Frame rate mode : Constant
Frame rate      : 60.000 fps
```

This confirmed that the recorder was producing correctly timed 60-FPS video.

---

# Encoder Shutdown

The encoder is also explicitly flushed during shutdown.

The final recorder sends:

```text
NULL frame
```

to the encoder so that any buffered packets can be written before the MP4 container is closed.

This is important for producing complete recordings rather than simply terminating the encoder and hoping all buffered data has already been written.

---

# Frame Management

CBRecorderZero uses a frame queue and frame pool to avoid repeatedly allocating memory for every captured frame.

The current design includes:

```text
FrameQueue maximum: 120 frames
FramePool:          300 frames
```

This reduces allocation overhead and allows capture and encoding to operate independently.

The capture thread can continue acquiring frames while the encoder processes queued frames.

---

# Frame Pacing

The capture loop uses `std::chrono::steady_clock` for frame pacing.

At 60 FPS, the target interval is approximately:

```text
16.666 ms
```

The recorder advances the next target frame time rather than simply sleeping for an arbitrary amount after each frame.

This helps maintain consistent timing.

---

# Test Results

## Short validation test

A test recording produced:

```text
Duration:          21.817 seconds
Resolution:        1600×900
Frame rate:        60.000 FPS
Frame rate mode:   Constant
Codec:             H.264
Profile:           Main@L4.2
Pixel format:      YUV420P
Chroma:            4:2:0
Bit depth:         8-bit
GOP:               120
Container:         MP4
FFmpeg:            Lavf59.27.100
```

File size:

```text
16.8 MiB
```

Average bitrate:

```text
6.45 Mbps
```

The lower measured bitrate is normal for a short recording because NVENC is not being forced to maintain an exact constant bitrate.

---

# Long Gameplay Test

CBRecorderZero was also tested during an actual PUBG session.

Recorded duration:

```text
7 minutes 57 seconds
```

File size:

```text
1.66 GiB
```

Average bitrate:

```text
29.8 Mbps overall
```

Video bitrate:

```text
17.2 Mbps
```

Video:

```text
1600×900
29.970 FPS reported by that particular test
H.264 Main@L4.1
YUV420P
8-bit
```

The recording completed successfully without the earlier short-recording/stopping problem.

The long test demonstrated that the capture pipeline could operate continuously during actual gameplay.

---

# Resource Usage

One of the primary goals of CBRecorderZero is minimizing recording overhead.

During testing of the 60-FPS build:

```text
CPU: approximately 0–0.3%
GPU encoder: approximately 7–12%
```

These are observed values on the development system rather than universal benchmarks.

The exact utilization will vary depending on:

* NVIDIA driver
* GPU
* game workload
* desktop composition
* encoder settings
* background processes
* Windows version

The important result is that the recorder does not require a large CPU workload to produce 1600×900 60-FPS H.264 video.

---

# Design Philosophy

CBRecorderZero deliberately avoids features that are useful in full recording suites but unnecessary for a minimal gameplay recorder.

There is currently no:

* Preview window
* Scene system
* Browser source
* Filters
* GUI recording studio
* Streaming system
* Audio mixer
* Plugin system
* Real-time video processing pipeline

The objective is simple:

```text
Capture → Encode → Save
```

with as little processing between those stages as possible.

---

# Current Architecture

```text
                 ┌─────────────────────┐
                 │  Windows Desktop    │
                 └──────────┬──────────┘
                            │
                            ▼
                 ┌─────────────────────┐
                 │ DXGI Desktop        │
                 │ Duplication         │
                 └──────────┬──────────┘
                            │
                         BGRA
                            │
                            ▼
                 ┌─────────────────────┐
                 │ Direct BGRA →       │
                 │ YUV420P conversion  │
                 └──────────┬──────────┘
                            │
                         YUV420P
                            │
                            ▼
                 ┌─────────────────────┐
                 │ NVIDIA NVENC        │
                 │ H.264 Encoder       │
                 └──────────┬──────────┘
                            │
                          H.264
                            │
                            ▼
                 ┌─────────────────────┐
                 │ FFmpeg MP4 Muxer    │
                 └──────────┬──────────┘
                            │
                            ▼
                       .mp4 file
```

---

# What Was Removed

The optimization work specifically investigated unnecessary processing in the original recorder.

The major removal was:

```text
libswscale / sws_scale()
```

The project retains the original recorder separately so that performance and reliability can be compared against the optimized implementation.

This makes CBRecorderZero an experimental optimization branch rather than a replacement that destroys the known-good baseline.

---

# Current Status

CBRecorderZero has reached the point where the core concept is proven:

* DXGI capture works.
* Direct BGRA → YUV420P conversion works.
* libswscale is no longer required.
* NVENC works with the existing NVIDIA driver.
* FFmpeg 5.1-era libraries work correctly.
* 1600×900 recording works.
* 60-FPS timing works.
* MP4 timestamp handling is corrected.
* Long gameplay recording works.
* CPU overhead is extremely low in testing.

The next stage is benchmarking and refinement rather than redesigning the fundamental recording pipeline.

---

# Project Goal

The goal of CBRecorderZero is not to reproduce OBS.

The goal is to answer a much narrower question:

> How little system overhead is required to reliably capture 1600×900 gameplay at 60 FPS when the GPU already provides hardware H.264 encoding?

The current implementation demonstrates that the answer can be surprisingly small when unnecessary processing is removed from the capture path.
