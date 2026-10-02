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
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}

using Microsoft::WRL::ComPtr;

std::atomic<bool> stopRecording(false);

void signalHandler(int)
{
    stopRecording = true;
}

std::string getFilename()
{
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
    localtime_s(&tm, &timeT);

    std::ostringstream oss;
    oss << "dxgi_output_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << ".mp4";

    return oss.str();
}

struct FrameItem
{
    AVFrame* frame;
    int64_t pts;
};

class FrameQueue
{
public:
    FrameQueue(size_t maxSize = 120)
        : maxSize(maxSize)
    {
    }

    void push(FrameItem item)
    {
        std::unique_lock<std::mutex> lock(mtx);

        if (q.size() >= maxSize)
        {
            AVFrame* old = q.front().frame;
            q.pop();

            if (old)
                av_frame_free(&old);
        }

        q.push(item);
        cv.notify_one();
    }

    bool pop(FrameItem& item)
    {
        std::unique_lock<std::mutex> lock(mtx);

        while (q.empty() && !stopRecording.load())
            cv.wait(lock);

        if (q.empty())
            return false;

        item = q.front();
        q.pop();

        return true;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return q.empty();
    }

    void wakeAll()
    {
        cv.notify_all();
    }

private:
    std::queue<FrameItem> q;
    std::mutex mtx;
    std::condition_variable cv;
    size_t maxSize;
};

class FramePool
{
public:
    FramePool(
        int size,
        int width,
        int height,
        AVPixelFormat fmt)
        : width(width),
          height(height),
          fmt(fmt)
    {
        for (int i = 0; i < size; ++i)
            freeFrames.push(create());
    }

    ~FramePool()
    {
        while (!freeFrames.empty())
        {
            av_frame_free(&freeFrames.front());
            freeFrames.pop();
        }
    }

    AVFrame* acquire()
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (freeFrames.empty())
            return create();

        AVFrame* f = freeFrames.front();
        freeFrames.pop();

        return f;
    }

    void release(AVFrame* f)
    {
        if (!f)
            return;

        f->pts = 0;

        std::lock_guard<std::mutex> lock(mtx);
        freeFrames.push(f);
    }

private:
    AVFrame* create()
    {
        AVFrame* f = av_frame_alloc();

        f->format = fmt;
        f->width = width;
        f->height = height;

        av_frame_get_buffer(f, 32);

        return f;
    }

    int width;
    int height;
    AVPixelFormat fmt;

    std::queue<AVFrame*> freeFrames;
    std::mutex mtx;
};

static inline uint8_t clampByte(int value)
{
    if (value < 0)
        return 0;

    if (value > 255)
        return 255;

    return static_cast<uint8_t>(value);
}

void convertBGRAtoYUV420P(
    const uint8_t* src,
    int srcStride,
    AVFrame* frame,
    int width,
    int height)
{
    uint8_t* yPlane = frame->data[0];
    uint8_t* uPlane = frame->data[1];
    uint8_t* vPlane = frame->data[2];

    int yStride = frame->linesize[0];
    int uStride = frame->linesize[1];
    int vStride = frame->linesize[2];

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row =
            src + y * srcStride;

        uint8_t* yOut =
            yPlane + y * yStride;

        for (int x = 0; x < width; ++x)
        {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];

            int Y =
                ((66 * r +
                  129 * g +
                  25 * b +
                  128) >> 8) + 16;

            yOut[x] = clampByte(Y);
        }
    }

    for (int y = 0; y < height; y += 2)
    {
        const uint8_t* row0 =
            src + y * srcStride;

        const uint8_t* row1 =
            src + (y + 1) * srcStride;

        uint8_t* uOut =
            uPlane + (y / 2) * uStride;

        uint8_t* vOut =
            vPlane + (y / 2) * vStride;

        for (int x = 0; x < width; x += 2)
        {
            int rSum = 0;
            int gSum = 0;
            int bSum = 0;

            for (int dy = 0; dy < 2; ++dy)
            {
                const uint8_t* row =
                    (dy == 0) ? row0 : row1;

                for (int dx = 0; dx < 2; ++dx)
                {
                    int px = x + dx;

                    int b = row[px * 4 + 0];
                    int g = row[px * 4 + 1];
                    int r = row[px * 4 + 2];

                    rSum += r;
                    gSum += g;
                    bSum += b;
                }
            }

            int r = rSum / 4;
            int g = gSum / 4;
            int b = bSum / 4;

            int U =
                ((-38 * r -
                  74 * g +
                  112 * b +
                  128) >> 8) + 128;

            int V =
                ((112 * r -
                  94 * g -
                  18 * b +
                  128) >> 8) + 128;

            uOut[x / 2] =
                clampByte(U);

            vOut[x / 2] =
                clampByte(V);
        }
    }
}

