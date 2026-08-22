// VideoDecoder.hpp — FFmpeg video decoder.
// spec 01: "Decode Threads" handle video packet decode.
//
// Produces DecodedVideoFrame objects carrying a host-side YUV plane buffer
// plus the source PTS/duration (spec 02: these are preserved 1:1 to output).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

// FFmpeg forward declarations at global scope.
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace temporal_forge {

struct Timebase { int num = 1; int den = 1; };

// A single codec-exported motion vector (H.264/H.265), normalized to source
// pixel units. Extracted from AV_FRAME_DATA_MOTION_VECTORS side data.
// Used to synthesize the RG16F motion texture for the FSR4 prepass.
struct MvEntry {
    int16_t dstX = 0;       // destination block top-left x (source pixel space)
    int16_t dstY = 0;       // destination block top-left y
    float   mvX = 0.0f;     // motion delta x in source pixels (motion_x / motion_scale)
    float   mvY = 0.0f;     // motion delta y in source pixels
    uint8_t w = 0;          // block width
    uint8_t h = 0;          // block height
    int8_t  source = 0;     // <0 = backward (past ref), >0 = forward (future ref)
};

struct DecodedVideoFrame {
    int64_t ptsUs = 0;          // presentation timestamp, microseconds
    int64_t durationUs = 0;     // frame duration, microseconds
    int64_t ptsTicks = -1;      // raw pts in stream timebase (-1 = none)
    int64_t durationTicks = 0;
    int width = 0;
    int height = 0;
    // Packed planar YUV 4:2:0 (Y, U, V) OR interleaved for 444/RGB.
    // We keep it simple: store the AVFrame-compatible data pointers + linesize
    // copied into owned host buffers. Format is recorded for the upload step.
    int avFormat = 0;           // AVPixelFormat
    int colorRange = 0;         // AVColorRange
    int colorSpace = 0;         // AVColorSpace
    int colorTransfer = 0;      // AVColorTransferCharacteristic
    bool hwFrame = false;       // true when backed by GPU hardware surfaces
    int hwFrameFormat = -1;     // AVPixelFormat for the hardware frame, if any
    // Retains mapped DRM PRIME descriptors and their DMA-BUF file descriptors
    // until the Vulkan uploader has imported the frame.
    std::shared_ptr<AVFrame> hwFrameOwner;
    int drmObjects = 0;         // DRM PRIME object count, if hwFrame is DRM-backed
    struct DrmPlane {
        int objectIndex = -1;
        ptrdiff_t offset = 0;
        ptrdiff_t pitch = 0;
    };
    struct DrmObject {
        int fd = -1;
        size_t size = 0;
        uint64_t formatModifier = 0;
    };
    uint32_t drmFourcc = 0;
    uint32_t drmLayerFourcc[4] = {0,0,0,0};
    int drmLayerPlaneCount[4] = {0,0,0,0};
    int drmLayers = 0;
    int drmPlanes = 0;
    DrmObject drmObject[4];
    DrmPlane drmLayerPlane[4][4];
    std::vector<uint8_t> plane[4];
    int linesize[4] = {0,0,0,0};
    int planes = 0;
    bool keyframe = false;
    double frameRate = 0.0;     // nominal source fps at decode time
    uint64_t frameIndex = 0;    // monotonic decode counter

    // Codec-exported motion vectors (empty if the codec/driver doesn't provide
    // them — e.g. intra-only streams, or export disabled). These feed the
    // motion-texture synthesis in SideBufferTextures (MotionMode::Codec).
    std::vector<MvEntry> motionVectors;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // open: open the video codec for streamIndex (VAAPI when available, with a
    //       CPU fallback when the codec/profile cannot use it).
    //
    // Called by: PlaybackEngine::openUrl (after Demuxer::open identifies the
    //            video stream).
    // Calls:     avcodec_open2; sets up hwaccel via getHwPixelFormat when the
    //            VAAPI device is available.
    // Returns:   false on failure (codec_ left null).
    bool open(AVFormatContext* fmt, int streamIndex);

    // close: free the codec context + frame (idempotent).
    //        Called by: PlaybackEngine::close, ::openUrl (re-open), dtor.
    void close();

    // Trivial accessors: width/height/pixelFormat describe the decoded frames;
    // gpuFriendlyFormat reports whether the format uploads to Vulkan without a
    // CPU convert; timebase exposes the stream timebase for PTS conversion;
    // hwaccelEnabled reports whether VAAPI is active.
    [[nodiscard]] bool isOpen() const { return codec_ != nullptr; }
    [[nodiscard]] int streamIndex() const { return streamIndex_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int pixelFormat() const { return pixFmt_; }
    [[nodiscard]] bool gpuFriendlyFormat() const;
    [[nodiscard]] Timebase timebase() const;
    [[nodiscard]] bool hwaccelEnabled() const { return hwaccelEnabled_; }

    // sendPacket: feed one demuxed packet to the decoder.
    //
    // Called by: PlaybackEngine::videoDecodeLoop.
    // Returns:   number of frames produced (0 or 1 typically; B-frames may
    //            delay output until later packets).
    int sendPacket(AVPacket* pkt);

    // receiveFrame: pull the next decoded frame (with YUV planes + motion vectors).
    //
    // Called by: PlaybackEngine::videoDecodeLoop (after sendPacket).
    // Calls:     avcodec_receive_frame; extracts AV_FRAME_DATA_MOTION_VECTORS
    //            into DecodedVideoFrame::motionVectors when present.
    // Returns:   false if no frame is ready yet (caller feeds more packets).
    bool receiveFrame(DecodedVideoFrame& out);

    // flush: drain delayed frames / reset after a seek.
    //        Called by: PlaybackEngine::seekUs and ::close.
    void flush();

private:
    // getHwPixelFormat: FFmpeg hwaccel pixel-format negotiator — selects the
    //                   VAAPI/DRM-prime format when hwaccel is active.
    //                   Called by: FFmpeg via the get_format callback during open.
    static enum AVPixelFormat getHwPixelFormat(struct AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);
    // isDrmPrimeFrame: returns whether a decoded AVFrame is a DRM-prime hw frame.
    //                  Called by: receiveFrame (decides the upload path in GpuImageUploader).
    static bool isDrmPrimeFrame(const AVFrame* frame);

    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVBufferRef* hwFramesCtx_ = nullptr;
    enum AVPixelFormat hwPixFmt_ = AV_PIX_FMT_NONE;
    int streamIndex_ = -1;
    int width_ = 0, height_ = 0;
    int pixFmt_ = 0;
    uint64_t frameCounter_ = 0;
    bool hwaccelEnabled_ = false;
};

} // namespace temporal_forge
