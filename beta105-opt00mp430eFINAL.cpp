//   lol  just encoded this baby - but i cant run it on this hardware - needs a nvenc 2000  or higher ... 
//
//            g++ -std=c++17 -o videoaudio125.exe videoaudio125.cpp     -I"C:/msys64/mingw64/include"     -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale     -lgdi32 -luser32
//    g++ -std=c++17 -o beta03.exe beta03.cpp     -I"C:/msys64/mingw64/include" -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale -lgdi32 -luser32 -lole32 -mconsole
//
//
//    g++ -std=c++17 -o beta10.exe beta10.cpp    -I"C:/NVIDIA_VideoCodecSDK_9.1/include"    -I"C:/ffmpeg/include"    -L"C:/msys64/mingw64/lib"    -L"C:/NVIDIA_VideoCodecSDK_9.1/lib"    -L"C:/ffmpeg/lib"    -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -lole32    -lavcodec -lavformat -lavutil -lswscale    -static-libgcc -static-libstdc++ -mconsole
//
//
#include <chrono>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>  // Added include for std::queue

#include <audioclient.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "Ole32.lib")


extern "C" {
    #include <libavutil/opt.h>  // ✅ Required for av_opt_set
}


#include <condition_variable>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
}


#define FF_API_OLD_CHANNEL_LAYOUT 1

std::atomic<bool> isRecording(false);
std::atomic<bool> stopProgram(false);
std::mutex mtx;
std::condition_variable cv;

// -----------------------------------------------------------------------------
// Added structure to hold captured frame data and its timestamp for encoding
struct CapturedFrame {
    uint8_t* data;
    int64_t timestamp; // in microseconds since start of capture
};

// -----------------------------------------------------------------------------
// FrameBuffer class to hold CapturedFrame pointers, preserving comments and whitespace
class FrameBuffer {
private:
    std::queue<CapturedFrame*> bufferQueue;
    size_t maxSize;
    std::mutex mtx;

public:
    FrameBuffer(size_t size) : maxSize(size) {}

    void push(CapturedFrame* frame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (bufferQueue.size() >= maxSize) {
            CapturedFrame* oldFrame = bufferQueue.front();
            delete[] oldFrame->data; // Drop the oldest frame
            delete oldFrame;
            bufferQueue.pop();
        }
        bufferQueue.push(frame);
    }

    CapturedFrame* pop() {
        std::lock_guard<std::mutex> lock(mtx);
        if (bufferQueue.empty()) return nullptr;
        CapturedFrame* frame = bufferQueue.front();
        bufferQueue.pop();
        return frame;
    }

    bool isEmpty() {
        std::lock_guard<std::mutex> lock(mtx);
        return bufferQueue.empty();
    }
};




bool captureScreen(uint8_t* buffer, int width, int height) {
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, width, height);
    SelectObject(hDC, hBitmap);

    if (!BitBlt(hDC, 0, 0, width, height, hScreen, 0, 0, SRCCOPY)) {
        std::cerr << "Error: BitBlt failed.\n";
        DeleteObject(hBitmap);
        DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);
        return false;
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hDC, hBitmap, 0, height, buffer, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
//formatContext->streams[0]->avg_frame_rate = {30, 1};

//
//(*formatContext)->oformat->video_codec = AV_CODEC_ID_H264;  //  add for avi
//
    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    return true;
}


static int64_t frameCounter = 0;  // ✅ Added missing frame counter
//frame->pts = frameCounter * (codecContext->time_base.den / codecContext->time_base.num) / 30;
//frameCounter++;

