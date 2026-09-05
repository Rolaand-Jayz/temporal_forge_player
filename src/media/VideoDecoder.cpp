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
#include <algorithm>

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

// Convert FFmpeg's past-reference vector at one explicit boundary. FFmpeg's
// contract is src_x = dst_x + motion_x / motion_scale, so this produces the
// displacement from the current destination block to its corresponding
// previous source block. The returned convention is current destination to
// previous source in source-pixel units; all downstream motion consumers use
// that convention without another sign or scale conversion.
static MvEntry codecMvToCurrentPrevious(const AVMotionVector& motion) {
    MvEntry entry;
    entry.dstX = motion.dst_x;
    entry.dstY = motion.dst_y;
    entry.mvX = static_cast<float>(motion.motion_x) /
               static_cast<float>(motion.motion_scale);
    entry.mvY = static_cast<float>(motion.motion_y) /
               static_cast<float>(motion.motion_scale);
    entry.w = motion.w;
    entry.h = motion.h;
    entry.source = static_cast<int8_t>(std::clamp(motion.source, -128, 127));
    return entry;
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

    // VAAPI/DRM frames remain suitable for color upload, but this handoff does
    // not preserve AV_FRAME_DATA_MOTION_VECTORS. If the caller explicitly
    // selects codec or codec_refined motion, force software decode so the
    // requested estimator receives real FFmpeg side data instead of an empty
    // seed field. The normal path stays hardware-decoded unless motion data is
    // explicitly requested.
    const char *motionMode = std::getenv("TFORGE_FSR4_MOTION_ESTIMATOR");
    // The quality runner uses MOTION_ABLATION for its named A/B arms, while
    // the standalone estimator accepts MOTION_ESTIMATOR. Keep the decoder's
    // hardware/software decision on the same precedence as the estimator
    // itself, so `refined` cannot silently lose FFmpeg motion side data.
    if (!motionMode || !*motionMode)
        motionMode = std::getenv("TFORGE_FSR4_MOTION_ABLATION");
    const bool motionMetadataRequested =
        motionMetadataRequested_ ||
        (motionMode && (std::strcmp(motionMode, "codec") == 0 ||
                        std::strcmp(motionMode, "codec_refined") == 0 ||
                        std::strcmp(motionMode, "refined") == 0));
    const bool disableHwDecode =
        std::getenv("TFORGE_DISABLE_HW_DECODE") != nullptr ||
        motionMetadataRequested;
    if (!disableHwDecode &&
        av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                               nullptr, 0) == 0 &&
        hwDeviceCtx_) {
        codec_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        hwaccelEnabled_ = true;
        logInfo("VideoDecoder: VAAPI device created");
    } else if (disableHwDecode) {
        hwaccelEnabled_ = false;
        logInfo("VideoDecoder: hardware decode disabled{}",
                motionMetadataRequested
                    ? " because codec motion metadata was requested"
                    : " by environment");
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
        logInfo("VideoDecoder: pix_fmt={} will use the software-to-YUV420 fallback",
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
    // Keep software frames even when they are not native 4:2:0. The uploader
    // normalizes supported libav pixel formats through libswscale before the
    // GPU YUV conversion pass; rejecting them here made valid videos render
    // as a permanent black frame.
    if (!decodedHwFrame &&
        (!av_pix_fmt_desc_get(decodedFmt) || !frame_->data[0])) {
        logWarn("VideoDecoder: decoded frame has no usable pixel data (format {})",
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
    out.colorTransfer = sourceFrame->color_trc != AVCOL_TRC_UNSPECIFIED
                            ? sourceFrame->color_trc
                            : codec_->color_trc;
    out.colorPrimaries = sourceFrame->color_primaries != AVCOL_PRI_UNSPECIFIED
                             ? sourceFrame->color_primaries
                             : codec_->color_primaries;
    out.chromaLocation = sourceFrame->chroma_location != AVCHROMA_LOC_UNSPECIFIED
                             ? sourceFrame->chroma_location
                             : codec_->chroma_sample_location;
    const AVPixFmtDescriptor *sourceDesc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(sourceFrame->format));
    out.bitDepth = sourceDesc && sourceDesc->comp[0].depth > 0
                       ? sourceDesc->comp[0].depth
                       : 8;
    out.hwFrame = drmFrame != nullptr;
    // Preserve coded picture type beside the motion side data. FFmpeg's
    // negative motion-vector source value identifies a past reference list,
    // but a B-picture may still refer to an older displayed picture rather
    // than the immediately previous frame owned by Temporal Forge.
    out.bFrame = sourceFrame->pict_type == AV_PICTURE_TYPE_B;
    // Estimate how many display frames one exported vector spans. B-picture
    // back references land on the nearest previous displayed frame, while a
    // P-picture's reference is the previous P/I frame, which in an I B B P
    // display group sits three display frames back. The history reprojection
    // contract needs current->previous-displayed-frame displacement, so the
    // per-group P magnitudes must be divided by this distance before use.
    switch (sourceFrame->pict_type) {
        case AV_PICTURE_TYPE_B:
            out.mvReferenceDistance = 1;
            ++displayFramesSinceRef_;
            break;
        case AV_PICTURE_TYPE_P:
            out.mvReferenceDistance = static_cast<uint8_t>(
                std::clamp(displayFramesSinceRef_ + 1, 1, 255));
            displayFramesSinceRef_ = 0;
            break;
        default:
            // I-frames export no motion vectors; restart the group count.
            out.mvReferenceDistance = 1;
            displayFramesSinceRef_ = 0;
            break;
    }
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
        const AVPixFmtDescriptor* desc = sourceDesc;
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

    // Optional hardware-analysis bridge: keep the DRM PRIME frame above as
    // the zero-copy presentation/upload surface, but make one software YUV
    // copy available to the cheap causal luma matcher. This is deliberately
    // opt-in because the transfer adds CPU/GPU synchronization; the default
    // hardware path remains unchanged until a matched quality/performance A/B
    // proves that the extra motion evidence is worth its cost.
    // A block-motion/replacement capture also needs the analysis copy. Keep
    // the zero-copy DRM surface for presentation, but do not make callers
    // remember a second hidden switch just to make the explicitly requested
    // matcher receive luma. Ordinary playback remains unchanged because all
    // three conditions are opt-in diagnostics.
    const bool hardwareAnalysisRequested =
        std::getenv("TFORGE_FSR4_ENABLE_HW_ANALYSIS_LUMA") != nullptr ||
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_BLOCK_MOTION") != nullptr ||
        std::getenv("TFORGE_FSR4_EXPERIMENTAL_REPLACE_MOTION") != nullptr;
    if (decodedHwFrame && drmFrame && hardwareAnalysisRequested) {
        AVFrame* analysisFrame = av_frame_alloc();
        if (analysisFrame) {
            analysisFrame->format = AV_PIX_FMT_YUV420P;
            analysisFrame->width = sourceFrame->width;
            analysisFrame->height = sourceFrame->height;
            if (av_hwframe_transfer_data(analysisFrame, frame_, 0) == 0 &&
                analysisFrame->data[0] && analysisFrame->linesize[0] > 0) {
                const size_t bytes = static_cast<size_t>(analysisFrame->linesize[0]) *
                                     static_cast<size_t>(analysisFrame->height);
                out.plane[0].assign(analysisFrame->data[0],
                                    analysisFrame->data[0] + bytes);
                out.linesize[0] = analysisFrame->linesize[0];
                out.planes = 1;
                logInfo("VideoDecoder: hardware analysis luma enabled {}x{} "
                        "pitch={} bytes={}", out.width, out.height,
                        out.linesize[0], bytes);
            } else {
                static bool warnedAnalysisTransfer = false;
                if (!warnedAnalysisTransfer) {
                    logWarn("VideoDecoder: hardware analysis-luma transfer "
                            "failed; retaining zero-copy frame only");
                    warnedAnalysisTransfer = true;
                }
            }
            av_frame_free(&analysisFrame);
        }
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
            MvEntry e = codecMvToCurrentPrevious(m);
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
    displayFramesSinceRef_ = 0;
}

} // namespace temporal_forge
