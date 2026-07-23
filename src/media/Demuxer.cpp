// Demuxer.cpp
#include "media/Demuxer.hpp"
#include "util/Log.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <cstring>

namespace temporal_forge {

// --- Packet ---
Packet::Packet() {
    av = av_packet_alloc();
}
Packet::~Packet() {
    if (av) av_packet_free(&av);
}
Packet::Packet(Packet&& o) noexcept
    : av(o.av), streamIndex(o.streamIndex), isFlush(o.isFlush), isEof(o.isEof) {
    o.av = nullptr;
}
Packet& Packet::operator=(Packet&& o) noexcept {
    if (this != &o) {
        if (av) av_packet_free(&av);
        av = o.av;
        streamIndex = o.streamIndex;
        isFlush = o.isFlush;
        isEof = o.isEof;
        o.av = nullptr;
    }
    return *this;
}

// --- Demuxer ---
Demuxer::Demuxer() = default;

Demuxer::~Demuxer() { close(); }

const char* Demuxer::typeName(StreamInfo::Type t) {
    switch (t) {
        case StreamInfo::Type::Video:    return "video";
        case StreamInfo::Type::Audio:    return "audio";
        case StreamInfo::Type::Subtitle: return "subtitle";
        case StreamInfo::Type::Other:    return "other";
    }
    return "?";
}

static StreamInfo::Type avTypeToInfo(int t) {
    switch (t) {
        case AVMEDIA_TYPE_VIDEO:    return StreamInfo::Type::Video;
        case AVMEDIA_TYPE_AUDIO:    return StreamInfo::Type::Audio;
        case AVMEDIA_TYPE_SUBTITLE: return StreamInfo::Type::Subtitle;
        default:                    return StreamInfo::Type::Other;
    }
}

void Demuxer::fillStreamInfo() {
    info_.streams.clear();
    info_.streams.reserve(ctx_->nb_streams);
    for (unsigned i = 0; i < ctx_->nb_streams; ++i) {
        AVStream* st = ctx_->streams[i];
        AVCodecParameters* cp = st->codecpar;
        StreamInfo s{};
        s.index = static_cast<int>(i);
        s.type = avTypeToInfo(cp->codec_type);
        const AVCodecDescriptor* desc = avcodec_descriptor_get(cp->codec_id);
        s.codecName = desc ? desc->name : "unknown";

        // Language tag (common for audio/subtitle).
        if (AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0))
            s.language = lang->value;
        if (AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0))
            (void)title;
        s.isDefault = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;

        if (s.type == StreamInfo::Type::Video) {
            s.width = cp->width;
            s.height = cp->height;
            if (st->avg_frame_rate.den)
                s.frameRate = static_cast<double>(st->avg_frame_rate.num) /
                              static_cast<double>(st->avg_frame_rate.den);
            if (st->sample_aspect_ratio.den)
                s.sampleAspectRatio = static_cast<double>(st->sample_aspect_ratio.num) /
                                      static_cast<double>(st->sample_aspect_ratio.den);
        } else if (s.type == StreamInfo::Type::Audio) {
            s.sampleRate = cp->sample_rate;
            s.channels = cp->ch_layout.nb_channels;
        }
        if (st->duration != AV_NOPTS_VALUE && st->time_base.den) {
            s.durationUs = av_rescale_q(st->duration, st->time_base, {1, 1000000});
        }
        info_.streams.push_back(s);
    }

    // Pick best streams.
    info_.videoIndex = av_find_best_stream(ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    info_.audioIndex = av_find_best_stream(ctx_, AVMEDIA_TYPE_AUDIO, -1, info_.videoIndex, nullptr, 0);
    info_.subtitleIndex = av_find_best_stream(ctx_, AVMEDIA_TYPE_SUBTITLE, -1,
                                              info_.audioIndex >= 0 ? info_.audioIndex : info_.videoIndex,
                                              nullptr, 0);
    auto find = [&](int idx) -> const StreamInfo* {
        if (idx < 0) return nullptr;
        for (const auto& s : info_.streams) if (s.index == idx) return &s;
        return nullptr;
    };
    info_.video = find(info_.videoIndex);
    info_.audio = find(info_.audioIndex);
    info_.subtitle = find(info_.subtitleIndex);
}

bool Demuxer::open(const std::string& url) {
    close();
    abort_ = false;
    info_ = {};

    if (avformat_open_input(&ctx_, url.c_str(), nullptr, nullptr) < 0 || !ctx_) {
        logError("Demuxer: avformat_open_input failed for '{}'", url);
        return false;
    }
    if (avformat_find_stream_info(ctx_, nullptr) < 0) {
        logError("Demuxer: avformat_find_stream_info failed");
        return false;
    }
    info_.url = url;
    info_.container = ctx_->iformat ? (ctx_->iformat->name ? ctx_->iformat->name : "?") : "?";
    if (ctx_->duration != AV_NOPTS_VALUE)
        info_.durationUs = av_rescale_q(ctx_->duration, {1, AV_TIME_BASE}, {1, 1000000});
    if (ctx_->start_time != AV_NOPTS_VALUE)
        info_.startTimeUs = av_rescale_q(ctx_->start_time, {1, AV_TIME_BASE}, {1, 1000000});

    fillStreamInfo();

    logInfo("Demuxer: opened '{}' [{}] duration={:.3f}s streams={} video={} audio={} sub={}",
            url, info_.container, info_.durationUs / 1e6,
            ctx_->nb_streams, info_.videoIndex, info_.audioIndex, info_.subtitleIndex);
    if (info_.video)
        logDebug("Demuxer: video {}x{} {} fps", info_.video->width, info_.video->height,
                 info_.video->frameRate);
    return true;
}

void Demuxer::close() {
    if (ctx_) {
        avformat_close_input(&ctx_);
        ctx_ = nullptr;
    }
    info_ = {};
    abort_ = false;
}

bool Demuxer::readPacket(Packet& out) {
    if (!ctx_) return false;
    if (abort_) return false;
    int err = av_read_frame(ctx_, out.av);
    if (err < 0) {
        if (err == AVERROR_EOF) return false;
        if (abort_) return false;
        char errbuf[128] = {0};
        av_strerror(err, errbuf, sizeof(errbuf));
        logWarn("Demuxer: av_read_frame error: {}", errbuf);
        return false;
    }
    out.streamIndex = out.av->stream_index;
    return true;
}

bool Demuxer::seekUs(int64_t targetUs) {
    if (!ctx_) return false;
    targetUs = std::max<int64_t>(0, std::min(targetUs, info_.durationUs));
    // Seek on AV_TIME_BASE (stream-agnostic).
    int64_t ts = av_rescale(targetUs, AV_TIME_BASE, 1000000) + ctx_->start_time;
    int flags = AVSEEK_FLAG_BACKWARD;
    int err = av_seek_frame(ctx_, -1, ts, flags);
    if (err < 0) {
        char errbuf[128] = {0};
        av_strerror(err, errbuf, sizeof(errbuf));
        logWarn("Demuxer: seek to {:.3f}s failed: {}", targetUs / 1e6, errbuf);
        return false;
    }
    return true;
}

} // namespace temporal_forge