/*
bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext, int width, int height, const char* filename) {
    // Allocate format context for MP4
    avformat_alloc_output_context2(formatContext, nullptr, "mp4", filename);
    if (!*formatContext) {
        std::cerr << "Error: Could not allocate AVI format context.\n";
        return false;
    }

    // Select NVIDIA NVENC Encoder
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        std::cerr << "Error: NVIDIA NVENC not found. Make sure your GPU drivers are installed.\n";
        return false;
    }

    // Allocate codec context
    *codecContext = avcodec_alloc_context3(codec);
    if (!*codecContext) {
        std::cerr << "Error: Could not allocate codec context.\n";
        return false;
    }

    (*codecContext)->width = width;
    (*codecContext)->height = height;
//    (*codecContext)->time_base = {1, 30};  // Microsecond timing
//    (*codecContext)->framerate = {30, 1};       // 30 FPS
(*codecContext)->time_base = (AVRational){1, 30};  // 1/30 sec per frame
(*codecContext)->framerate = (AVRational){30, 1};  // 30 FPS




    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecContext)->gop_size = 15;             // Keyframe every second (30 FPS)
    (*codecContext)->max_b_frames = 0;

    // NVENC requires additional settings
    (*codecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; 
    av_opt_set((*codecContext)->priv_data, "preset", "p4", 0); // "p4" = Performance-focused preset
    av_opt_set((*codecContext)->priv_data, "tune", "hq", 0);   // High-quality tuning
//    av_opt_set((*codecContext)->priv_data, "rc", "cbr", 0);    // Variable bitrate
//    av_opt_set((*codecContext)->priv_data, "rc", "cqp", 0);      //    set rc to instead of cbr
    av_opt_set((*codecContext)->priv_data, "cq", "20", 0);
//    av_opt_set((*codecContext)->priv_data, "rc", "constqp", 0);

    // Choose the best NVENC preset for real-time recording
    //av_opt_set(*codecContext)->priv_data, "preset", "p1", 0);  // P1 = Low latency (P7 = Max Quality)
    //av_opt_set(*codecContext)->priv_data, "tune", "hq", 0);    // HQ = High quality mode
    //av_opt_set(*codecContext)->priv_data, "rc", "vbr", 0);     // VBR = Variable bitrate (best for quality)
    av_opt_set((*codecContext)->priv_data, "bitrate", "20000000", 0);  // 50 Mbps bitrate target
    av_opt_set((*codecContext)->priv_data, "maxrate", "20000000", 0);  // 75 Mbps max bitrate
    av_opt_set((*codecContext)->priv_data, "bufsize", "40000000", 0);  // Buffer size for VBR stability

    // Lower latency by reducing frame buffering
    av_opt_set((*codecContext)->priv_data, "delay", "0", 0);
    av_opt_set((*codecContext)->priv_data, "zerolatency", "1", 0);  // Ensures minimal encoding delay
    av_opt_set((*codecContext)->priv_data, "bf", "0", 0);  // No B-frames for low latency

    // Multi-threading optimization
    av_opt_set((*codecContext)->priv_data, "gpu", "0", 0);    // Ensure it's using the correct GPU
    av_opt_set((*codecContext)->priv_data, "threads", "auto", 0);  // Let NVENC handle threading

    // Frame pacing & keyframe settings
    av_opt_set((*codecContext)->priv_data, "g", "15", 0);  // Keyframe interval: 1s (for smooth seeking)
    av_opt_set((*codecContext)->priv_data, "temporal-aq", "1", 0);  // Adaptive quantization for better motion quality
    av_opt_set((*codecContext)->priv_data, "spatial-aq", "1", 0);
    av_opt_set((*codecContext)->priv_data, "aq-strength", "8", 0);  // Moderate AQ strength

    // Improve visual quality for fast motion (use 'll' for ultra-low latency)
    av_opt_set((*codecContext)->priv_data, "profile", "high", 0);
    //av_opt_set(codecCtx->priv_data, "tune", "hq", 0);

    // Open the codec
    if (avcodec_open2(*codecContext, codec, nullptr) < 0) {
        std::cerr << "Error: Could not open NVENC codec.\n";
        return false;
    }

    // Create new stream
    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }

    // Set stream parameters
    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base; // Match codec time_base
    stream->avg_frame_rate = (*codecContext)->framerate;

    // Open output file
    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output MP4 file.\n";
            return false;
        }
    }

    // Write header
    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write MP4 header.\n";
        return false;
    }

    return true;
}
*/


bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext, int width, int height, const char* filename) {
    // Allocate format context for MP4
    avformat_alloc_output_context2(formatContext, nullptr, "mp4", filename);
    if (!*formatContext) {
        std::cerr << "Error: Could not allocate AVI format context.\n";
        return false;
    }

    // Select NVIDIA NVENC Encoder
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        std::cerr << "Error: NVIDIA NVENC not found. Make sure your GPU drivers are installed.\n";
        return false;
    }

    // Allocate codec context
    *codecContext = avcodec_alloc_context3(codec);
    if (!*codecContext) {
        std::cerr << "Error: Could not allocate codec context.\n";
        return false;
    }

    (*codecContext)->width = width;
    (*codecContext)->height = height;
//    (*codecContext)->time_base = {1001, 30000};  // Microsecond timing
//    (*codecContext)->framerate = {30000, 1001};        // 30 FPS
    (*codecContext)->time_base = (AVRational){1, 1000000};  // 1/30 sec per frame
    (*codecContext)->framerate = (AVRational){30, 1};  // 30 FPS
    (*codecContext)->pkt_timebase = (AVRational){1, 1000000};  //
//(*codecContext)->ticks_per_frame = 1;
    (*codecContext)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecContext)->gop_size = 15;              // Keyframe every second (30 FPS)
    (*codecContext)->max_b_frames = 0;


    // NVENC requires additional settings
    (*codecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; 

    AVDictionary* opts = nullptr;
    // Set NVENC options via dictionary

av_dict_set(&opts, "vsync", "cfr", 0);  // Force constant frame rate

    av_dict_set(&opts, "force-cfr", "1", 0); // Force Constant Frame Rate mode
//    av_dict_set(&opts, "vsync", "1", 0);     // Synchronize frames to prevent drift
av_dict_set(&opts, "framerate", "30000/1001", 0);
    av_dict_set(&opts, "preset", "p5", 0); // "p4" = Performance-focused preset
    av_dict_set(&opts, "tune", "hq", 0);   // High-quality tuning
    av_dict_set(&opts, "rc", "cbr", 0);    // Variable bitrate (uncomment if needed) --- wrong
//    av_dict_set(&opts, "rc", "cqp", 0);      // Use constant quantizer mode instead of cbr
    av_dict_set(&opts, "cq", "18", 0);
//    av_dict_set(&opts, "rc", "constqp", 0);
    av_dict_set(&opts, "bitrate", "20000000", 0);  // 20 Mbps bitrate target
    av_dict_set(&opts, "maxrate", "20000000", 0);  // 20 Mbps max bitrate
    av_dict_set(&opts, "bufsize", "40000000", 0);  // Buffer size for VBR stability
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "zerolatency", "1", 0);  // Ensures minimal encoding delay
    av_dict_set(&opts, "bf", "0", 0);           // No B-frames for low latency
    av_dict_set(&opts, "gpu", "0", 0);          // Ensure it's using the correct GPU
    av_dict_set(&opts, "threads", "auto", 0);   // Let NVENC handle threading
    av_dict_set(&opts, "g", "15", 0);           // Keyframe interval: 1s (for smooth seeking)
    av_dict_set(&opts, "temporal-aq", "1", 0);   // Adaptive quantization for better motion quality
    av_dict_set(&opts, "spatial-aq", "1", 0);
    av_dict_set(&opts, "aq-strength", "15", 0);   // Moderate AQ strength
//    av_dict_set(&opts, "profile", "high", 0);
    av_dict_set(&opts, "profile", "100", 0); // Set high profile (100) for NVENC
    av_dict_set(&opts, "rc-lookahead", "20", 0);  // Optimizes frame prediction
av_dict_set(&opts, "no-scenecut", "1", 0);  // Prevent NVENC from skipping frames
av_dict_set(&opts, "strict_gop", "1", 0);   // Ensure frames stay at 30 FPS
//av_dict_set(&opts, "rc", "cbr_hq", 0);  // Force high-quality CBR

