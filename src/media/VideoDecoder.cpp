// VideoDecoder.cpp
#include "media/VideoDecoder.hpp"
#include "util/Log.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
}

#include <cstring>
#include <cstdlib>

namespace temporal_forge {

static bool isGpuFriendlyPixelFormat(AVPixelFormat fmt) {
    return fmt == AV_PIX_FMT_YUV420P ||
           fmt == AV_PIX_FMT_YUVJ420P ||
           fmt == AV_PIX_FMT_YUV420P10LE ||
           fmt == AV_PIX_FMT_YUV420P12LE;
}

static bool isHardwarePixelFormat(AVPixelFormat fmt) {
    return fmt == AV_PIX_FMT_VAAPI || fmt == AV_PIX_FMT_DRM_PRIME;
}

enum AVPixelFormat VideoDecoder::getHwPixelFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    auto* self = static_cast<VideoDecoder*>(ctx->opaque);
    // Keep compressed frames on the GPU. The uploader imports a DRM PRIME
    // mapping of VAAPI surfaces directly into Vulkan; software YUV remains the
    // compatibility fallback when the codec/profile cannot use VAAPI.
    for (const enum AVPixelFormat* p = pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_VAAPI || *p == AV_PIX_FMT_DRM_PRIME) {
            if (self) self->hwPixFmt_ = *p;
            return *p;
        }
    }
    for (const enum AVPixelFormat* p = pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        if (isGpuFriendlyPixelFormat(*p)) {
            return *p;
        }
    }
    return avcodec_default_get_format(ctx, pix_fmts);
}

bool VideoDecoder::isDrmPrimeFrame(const AVFrame* frame) {
    return frame && frame->format == AV_PIX_FMT_DRM_PRIME && frame->data[0];
}

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(AVFormatContext* fmt, int streamIndex) {
    close();
    if (!fmt || streamIndex < 0 || streamIndex >= static_cast<int>(fmt->nb_streams)) {
        logError("VideoDecoder: invalid stream index {}", streamIndex);
        return false;
    }
    AVStream* st = fmt->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        logError("VideoDecoder: no decoder for codec id {}", static_cast<int>(st->codecpar->codec_id));
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) { logError("VideoDecoder: alloc context failed"); return false; }
    if (avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
        logError("VideoDecoder: parameters_to_context failed");
        close();
        return false;
    }
    codec_->pkt_timebase = st->time_base;
    // Export codec motion vectors (H.264/H.265) as AV_FRAME_DATA_MOTION_VECTORS
    // side data. Required before avcodec_open2. Used by SideBufferTextures
    // (MotionMode::Codec) to build the RG16F motion texture for the FSR4 prepass.
    codec_->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    codec_->opaque = this;
    codec_->get_format = &VideoDecoder::getHwPixelFormat;

    const bool disableHwDecode = std::getenv("TFORGE_DISABLE_HW_DECODE") != nullptr;
    if (!disableHwDecode &&
        av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                               nullptr, 0) == 0 &&
        hwDeviceCtx_) {
        codec_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        hwaccelEnabled_ = true;
        logInfo("VideoDecoder: VAAPI device created");
    } else if (disableHwDecode) {
        hwaccelEnabled_ = false;
        logInfo("VideoDecoder: hardware decode disabled by environment");
    } else {
        hwaccelEnabled_ = false;
        logWarn("VideoDecoder: VAAPI device creation failed; continuing with software decode");
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "threads", "auto", 0);
    // Low-latency-friendly but keep correct PTS handling.
    if (avcodec_open2(codec_, codec, &opts) < 0) {
        logError("VideoDecoder: avcodec_open2 failed");
        av_dict_free(&opts);
        close();
        return false;
    }
    av_dict_free(&opts);
    frame_ = av_frame_alloc();
    streamIndex_ = streamIndex;
    width_ = codec_->width;
    height_ = codec_->height;
    pixFmt_ = codec_->pix_fmt;
    frameCounter_ = 0;
    logInfo("VideoDecoder: opened codec '{}' {}x{} pix_fmt={}",
            codec->name, width_, height_, av_get_pix_fmt_name(static_cast<AVPixelFormat>(pixFmt_)));
    if (!gpuFriendlyFormat()) {
        logWarn("VideoDecoder: pix_fmt={} is not GPU-friendly for the current upload path",
                av_get_pix_fmt_name(static_cast<AVPixelFormat>(pixFmt_)));
    }
    return true;
}

void VideoDecoder::close() {
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (hwFramesCtx_) { av_buffer_unref(&hwFramesCtx_); hwFramesCtx_ = nullptr; }
    if (hwDeviceCtx_) { av_buffer_unref(&hwDeviceCtx_); hwDeviceCtx_ = nullptr; }
    if (codec_) { avcodec_free_context(&codec_); }
    streamIndex_ = -1;
    width_ = height_ = 0;
    pixFmt_ = 0;
    frameCounter_ = 0;
    hwPixFmt_ = AV_PIX_FMT_NONE;
    hwaccelEnabled_ = false;
}

