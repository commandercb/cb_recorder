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

struct FrameItem {
    AVFrame* frame;
    int64_t pts;
};

class FrameQueue {
public:
    FrameQueue(size_t maxSize = 120) : maxSize(maxSize) {}

    void push(FrameItem item) {
        std::unique_lock<std::mutex> lock(mtx);

        if (q.size() >= maxSize) {
            AVFrame* old = q.front().frame;
            q.pop();
            if (old) av_frame_free(&old);
        }

        q.push(item);
        cv.notify_one();
    }

    bool pop(FrameItem& item) {
        std::unique_lock<std::mutex> lock(mtx);
        while (q.empty() && !stopRecording.load())
            cv.wait(lock);

        if (q.empty()) return false;

        item = q.front();
        q.pop();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return q.empty();
    }

    void wakeAll() { cv.notify_all(); }

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
        for (int i = 0; i < size; ++i)
            freeFrames.push(create());
    }

    ~FramePool() {
        while (!freeFrames.empty()) {
            av_frame_free(&freeFrames.front());
            freeFrames.pop();
        }
    }

    AVFrame* acquire() {
        std::lock_guard<std::mutex> lock(mtx);
        if (freeFrames.empty()) return create();

        AVFrame* f = freeFrames.front();
        freeFrames.pop();
        return f;
    }

    void release(AVFrame* f) {
        if (!f) return;
        f->pts = 0;
        std::lock_guard<std::mutex> lock(mtx);
        freeFrames.push(f);
    }

private:
    AVFrame* create() {
        AVFrame* f = av_frame_alloc();
        f->format = fmt;
        f->width = width;
        f->height = height;
        av_frame_get_buffer(f, 32);
        return f;
    }

    int width, height;
    AVPixelFormat fmt;
    std::queue<AVFrame*> freeFrames;
    std::mutex mtx;
};

int main() {
    signal(SIGINT, signalHandler);

    const int width = 1600;
    const int height = 900;

    av_log_set_level(AV_LOG_ERROR);

    // ---------------- D3D11 INIT ----------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;

    if (FAILED(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context)))
        return -1;

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> duplication;

    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->EnumOutputs(0, &output);
    output.As(&output1);

    if (FAILED(output1->DuplicateOutput(device.Get(), &duplication)))
        return -1;

    // ---------------- FFmpeg INIT ----------------
    std::string filename = getFilename();

    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, "mp4", filename.c_str()) < 0)
        return -1;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return -1;

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);

    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = 18 * 1000 * 1000;
    codecCtx->gop_size = 120;
    codecCtx->max_b_frames = 0;

    // ✅ REAL-TIME TIMING MODEL (FIXED)
    const int timeBase = 1000000;
    codecCtx->time_base = {1, timeBase};

    codecCtx->thread_count = 4;

    av_opt_set(codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx->priv_data, "tune", "fastdecode", 0);

    if (outCtx->oformat->flags & AVFMT_GLOBALHEADER)
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVStream* stream = avformat_new_stream(outCtx, codec);
    stream->time_base = codecCtx->time_base;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) return -1;
    if (avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) return -1;

    if (!(outCtx->oformat->flags & AVFMT_NOFILE))
        avio_open(&outCtx->pb, filename.c_str(), AVIO_FLAG_WRITE);

    avformat_write_header(outCtx, nullptr);

    // ---------------- SCALE ----------------
    SwsContext* swsCtx = sws_getContext(
        width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    FrameQueue queue(120);
    FramePool pool(300, width, height, codecCtx->pix_fmt);

    // ---------------- GPU BUFFER ----------------
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> cpuTex;
    device->CreateTexture2D(&desc, nullptr, &cpuTex);

    // ---------------- TIMING ----------------
    auto startTime = std::chrono::steady_clock::now();

    // ---------------- ENCODER ----------------
    std::thread encoder([&]() {
        AVPacket* pkt = av_packet_alloc();
        FrameItem item;

        while (!stopRecording.load() || !queue.empty()) {
            if (!queue.pop(item)) continue;

            avcodec_send_frame(codecCtx, item.frame);
            pool.release(item.frame);

            while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                pkt->stream_index = stream->index;
                av_interleaved_write_frame(outCtx, pkt);
                av_packet_unref(pkt);
            }
        }

        av_packet_free(&pkt);
    });

    // ---------------- CAPTURE LOOP ----------------
    while (!stopRecording.load()) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;

        if (FAILED(duplication->AcquireNextFrame(16, &info, &res)))
            continue;

        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);

        context->CopyResource(cpuTex.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(cpuTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            duplication->ReleaseFrame();
            continue;
        }

        AVFrame* frame = pool.acquire();
        av_frame_make_writable(frame);

        uint8_t* src[] = { (uint8_t*)mapped.pData };
        int stride[] = { (int)mapped.RowPitch };

        sws_scale(swsCtx, src, stride, 0, height,
                  frame->data, frame->linesize);

        // ✅ CORRECT REAL-TIME PTS (THIS IS THE FIX)
        auto now = std::chrono::steady_clock::now();
        frame->pts = std::chrono::duration_cast<std::chrono::microseconds>(
            now - startTime
        ).count();

        queue.push({ frame, frame->pts });

        context->Unmap(cpuTex.Get(), 0);
        duplication->ReleaseFrame();
    }

    queue.wakeAll();
    encoder.join();

    av_write_trailer(outCtx);
    sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);

    if (!(outCtx->oformat->flags & AVFMT_NOFILE))
        avio_close(outCtx->pb);

    avformat_free_context(outCtx);

    std::cout << "DONE: " << filename << "\n";
    return 0;
}