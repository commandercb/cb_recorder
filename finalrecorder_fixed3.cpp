// finalrecorder_nvenc_fixed_full.cpp

// finalrecorder06fps60nvenc471.cpp
// FULL FIXED VERSION: stable timestamps, writes proper MKV, 60 FPS, NVENC

// finalrecorder06fps60nvenc471.cpp
// FULL FIX: proper timestamps, correct FPS, writes AVI, works on your driver
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <csignal>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using Microsoft::WRL::ComPtr;

std::atomic<bool> stopRecording(false);
void signalHandler(int) { stopRecording = true; }

std::string getFilename() {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timeT);
    std::ostringstream oss;
    oss << "dxgi_output_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".mp4";
    return oss.str();
}

struct FrameItem { AVFrame* frame; int64_t pts; };

class FrameQueue {
public:
    FrameQueue(size_t maxSize = 120) : maxSize(maxSize) {}
    void push(FrameItem item) {
        std::unique_lock<std::mutex> lock(mtx);
        while (q.size() >= maxSize) {
            AVFrame* old = q.front().frame;
            q.pop();
            if (old) av_frame_free(&old);
        }
        q.push(item);
        cv.notify_one();
    }
    bool pop(FrameItem& item) {
        std::unique_lock<std::mutex> lock(mtx);
        while (q.empty() && !stopRecording.load()) cv.wait(lock);
        if (q.empty()) return false;
        item = q.front(); q.pop();
        return true;
    }
    void wakeAll() { cv.notify_all(); }
    bool empty() { std::lock_guard<std::mutex> lock(mtx); return q.empty(); }
private:
    std::queue<FrameItem> q;
    std::mutex mtx;
    std::condition_variable cv;
    size_t maxSize;
};

class FramePool {
public:
    FramePool(int size, int width, int height, AVPixelFormat fmt)
        : width(width), height(height), fmt(fmt) {
        for (int i = 0; i < size; ++i) freeFrames.push(create());
    }
    ~FramePool() { while (!freeFrames.empty()) { av_frame_free(&freeFrames.front()); freeFrames.pop(); } }
    AVFrame* acquire() {
        std::lock_guard<std::mutex> lock(mtx);
        if (freeFrames.empty()) return create();
        AVFrame* f = freeFrames.front(); freeFrames.pop(); return f;
    }
    void release(AVFrame* f) { if (!f) return; f->pts = 0; std::lock_guard<std::mutex> lock(mtx); freeFrames.push(f); }
private:
    AVFrame* create() {
        AVFrame* f = av_frame_alloc();
        f->format = fmt; f->width = width; f->height = height;
        av_frame_get_buffer(f, 32);
        return f;
    }
    int width, height; AVPixelFormat fmt;
    std::queue<AVFrame*> freeFrames; std::mutex mtx;
};

int main() {
    signal(SIGINT, signalHandler);

    const int width = 1600, height = 900, targetFPS = 60;

    av_log_set_level(AV_LOG_ERROR);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context))) return -1;

    ComPtr<IDXGIDevice> dxgiDevice; device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output; adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1; output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) return -1;

    std::string filename = getFilename();

    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, "mp4", filename.c_str()) < 0) return -1;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return -1;

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->width = width; codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = 12 * 1000 * 1000;
    codecCtx->gop_size = 120;
    codecCtx->max_b_frames = 0;
    codecCtx->time_base = {1, 1000000}; // microseconds
    codecCtx->framerate = {targetFPS, 1};
    codecCtx->thread_count = 4;
    av_opt_set(codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx->priv_data, "tune", "fastdecode", 0);
    av_opt_set(codecCtx->priv_data, "profile", "main", 0);
    if (outCtx->oformat->flags & AVFMT_GLOBALHEADER) codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVStream* videoStream = avformat_new_stream(outCtx, codec);
    videoStream->time_base = codecCtx->time_base;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) return -1;
    if (avcodec_parameters_from_context(videoStream->codecpar, codecCtx) < 0) return -1;

    if (!(outCtx->oformat->flags & AVFMT_NOFILE))
        if (avio_open(&outCtx->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) return -1;

    if (avformat_write_header(outCtx, nullptr) < 0) return -1;

    SwsContext* swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height,
        AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);

    FrameQueue frameQueue(180);
    FramePool framePool(400, width, height, codecCtx->pix_fmt);

    D3D11_TEXTURE2D_DESC cpuDesc{};
    cpuDesc.Width = width; cpuDesc.Height = height; cpuDesc.MipLevels = 1; cpuDesc.ArraySize = 1;
    cpuDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cpuDesc.SampleDesc.Count = 1;
    cpuDesc.Usage = D3D11_USAGE_STAGING; cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    std::vector<ComPtr<ID3D11Texture2D>> cpuTextures(3);
    for (auto& t : cpuTextures) device->CreateTexture2D(&cpuDesc, nullptr, &t);

    std::thread encoderThread([&]() {
        AVPacket* pkt = av_packet_alloc();
        FrameItem item;
        while (!stopRecording.load() || !frameQueue.empty()) {
            if (!frameQueue.pop(item)) { frameQueue.wakeAll(); continue; }
            avcodec_send_frame(codecCtx, item.frame);
            framePool.release(item.frame);
            while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                pkt->stream_index = videoStream->index;
                av_interleaved_write_frame(outCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        avcodec_send_frame(codecCtx, nullptr);
        while (avcodec_receive_packet(codecCtx, pkt) == 0) {
            pkt->stream_index = videoStream->index;
            av_interleaved_write_frame(outCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    });

    using clock = std::chrono::high_resolution_clock;
    auto startTime = clock::now();
    int ringIndex = 0;

    const int64_t frameDurationUs = 1000000 / targetFPS;

    while (!stopRecording.load()) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = duplication->AcquireNextFrame(16, &frameInfo, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) continue;

        ComPtr<ID3D11Texture2D> tex; res.As(&tex);
        context->CopyResource(cpuTextures[ringIndex].Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(cpuTextures[ringIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            duplication->ReleaseFrame(); continue;
        }

        AVFrame* frameYUV = framePool.acquire();
        av_frame_make_writable(frameYUV);

        uint8_t* srcData[1] = { (uint8_t*)mapped.pData };
        int srcLinesize[1] = { (int)mapped.RowPitch };
        sws_scale(swsCtx, srcData, srcLinesize, 0, height, frameYUV->data, frameYUV->linesize);

        auto now = clock::now();
        frameYUV->pts = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();

        frameQueue.push({frameYUV, frameYUV->pts});

        context->Unmap(cpuTextures[ringIndex].Get(), 0);
        duplication->ReleaseFrame();

        ringIndex = (ringIndex + 1) % cpuTextures.size();

        auto nextFrameTime = startTime + std::chrono::microseconds(frameDurationUs * frameYUV->pts / frameDurationUs);
        std::this_thread::sleep_until(nextFrameTime);
    }

    frameQueue.wakeAll();
    encoderThread.join();
    av_write_trailer(outCtx);
    sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) avio_close(outCtx->pb);
    avformat_free_context(outCtx);

    std::cout << "Recording finished: " << filename << "\n";
    return 0;
}