int main()
{
    signal(SIGINT, signalHandler);

    const int width = 1600;
    const int height = 900;

    const int fps = 60;

    av_log_set_level(AV_LOG_ERROR);

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
    {
        return -1;
    }

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> duplication;

    ComPtr<IDXGIDevice> dxgiDevice;

    device.As(&dxgiDevice);

    dxgiDevice->GetAdapter(&adapter);
    adapter->EnumOutputs(0, &output);
    output.As(&output1);

    if (FAILED(output1->DuplicateOutput(
        device.Get(),
        &duplication)))
    {
        return -1;
    }

    std::string filename = getFilename();

    AVFormatContext* outCtx = nullptr;

    if (avformat_alloc_output_context2(
        &outCtx,
        nullptr,
        "mp4",
        filename.c_str()) < 0)
    {
        return -1;
    }

    const AVCodec* codec =
        avcodec_find_encoder_by_name("h264_nvenc");

    if (!codec)
        return -1;

    AVCodecContext* codecCtx =
        avcodec_alloc_context3(codec);

    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    codecCtx->bit_rate = 18 * 1000 * 1000;

    codecCtx->gop_size = 120;

    codecCtx->max_b_frames = 0;

    /*
        CHANGED:
        Use millisecond timestamps so PTS represents
        actual elapsed recording time.
    */
    codecCtx->time_base = {1, 1000};

    codecCtx->framerate = {fps, 1};

    codecCtx->thread_count = 4;

    if (outCtx->oformat->flags &
        AVFMT_GLOBALHEADER)
    {
        codecCtx->flags |=
            AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVStream* stream =
        avformat_new_stream(outCtx, codec);

    stream->time_base =
        codecCtx->time_base;

    stream->avg_frame_rate =
        {fps, 1};

    stream->r_frame_rate =
        {fps, 1};

    if (avcodec_open2(
        codecCtx,
        codec,
        nullptr) < 0)
    {
        return -1;
    }

    if (avcodec_parameters_from_context(
        stream->codecpar,
        codecCtx) < 0)
    {
        return -1;
    }

    if (!(outCtx->oformat->flags &
          AVFMT_NOFILE))
    {
        if (avio_open(
            &outCtx->pb,
            filename.c_str(),
            AVIO_FLAG_WRITE) < 0)
        {
            return -1;
        }
    }

    if (avformat_write_header(
        outCtx,
        nullptr) < 0)
    {
        return -1;
    }

    FrameQueue queue(120);

    FramePool pool(
        300,
        width,
        height,
        codecCtx->pix_fmt);

    D3D11_TEXTURE2D_DESC desc{};

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;

    desc.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;

    desc.SampleDesc.Count = 1;

    desc.Usage =
        D3D11_USAGE_STAGING;

    desc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> cpuTex;

    if (FAILED(device->CreateTexture2D(
        &desc,
        nullptr,
        &cpuTex)))
    {
        return -1;
    }

    std::thread encoder([&]()
    {
        AVPacket* pkt =
            av_packet_alloc();

        FrameItem item;

        while (!stopRecording.load() ||
               !queue.empty())
        {
            if (!queue.pop(item))
                continue;

            avcodec_send_frame(
                codecCtx,
                item.frame);

            pool.release(item.frame);

            while (avcodec_receive_packet(
                codecCtx,
                pkt) == 0)
            {
                pkt->stream_index =
                    stream->index;

                av_packet_rescale_ts(
                    pkt,
                    codecCtx->time_base,
                    stream->time_base);

                av_interleaved_write_frame(
                    outCtx,
                    pkt);

                av_packet_unref(pkt);
            }
        }

        avcodec_send_frame(
            codecCtx,
            nullptr);

        while (avcodec_receive_packet(
            codecCtx,
            pkt) == 0)
        {
            pkt->stream_index =
                stream->index;

            av_packet_rescale_ts(
                pkt,
                codecCtx->time_base,
                stream->time_base);

            av_interleaved_write_frame(
                outCtx,
                pkt);

            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
    });

    /*
        CHANGED:
        Use the real elapsed recording time for PTS.
    */
    auto recordingStart =
        std::chrono::steady_clock::now();

    int64_t lastPts = -1;

    const auto frameDuration =
        std::chrono::microseconds(
            1000000 / fps);

    auto nextFrameTime =
        recordingStart;

    while (!stopRecording.load())
    {
        std::this_thread::sleep_until(
            nextFrameTime);

        DXGI_OUTDUPL_FRAME_INFO info{};

        ComPtr<IDXGIResource> res;

        HRESULT hr =
            duplication->AcquireNextFrame(
                1,
                &info,
                &res);

        if (FAILED(hr))
        {
            nextFrameTime =
                std::chrono::steady_clock::now()
                + frameDuration;

            continue;
        }

        ComPtr<ID3D11Texture2D> tex;

        res.As(&tex);

        context->CopyResource(
            cpuTex.Get(),
            tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};

        if (FAILED(context->Map(
            cpuTex.Get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped)))
        {
            duplication->ReleaseFrame();

            nextFrameTime =
                std::chrono::steady_clock::now()
                + frameDuration;

            continue;
        }

        AVFrame* frame =
            pool.acquire();

        av_frame_make_writable(frame);

        convertBGRAtoYUV420P(
            static_cast<const uint8_t*>(
                mapped.pData),
            static_cast<int>(
                mapped.RowPitch),
            frame,
            width,
            height);

        /*
            CHANGED:
            Timestamp this frame from actual elapsed
            wall-clock recording time.
        */
        auto now =
            std::chrono::steady_clock::now();

        int64_t pts =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    now - recordingStart).count();

        if (pts <= lastPts)
            pts = lastPts + 1;

        frame->pts = pts;
        lastPts = pts;

        queue.push({
            frame,
            frame->pts
        });

        context->Unmap(
            cpuTex.Get(),
            0);

        duplication->ReleaseFrame();

        nextFrameTime += frameDuration;

        if (nextFrameTime <= now)
        {
            nextFrameTime =
                now + frameDuration;
        }
    }

    queue.wakeAll();

    encoder.join();

    av_write_trailer(outCtx);

    avcodec_free_context(
        &codecCtx);

    if (!(outCtx->oformat->flags &
          AVFMT_NOFILE))
    {
        avio_close(outCtx->pb);
    }

    avformat_free_context(outCtx);

    std::cout << "DONE: "
              << filename
              << "\n";

    return 0;
}