//using the MinGW64 shell
//                 
//            g++ -std=c++17 -o videoaudio125.exe videoaudio125.cpp     -I"C:/msys64/mingw64/include"     -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale     -lgdi32 -luser32
//    g++ -std=c++17 -o beta03.exe beta03.cpp     -I"C:/msys64/mingw64/include" -L"C:/msys64/mingw64/lib"     -lavcodec -lavformat -lavutil -lswscale -lgdi32 -luser32 -lole32 -mconsole
//
//
//    g++ -std=c++17 -o beta10.exe beta10.cpp    -I"C:/NVIDIA_VideoCodecSDK_9.1/include"    -I"C:/ffmpeg/include"    -L"C:/msys64/mingw64/lib"    -L"C:/NVIDIA_VideoCodecSDK_9.1/lib"    -L"C:/ffmpeg/lib"    -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -lole32    -lavcodec -lavformat -lavutil -lswscale    -static-libgcc -static-libstdc++ -mconsole
//
//
#include <windows.h>
#include <iostream>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// --- Globals ---
std::atomic<bool> isRecording(false);
std::atomic<bool> stopProgram(false);
std::mutex mtx;
std::condition_variable cv;

// --- Forward declarations ---
bool initializeFFmpeg(AVFormatContext** formatContext, AVCodecContext** codecContext,
                      int width, int height, const char* filename);
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext,
                 uint8_t* rgbBuffer, int width, int height, int64_t pts);
bool captureScreen(uint8_t* buffer, int width, int height);

// --- Low-level keyboard hook ---
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN && kb->vkCode == 0x55) { // 'U' key
            isRecording = !isRecording;
            cv.notify_all();
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// --- Capture screen to BGRA buffer ---
bool captureScreen(uint8_t* buffer, int width, int height) {
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, width, height);
    SelectObject(hDC, hBitmap);

    if (!BitBlt(hDC, 0, 0, width, height, hScreen, 0, 0, SRCCOPY)) {
        DeleteObject(hBitmap);
        DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);
        return false;
    }

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hDC, hBitmap, 0, height, buffer, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);
    return true;
}

// --- Encode a single frame ---
void encodeFrame(AVCodecContext* codecContext, AVFormatContext* formatContext,
                 uint8_t* rgbBuffer, int width, int height, int64_t pts) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;

    frame->format = codecContext->pix_fmt;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) { av_frame_free(&frame); return; }

    SwsContext* swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                        width, height, AV_PIX_FMT_YUV420P,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    uint8_t* srcSlice[1] = { rgbBuffer };
    int srcStride[1] = { 4 * width };
    sws_scale(swsCtx, srcSlice, srcStride, 0, height, frame->data, frame->linesize);
    sws_freeContext(swsCtx);

    frame->pts = pts;

    avcodec_send_frame(codecContext, frame);

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecContext, pkt) >= 0) {
        AVStream* out_stream = formatContext->streams[0];
        av_packet_rescale_ts(pkt, codecContext->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(formatContext, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// --- Video-only FFmpeg initialization ---
bool initializeFFmpeg(AVFormatContext** fmtCtx, AVCodecContext** codecCtx,
                      int width, int height, const char* filename) {
    avformat_alloc_output_context2(fmtCtx, nullptr, "matroska", filename);
    if (!*fmtCtx) return false;

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    *codecCtx = avcodec_alloc_context3(codec);
    (*codecCtx)->width = width;
    (*codecCtx)->height = height;
    (*codecCtx)->time_base = {1, 30};
    (*codecCtx)->framerate = {30, 1};
    (*codecCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*codecCtx)->gop_size = 30;
    (*codecCtx)->max_b_frames = 0;
    (*codecCtx)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set((*codecCtx)->priv_data, "preset", "p4", 0);
    av_opt_set((*codecCtx)->priv_data, "tune", "hq", 0);
    av_opt_set((*codecCtx)->priv_data, "rc", "constqp", 0);
    av_opt_set((*codecCtx)->priv_data, "cq", "20", 0);
    av_opt_set((*codecCtx)->priv_data, "zerolatency", "1", 0);

    if (avcodec_open2(*codecCtx, codec, nullptr) < 0) return false;

    AVStream* stream = avformat_new_stream(*fmtCtx, nullptr);
    if (!stream) return false;

    avcodec_parameters_from_context(stream->codecpar, *codecCtx);
    stream->time_base = (*codecCtx)->time_base;
    stream->avg_frame_rate = (*codecCtx)->framerate;

    if (!((*fmtCtx)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*fmtCtx)->pb, filename, AVIO_FLAG_WRITE) < 0) return false;
    }

    if (avformat_write_header(*fmtCtx, nullptr) < 0) return false;
    return true;
}

// --- Recording thread ---
void recordingThread(int width, int height) {
    char outputFilename[128];
    time_t now = time(nullptr);
    strftime(outputFilename, sizeof(outputFilename), "recording_%Y%m%d_%H%M%S.mkv", localtime(&now));

    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    if (!initializeFFmpeg(&fmtCtx, &codecCtx, width, height, outputFilename)) return;

    uint8_t* buffer = new uint8_t[width * height * 4];
    std::cout << "Recording to " << outputFilename << "...\n";

    auto startTime = std::chrono::steady_clock::now();

    while (isRecording.load()) {
        auto nowTime = std::chrono::steady_clock::now();
        int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(nowTime - startTime).count();

        int64_t pts = elapsed_us * codecCtx->time_base.den / (1000000 * codecCtx->time_base.num);

        if (!captureScreen(buffer, width, height)) break;
        encodeFrame(codecCtx, fmtCtx, buffer, width, height, pts);

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Flush encoder without passing nullptr buffer
    avcodec_send_frame(codecCtx, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx, pkt) >= 0) {
        AVStream* out_stream = fmtCtx->streams[0];
        av_packet_rescale_ts(pkt, codecCtx->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(fmtCtx);
    if (fmtCtx && !(fmtCtx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmtCtx->pb);

    avcodec_free_context(&codecCtx);
    avformat_free_context(fmtCtx);
    delete[] buffer;

    std::cout << "Recording saved to " << outputFilename << "\n";
}

// --- Main ---
int main() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);

    std::thread recorder([&]() {
        while (!stopProgram) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [] { return isRecording || stopProgram; });
            if (stopProgram) break;
            recordingThread(width, height);
        }
    });

    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!hook) std::cerr << "Warning: SetWindowsHookEx failed.\n";

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    stopProgram = true;
    cv.notify_all();
    recorder.join();
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}