//🚀 Summary: Only 3 Character Changes   ...says AI
//Setting	Change From (30 FPS)	Change To (60 FPS)
//time_base	{1, 30}	{1, 60}
//framerate	{30, 1}	{60, 1}
//pkt_timebase	{1, 30}	{1, 60}








    if (avcodec_open2(*codecContext, codec, &opts) < 0) {
        std::cerr << "Error: Could not open NVENC codec.\n";
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    // Create new stream
    AVStream* stream = avformat_new_stream(*formatContext, nullptr);
    if (!stream) {
        std::cerr << "Error: Could not create stream.\n";
        return false;
    }

    // Set stream parameters
    avcodec_parameters_from_context(stream->codecpar, *codecContext);
    stream->time_base = (*codecContext)->time_base; // Match codec time_base
    stream->avg_frame_rate = (*codecContext)->framerate;

    // Open output file
    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Could not open output MP4 file.\n";
            return false;
        }
    }

    // Write header
    if (avformat_write_header(*formatContext, nullptr) < 0) {
        std::cerr << "Error: Could not write MP4 header.\n";
        return false;
    }

    return true;
}








#include <vector>  // ✅ Added this to fix the missing include


// ✅ Fixed incorrect pointer dereference in encodeFrame()

//void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext, uint8_t* rgbBuffer, int width, int height, int64_t& frameCounter) {
//    AVFrame* frame = av_frame_alloc();
//    frame->format = codecContext->pix_fmt;
//    frame->width = width;
//    frame->height = height;
//    av_frame_get_buffer(frame, 32);


void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext, uint8_t* rgbBuffer, int width, int height, int64_t& frameCounter, std::chrono::high_resolution_clock::time_point startTime) {

auto currentTime = std::chrono::high_resolution_clock::now();
int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();






////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Correct PTS calculation based on real-time timestamps
//frame->pts = av_rescale_q(elapsedTime, (AVRational){1, 1000000}, codecContext->time_base);     //  was using this  ?
frame->pts = (int64_t)(frameCounter * (1000.0 / 30.0 * 44.77));//////////////////44.84/////81///44.78///









//    auto currentTime = std::chrono::high_resolution_clock::now();
//    int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

    // Use real-time PTS
//    frame->pts = av_rescale_q(elapsedTime, (AVRational){1, 1000000}, codecContext->time_base);
}




    SwsContext* swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    uint8_t* srcSlice[1] = {rgbBuffer};
    int srcStride[1] = {4 * width};
    sws_scale(swsContext, srcSlice, srcStride, 0, height, frame->data, frame->linesize);
    sws_freeContext(swsContext);








    // ✅ FIXED: Ensure PTS always increases
//    frame->pts = frameCounter * 1000000 / 30;  // ✅ Slows it down to match 30 FPS
//frame->pts = frameCounter * (30000 / 30);  // Calculate timestamps correctly
static int64_t frame_index = 0;
//frame->pts = frame_index * 1;  // Ensure each frame is spaced correctly
//frame->pts = frame_index * 1001 / 30;  //
//frame->pts = frame_index * 30;
//frame->pts = frame_index * (int64_t)(30000.0 / 1001 * codecContext->time_base.den / codecContext->time_base.num);

//frame->pts = frame_index * codecContext->time_base.den / codecContext->time_base.num;
//frame->pts = frame_index * (int64_t)(30 * codecContext->time_base.den / codecContext->time_base.num);
//frame_index++;


auto currentTime = std::chrono::high_resolution_clock::now();
int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

// Use real-time PTS calculation
frame->pts = av_rescale_q(elapsedTime, (AVRational){1, 1000000}, codecContext->time_base);




//frame->pts = av_rescale_q(frame_index, (AVRational){1, 30}, codecContext->time_base);
//frame_index++;


//frame->pts = frame_index * (*codecContext)->time_base.den / (*codecContext)->time_base.num;
//frame_index++;  // Increment frame count




