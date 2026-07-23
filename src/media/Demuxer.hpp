// Demuxer.hpp — FFmpeg demuxer (libavformat).
// spec 01: "DemuxPacketQueue" feeds decode threads. Owns the AVFormatContext.
//
// Reads packets from a local file and routes them to per-stream queues.
// Identifies the best video / audio / subtitle stream indices (spec 05
// "Media Info Panel" wants these exposed).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare FFmpeg types at global scope so they resolve to ::AV*
// (not temporal_forge::AV*) regardless of where they're referenced.
struct AVFormatContext;
struct AVCodecParameters;
struct AVPacket;

namespace temporal_forge {

struct StreamInfo {
    int index = -1;
    enum class Type { Video, Audio, Subtitle, Other } type = Type::Other;
    std::string codecName;
    std::string language;
    bool isDefault = false;
    // video-only
    int width = 0;
    int height = 0;
    double frameRate = 0.0;        // computed fps (num/den) at demux time
    double sampleAspectRatio = 1.0;
    // audio-only
    int sampleRate = 0;
    int channels = 0;
    // common
    int64_t durationUs = 0;
};

struct MediaInfo {
    std::string url;
    std::string container;
    int64_t durationUs = 0;
    int64_t startTimeUs = 0;
    std::vector<StreamInfo> streams;
    int videoIndex = -1;
    int audioIndex = -1;
    int subtitleIndex = -1;
    // Convenience: the chosen streams (or nullptr if absent)
    const StreamInfo* video = nullptr;
    const StreamInfo* audio = nullptr;
    const StreamInfo* subtitle = nullptr;
};

// A single demuxed packet awaiting decode. Owning wrapper over AVPacket.
struct Packet {
    struct AVPacket* av = nullptr;
    int streamIndex = -1;
    bool isFlush = false; // reset decoder state after a seek
    bool isEof = false;   // drain delayed decoder frames at end of stream
    Packet();
    ~Packet();
    Packet(Packet&&) noexcept;
    Packet& operator=(Packet&&) noexcept;
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
};

class Demuxer {
public:
    Demuxer();
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    // open: open the file at url and probe its streams.
    //
    // Called by: PlaybackEngine::openUrl (on the UI thread, before starting the
    //            demux/decode threads).
    // Calls:     avformat_open_input, avformat_find_stream_info, fillStreamInfo.
    // Returns:   false on failure (ctx_ left null; isOpen() stays false).
    bool open(const std::string& url);

    // close: release the AVFormatContext and all resources (idempotent).
    //        Called by: PlaybackEngine::close, ::openUrl (re-open), dtor.
    void close();

    // isOpen / info / ctx: trivial accessors. info() exposes stream metadata
    //                      (used by the QML media-info panel, spec 05); ctx()
    //                      hands the raw AVFormatContext to the decoders.
    [[nodiscard]] bool isOpen() const { return ctx_ != nullptr; }
    [[nodiscard]] const MediaInfo& info() const { return info_; }
    [[nodiscard]] AVFormatContext* ctx() { return ctx_; }

    // readPacket: read the next packet into out.
    //
    // Called by: PlaybackEngine::demuxLoop (the playback/demux thread).
    // Returns:   false at EOF or when abort_ is set; true with out.isEof on the
    //            final packet. Blocking unless requestAbort() was called.
    // Notes:     out.isFlush is set on flush packets emitted after a seek.
    bool readPacket(Packet& out);

    // seekUs: seek to targetUs microseconds (stream-agnostic via AV_TIME_BASE).
    //
    // Called by: PlaybackEngine::seekUs (UI thread) before signaling the demux loop.
    // Calls:     av_seek_frame; clamps target to [0, duration].
    // Notes:     Flushes the decoder state so the next packets are post-seek.
    bool seekUs(int64_t targetUs);

    // requestAbort: ask a blocking readPacket to return promptly (cross-thread).
    //               Called by: PlaybackEngine::close / ::stopThreads from the UI thread.
    void requestAbort() { abort_ = true; }
    [[nodiscard]] bool abortRequested() const { return abort_; }

private:
    // fillStreamInfo: populate info_.streams + pick best video/audio/subtitle indices.
    //                  Called by: open. Uses avcodec_parameters + stream disposition.
    void fillStreamInfo();
    // typeName: stream-type display name (Video/Audio/Subtitle/Other).
    //           Called by: fillStreamInfo.
    static const char* typeName(StreamInfo::Type t);

    AVFormatContext* ctx_ = nullptr;
    MediaInfo info_;
    std::atomic<bool> abort_{false};
};

} // namespace temporal_forge
