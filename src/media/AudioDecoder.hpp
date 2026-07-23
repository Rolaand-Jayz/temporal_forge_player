// AudioDecoder.hpp — FFmpeg audio decoder producing interleaved float samples.
// spec 01: audio is decoded on a decode thread and fed to the AudioSink.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// FFmpeg forward declarations at global scope.
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace temporal_forge {

struct DecodedAudioChunk {
    std::vector<float> samples;  // interleaved float, -1..1
    int channels = 2;
    int sampleRate = 48000;
    int64_t ptsUs = 0;           // pts of first sample
    double durationUs = 0.0;
};

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // open: open the audio codec for streamIndex of the given AVFormatContext.
    //
    // Called by: PlaybackEngine::openUrl (after Demuxer::open identifies the
    //            audio stream).
    // Calls:     avcodec_open2, swr_alloc_set_opts (resampler to outChannels/
    //            outSampleRate stereo float @ 48k by default for clock stability).
    // Returns:   false on failure (codec_ left null).
    bool open(AVFormatContext* fmt, int streamIndex,
              int outChannels = 2, int outSampleRate = 48000);

    // close: free the codec context, frame, and resampler (idempotent).
    //        Called by: PlaybackEngine::close, ::openUrl (re-open), dtor.
    void close();

    // isOpen / streamIndex / outChannels / outSampleRate: trivial accessors.
    [[nodiscard]] bool isOpen() const { return codec_ != nullptr; }
    [[nodiscard]] int streamIndex() const { return streamIndex_; }
    [[nodiscard]] int outChannels() const { return outChannels_; }
    [[nodiscard]] int outSampleRate() const { return outSampleRate_; }

    // sendPacket: feed one demuxed packet to the decoder (may produce 0+ frames).
    //             Called by: PlaybackEngine::audioDecodeLoop. Returns FFmpeg error
    //             codes (negative) or 0/EAGAIN semantics.
    int sendPacket(AVPacket* pkt);

    // receiveChunk: pull the next decoded chunk, resampled to interleaved float.
    //               Called by: PlaybackEngine::audioDecodeLoop (after sendPacket).
    // Calls:        avcodec_receive_frame, swr_convert.
    // Returns:      false if no frame is ready yet (caller feeds more packets).
    bool receiveChunk(DecodedAudioChunk& out);

    // flush: drain delayed decoder frames / reset after a seek.
    //        Called by: PlaybackEngine::seekUs and ::close.
    void flush();

private:
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwrContext* swr_ = nullptr;
    int streamIndex_ = -1;
    int outChannels_ = 2;
    int outSampleRate_ = 48000;
};

} // namespace temporal_forge
