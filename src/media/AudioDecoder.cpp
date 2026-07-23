// AudioDecoder.cpp
#include "media/AudioDecoder.hpp"
#include "util/Log.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace temporal_forge {

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() { close(); }

bool AudioDecoder::open(AVFormatContext* fmt, int streamIndex,
                        int outChannels, int outSampleRate) {
    close();
    if (!fmt || streamIndex < 0) return false;
    AVStream* st = fmt->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { logError("AudioDecoder: no decoder for codec"); return false; }

    codec_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_, st->codecpar);
    codec_->pkt_timebase = st->time_base;
    AVDictionary* opts = nullptr;
    if (avcodec_open2(codec_, codec, &opts) < 0) {
        av_dict_free(&opts);
        logError("AudioDecoder: avcodec_open2 failed");
        close();
        return false;
    }
    av_dict_free(&opts);
    frame_ = av_frame_alloc();

    // Configure resampler to interleaved float stereo @ outSampleRate.
    // Use av_opt_set_chlayout to avoid the channel-layout lifetime issues that
    // swr_alloc_set_opts2 has with stack-local AVChannelLayout.
    outChannels_ = outChannels;
    outSampleRate_ = outSampleRate;
    swr_ = swr_alloc();
    if (!swr_) { logError("AudioDecoder: swr_alloc failed"); close(); return false; }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels);

    // Copy the input channel layout properly (av_channel_layout_copy handles
    // any heap-allocated internal map).
    AVChannelLayout inLayout = {};
    if (codec_->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&inLayout, &codec_->ch_layout);
    } else {
        av_channel_layout_default(&inLayout, 2);
    }

    av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
    av_opt_set_int(swr_, "out_sample_rate", outSampleRate, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_chlayout(swr_, "in_chlayout", &inLayout, 0);
    av_opt_set_int(swr_, "in_sample_rate",
                   codec_->sample_rate > 0 ? codec_->sample_rate : 48000, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", codec_->sample_fmt, 0);
    av_channel_layout_uninit(&outLayout);
    av_channel_layout_uninit(&inLayout);

    int ret = swr_init(swr_);
    if (ret < 0) {
        logError("AudioDecoder: swr_init failed (code={})", ret);
        close();
        return false;
    }

    streamIndex_ = streamIndex;
    logInfo("AudioDecoder: opened codec '{}' -> {}ch {}Hz float",
            codec->name, outChannels_, outSampleRate_);
    return true;
}

void AudioDecoder::close() {
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (swr_) { swr_free(&swr_); }
    if (codec_) { avcodec_free_context(&codec_); }
    streamIndex_ = -1;
}

int AudioDecoder::sendPacket(AVPacket* pkt) {
    if (!codec_) return 0;
    int err = avcodec_send_packet(codec_, pkt);
    return (err == 0 || err == AVERROR(EAGAIN)) ? 1 : 0;
}

bool AudioDecoder::receiveChunk(DecodedAudioChunk& out) {
    if (!codec_ || !frame_) return false;
    int err = avcodec_receive_frame(codec_, frame_);
    if (err < 0) return false;

    int nbSamples = frame_->nb_samples;
    if (nbSamples <= 0) { av_frame_unref(frame_); return false; }
    const int inCh = frame_->ch_layout.nb_channels > 0 ? frame_->ch_layout.nb_channels : outChannels_;
    const AVSampleFormat fmt = static_cast<AVSampleFormat>(frame_->format);

    // Manual format conversion to interleaved float. Avoids libswresample
    // entirely (crash on this system's swresample 62.x).
    std::vector<float> dst(static_cast<size_t>(nbSamples) * outChannels_, 0.0f);
    int got = nbSamples;

    if (fmt == AV_SAMPLE_FMT_FLTP) {
        for (int s = 0; s < nbSamples; ++s)
            for (int ch = 0; ch < outChannels_; ++ch)
                if (ch < inCh)
                    dst[s * outChannels_ + ch] = reinterpret_cast<const float*>(frame_->data[ch])[s];
    } else if (fmt == AV_SAMPLE_FMT_FLT) {
        const float* src = reinterpret_cast<const float*>(frame_->data[0]);
        for (int s = 0; s < nbSamples; ++s)
            for (int ch = 0; ch < outChannels_; ++ch)
                dst[s * outChannels_ + ch] = (ch < inCh) ? src[s * inCh + ch] : 0.0f;
    } else if (fmt == AV_SAMPLE_FMT_S16P) {
        for (int s = 0; s < nbSamples; ++s)
            for (int ch = 0; ch < outChannels_; ++ch)
                if (ch < inCh)
                    dst[s * outChannels_ + ch] = float(reinterpret_cast<const int16_t*>(frame_->data[ch])[s]) / 32768.0f;
    } else if (fmt == AV_SAMPLE_FMT_S16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(frame_->data[0]);
        for (int s = 0; s < nbSamples; ++s)
            for (int ch = 0; ch < outChannels_; ++ch)
                dst[s * outChannels_ + ch] = (ch < inCh) ? float(src[s * inCh + ch]) / 32768.0f : 0.0f;
    } else if (fmt == AV_SAMPLE_FMT_S32P) {
        for (int s = 0; s < nbSamples; ++s)
            for (int ch = 0; ch < outChannels_; ++ch)
                if (ch < inCh)
                    dst[s * outChannels_ + ch] = float(reinterpret_cast<const int32_t*>(frame_->data[ch])[s]) / 2147483648.0f;
    } else {
        // Unsupported format — emit silence (don't crash).
        logWarn("AudioDecoder: unsupported sample format {}, emitting silence", int(fmt));
    }

    out.samples = std::move(dst);
    out.channels = outChannels_;
    out.sampleRate = outSampleRate_;

    AVRational tb = codec_->pkt_timebase;
    if (tb.den == 0) tb = {1, 1};
    if (frame_->pts != AV_NOPTS_VALUE)
        out.ptsUs = av_rescale_q(frame_->pts, tb, {1, 1000000});
    out.durationUs = static_cast<double>(got) * 1e6 / outSampleRate_;

    av_frame_unref(frame_);
    return true;
}

void AudioDecoder::flush() {
    if (codec_) avcodec_flush_buffers(codec_);
    if (swr_) swr_init(swr_);
}

} // namespace temporal_forge