Timebase VideoDecoder::timebase() const {
    if (!codec_) return {1, 1};
    return {static_cast<int>(codec_->pkt_timebase.num),
            static_cast<int>(codec_->pkt_timebase.den)};
}

int VideoDecoder::sendPacket(AVPacket* pkt) {
    if (!codec_) return 0;
    int err = avcodec_send_packet(codec_, pkt);
    if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        char buf[128] = {0}; av_strerror(err, buf, sizeof(buf));
        logWarn("VideoDecoder: send_packet error: {}", buf);
    }
    return (err == 0 || err == AVERROR(EAGAIN)) ? 1 : 0;
}

bool VideoDecoder::receiveFrame(DecodedVideoFrame& out) {
    if (!codec_ || !frame_) return false;
    out.hwFrameOwner.reset();
    out.hwFrame = false;
    out.hwFrameFormat = -1;
    out.drmObjects = 0;
    out.drmLayers = 0;
    out.drmPlanes = 0;
    out.drmFourcc = 0;
    for (auto& object : out.drmObject) object = {};
    std::memset(out.drmLayerFourcc, 0, sizeof(out.drmLayerFourcc));
    std::memset(out.drmLayerPlaneCount, 0,
                sizeof(out.drmLayerPlaneCount));
    for (auto& layer : out.drmLayerPlane)
        for (auto& plane : layer) plane = {};
    int err = avcodec_receive_frame(codec_, frame_);
    if (err < 0) return false; // EAGAIN or EOF

    const AVPixelFormat decodedFmt = static_cast<AVPixelFormat>(frame_->format);
    const bool decodedHwFrame = isHardwarePixelFormat(decodedFmt) || isDrmPrimeFrame(frame_);
    if (!decodedHwFrame && !isGpuFriendlyPixelFormat(decodedFmt)) {
        logWarn("VideoDecoder: rejecting unsupported frame format {} on GPU-only path",
                av_get_pix_fmt_name(decodedFmt));
        av_frame_unref(frame_);
        return false;
    }

    AVFrame* sourceFrame = frame_;
    AVFrame* transferredFrame = nullptr;
    AVFrame* drmFrame = nullptr;
    if (decodedHwFrame) {
        if (isDrmPrimeFrame(frame_)) {
            drmFrame = av_frame_clone(frame_);
        } else {
            drmFrame = av_frame_alloc();
            if (drmFrame) {
                drmFrame->width = frame_->width;
                drmFrame->height = frame_->height;
                drmFrame->format = AV_PIX_FMT_DRM_PRIME;
                drmFrame->hw_frames_ctx = av_buffer_ref(frame_->hw_frames_ctx);
                const int mapFlags = AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT;
                if (av_hwframe_map(drmFrame, frame_, mapFlags) < 0) {
                    av_frame_free(&drmFrame);
                } else {
                    av_frame_copy_props(drmFrame, frame_);
                }
            }
        }
        if (!drmFrame) {
            transferredFrame = av_frame_alloc();
            if (!transferredFrame) {
                logWarn("VideoDecoder: failed to allocate transfer frame for hw decode");
                av_frame_unref(frame_);
                return false;
            }
            if (av_hwframe_transfer_data(transferredFrame, frame_, 0) < 0) {
                logWarn("VideoDecoder: direct DRM map and software transfer failed");
                av_frame_free(&transferredFrame);
                av_frame_unref(frame_);
                return false;
            }
            transferredFrame->pts = frame_->pts;
            transferredFrame->best_effort_timestamp = frame_->best_effort_timestamp;
            transferredFrame->duration = frame_->duration;
            av_frame_copy_props(transferredFrame, frame_);
            sourceFrame = transferredFrame;
        }
    }

    out.width = sourceFrame->width;
    out.height = sourceFrame->height;
    out.avFormat = drmFrame ? AV_PIX_FMT_DRM_PRIME : sourceFrame->format;
    out.colorRange = sourceFrame->color_range != AVCOL_RANGE_UNSPECIFIED
                         ? sourceFrame->color_range
                         : codec_->color_range;
    out.colorSpace = sourceFrame->colorspace != AVCOL_SPC_UNSPECIFIED
                         ? sourceFrame->colorspace
                         : codec_->colorspace;
    out.hwFrame = drmFrame != nullptr;
    out.hwFrameFormat = out.hwFrame ? AV_PIX_FMT_DRM_PRIME : -1;
    out.drmObjects = 0;
    out.keyframe = (sourceFrame->flags & AV_FRAME_FLAG_KEY) != 0;
    out.frameIndex = frameCounter_++;
    out.ptsTicks = sourceFrame->pts != AV_NOPTS_VALUE ? sourceFrame->pts : -1;
    out.durationTicks = sourceFrame->duration;

    // Convert PTS/duration to microseconds using the stream timebase.
    AVRational tb = codec_->pkt_timebase;
    if (tb.den == 0) tb = {1, 1};
    if (sourceFrame->pts != AV_NOPTS_VALUE) {
        out.ptsUs = av_rescale_q(sourceFrame->pts, tb, {1, 1000000});
    } else if (sourceFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
        out.ptsUs = av_rescale_q(sourceFrame->best_effort_timestamp, tb, {1, 1000000});
    }
    if (sourceFrame->duration > 0) {
        out.durationUs = av_rescale_q(sourceFrame->duration, tb, {1, 1000000});
    }
    // Fallback duration from frame rate if missing (spec 02 VFR section:
    // compute frameTimeMs from timestamp delta; for a single frame use fps).
    if (out.durationUs <= 0 && codec_->framerate.den) {
        out.durationUs = av_rescale_q(1, av_inv_q(codec_->framerate), {1, 1000000});
    }

    if (!decodedHwFrame || !out.hwFrame) {
        // Copy each plane into owned host memory so the frame can outlive the
        // AVFrame reuse. This is the CPU-decode fallback path.
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(sourceFrame->format));
        out.planes = 0;
        if (desc) {
            int nb = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(sourceFrame->format));
            if (nb <= 0) nb = 1;
            out.planes = std::min(nb, 4);
        }
        for (int i = 0; i < out.planes; ++i) {
            int planeH = out.height;
            if (desc && i > 0) planeH = AV_CEIL_RSHIFT(out.height, desc->log2_chroma_h);
            int ls = sourceFrame->linesize[i];
            size_t bytes = static_cast<size_t>(ls) * planeH;
            out.linesize[i] = ls;
            out.plane[i].assign(sourceFrame->data[i], sourceFrame->data[i] + bytes);
        }
    } else if (drmFrame) {
        const auto* drm = reinterpret_cast<const AVDRMFrameDescriptor*>(drmFrame->data[0]);
        out.drmObjects = drm ? std::min(drm->nb_objects, 4) : 0;
        out.drmLayers = drm ? std::min(drm->nb_layers, 4) : 0;
        if (drm) {
            for (int i = 0; i < out.drmObjects; ++i) {
                out.drmObject[i].fd = drm->objects[i].fd;
                out.drmObject[i].size = drm->objects[i].size;
                out.drmObject[i].formatModifier = drm->objects[i].format_modifier;
            }
            for (int l = 0; l < out.drmLayers; ++l) {
                out.drmLayerFourcc[l] = drm->layers[l].format;
                out.drmLayerPlaneCount[l] =
                    std::min(drm->layers[l].nb_planes, 4);
                if (l == 0) {
                    out.drmFourcc = out.drmLayerFourcc[l];
                    out.drmPlanes = out.drmLayerPlaneCount[l];
                }
                for (int p = 0; p < out.drmLayerPlaneCount[l]; ++p) {
                    out.drmLayerPlane[l][p].objectIndex = drm->layers[l].planes[p].object_index;
                    out.drmLayerPlane[l][p].offset = drm->layers[l].planes[p].offset;
                    out.drmLayerPlane[l][p].pitch = drm->layers[l].planes[p].pitch;
                }
            }
            if (out.frameIndex == 0) {
                logInfo("VideoDecoder: DRM PRIME map objects={} layers={} "
                        "planes={} fourcc=[0x{:08x},0x{:08x}] modifier=0x{:x}",
                        out.drmObjects, out.drmLayers, out.drmPlanes,
                        out.drmLayerFourcc[0], out.drmLayerFourcc[1],
                        out.drmObjects > 0 ? out.drmObject[0].formatModifier
                                           : 0u);
            }
        }
        out.planes = 0;
    }

    // Extract codec motion vectors (H.264/H.265) into the frame. Each
    // AVMotionVector is normalized to source-pixel motion deltas. Empty for
    // intra-only codecs or when MV export is unsupported — the motion-texture
    // synth falls back to block-match or zero mode in that case.
    out.motionVectors.clear();
    AVFrameSideData* sd = av_frame_get_side_data(frame_, AV_FRAME_DATA_MOTION_VECTORS);
    if (sd && sd->data && sd->size >= sizeof(AVMotionVector)) {
        const size_t count = sd->size / sizeof(AVMotionVector);
        const AVMotionVector* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);
        out.motionVectors.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& m = mvs[i];
            if (m.motion_scale == 0) continue; // guard against div-by-zero
            MvEntry e;
            e.dstX = m.dst_x;
            e.dstY = m.dst_y;
            e.mvX  = static_cast<float>(m.motion_x) / static_cast<float>(m.motion_scale);
            e.mvY  = static_cast<float>(m.motion_y) / static_cast<float>(m.motion_scale);
            e.w    = m.w;
            e.h    = m.h;
            e.source = static_cast<int8_t>(m.source < 0 ? -1 : (m.source > 0 ? 1 : 0));
            out.motionVectors.push_back(e);
        }
    }

    if (drmFrame) {
        out.hwFrameOwner = std::shared_ptr<AVFrame>(drmFrame, [](AVFrame* owned) {
            av_frame_free(&owned);
        });
    }
    if (transferredFrame) av_frame_free(&transferredFrame);
    av_frame_unref(frame_);
    return true;
}

bool VideoDecoder::gpuFriendlyFormat() const {
    return isGpuFriendlyPixelFormat(static_cast<AVPixelFormat>(pixFmt_));
}

void VideoDecoder::flush() {
    if (codec_) avcodec_flush_buffers(codec_);
    frameCounter_ = 0;
}

} // namespace temporal_forge