//frame->pts = av_rescale_q(frameCounter, (AVRational){1, 30}, codecContext->time_base);
//    frameCounter++;
//    frame->pts = frameCounter++;  

    AVPacket* packet = av_packet_alloc();
    if (avcodec_send_frame(codecContext, frame) >= 0) {
        while (avcodec_receive_packet(codecContext, packet) >= 0) {
            av_interleaved_write_frame(formatContext, packet);
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
}







#include <chrono>

void recordingThread(int width, int height) {
    char outputFilename[128];
    time_t now = time(nullptr);
//    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mp4", localtime(&now));
       strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mp4", localtime(&now));

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;

    if (!initializeFFmpeg(&formatContext, &codecContext, width, height, outputFilename)) {
        return;
    }

    // Create a FrameBuffer for captured frames (buffer size can be tuned as needed)
    FrameBuffer frameBuffer(30);

    int64_t localFrameCounter = 0;
    auto startTime = std::chrono::high_resolution_clock::now();

    // -------------------------------------------------------------------------
    // Capture Thread: Captures screen frames and pushes them into the buffer.
    std::thread captureThread([&]() {
        auto nextFrameTime = startTime;
        while (isRecording) {
            // Allocate a new buffer for each frame
            uint8_t* buffer = new uint8_t[width * height * 4];

            // Capture screen into the new buffer and record capture timestamp
auto captureStart = std::chrono::high_resolution_clock::now();
if (!captureScreen(buffer, width, height)) {
    std::cerr << "Error capturing screen.\n";
    delete[] buffer;
    break;
}
auto captureEnd = std::chrono::high_resolution_clock::now();
int64_t captureDuration = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();
auto captureStart = std::chrono::high_resolution_clock::now();
if (!captureScreen(buffer, width, height)) {
    std::cerr << "Error capturing screen.\n";
    delete[] buffer;
    break;
}
auto captureEnd = std::chrono::high_resolution_clock::now();
int64_t captureDuration = std::chrono::duration_cast<std::chrono::microseconds>(captureEnd - captureStart).count();

// Ensure a stable frame interval of 30 FPS
int64_t waitTime = (1000000 / 30) - captureDuration;
if (waitTime > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
}


 //           std::cout << "Capture Time: " << captureTime << " µs\n";

            auto currentTime = std::chrono::high_resolution_clock::now();
            int64_t elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - startTime).count();

            // Create a CapturedFrame struct and push it to the buffer
            CapturedFrame* capFrame = new CapturedFrame;
            capFrame->data = buffer;
            capFrame->timestamp = elapsedTime;
            frameBuffer.push(capFrame);

        
        }
    });

    // -------------------------------------------------------------------------
    // Encoding Thread: Pops frames from the buffer and encodes them.
    std::thread encodingThread([&]() {
        // Continue processing while recording or there are frames in the buffer
        while (isRecording || !frameBuffer.isEmpty()) {
            CapturedFrame* capFrame = frameBuffer.pop();
            if (capFrame == nullptr) {
                // If no frame is available, sleep briefly and try again.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            encodeFrame(codecContext, formatContext, capFrame->data, width, height, localFrameCounter);
            // Free the frame data after encoding
            delete[] capFrame->data;
            delete capFrame;
        }
    });

    // Wait for both threads to finish
    captureThread.join();
    encodingThread.join();

    av_write_trailer(formatContext);
    avcodec_free_context(&codecContext);
    avformat_free_context(formatContext);

    std::cout << "Recording saved to " << outputFilename << ".\n";
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;
        if (kbStruct->vkCode == 0x55) { // 'U' key
            if (wParam == WM_KEYDOWN) {
                isRecording = !isRecording;
                cv.notify_all();
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);

    if (width <= 0 || height <= 0) {
        std::cerr << "Error: Invalid screen resolution.\n";
        return -1;
    }

    std::thread recorder([&]() {
        while (!stopProgram) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []() { return isRecording || stopProgram; });
            if (stopProgram) break;

            std::cout << "Recording started. Press \\  U to stop.\n";
            recordingThread(width, height);
            std::cout << "Recording stopped. Press \\  U to start again.\n";
        }
    });

    HHOOK keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!keyboardHook) {
        std::cerr << "Error: Could not set keyboard hook.\n";
        return -1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    stopProgram = true;
    cv.notify_all();
    recorder.join();

    UnhookWindowsHookEx(keyboardHook);
    return 0;
}

