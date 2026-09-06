// inspect_ffmpeg_mvs.cpp — print raw FFmpeg motion-vector side data.
//
// This diagnostic stays outside playback. It answers whether a controlled
// clip's source/destination displacement is already wrong in AVFrame side data
// or becomes wrong in Temporal Forge's upload/expansion path.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/motion_vector.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s VIDEO\n", argv[0]);
    return 2;
  }
  AVFormatContext *format = nullptr;
  if (avformat_open_input(&format, argv[1], nullptr, nullptr) < 0 ||
      avformat_find_stream_info(format, nullptr) < 0) {
    std::fprintf(stderr, "could not open %s\n", argv[1]);
    avformat_close_input(&format);
    return 1;
  }
  const int streamIndex = av_find_best_stream(
      format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (streamIndex < 0) {
    avformat_close_input(&format);
    return 1;
  }
  const AVCodecParameters *params = format->streams[streamIndex]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(params->codec_id);
  AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!context || avcodec_parameters_to_context(context, params) < 0) {
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return 1;
  }
  context->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
  if (avcodec_open2(context, codec, nullptr) < 0) {
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return 1;
  }

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int frameNumber = 0;
  while (packet && frame && av_read_frame(format, packet) >= 0 &&
         frameNumber < 8) {
    if (packet->stream_index == streamIndex &&
        avcodec_send_packet(context, packet) >= 0) {
      while (avcodec_receive_frame(context, frame) >= 0 && frameNumber < 8) {
        const AVFrameSideData *side = av_frame_get_side_data(
            frame, AV_FRAME_DATA_MOTION_VECTORS);
        std::printf("frame=%d type=%c dims=%dx%d", frameNumber,
                    frame->pict_type == AV_PICTURE_TYPE_I ? 'I' :
                    frame->pict_type == AV_PICTURE_TYPE_P ? 'P' : 'B',
                    frame->width, frame->height);
        if (!side || side->size < sizeof(AVMotionVector)) {
          std::printf(" vectors=0\n");
        } else {
          const auto *vectors = reinterpret_cast<const AVMotionVector *>(side->data);
          const size_t count = side->size / sizeof(AVMotionVector);
          std::printf(" vectors=%zu\n", count);
          // Print every non-zero vector first. The first entries are commonly
          // static border blocks, which can hide the moving-object evidence
          // needed to distinguish FFmpeg extraction from GPU expansion.
          size_t printed = 0;
          for (size_t i = 0; i < count && printed < 24; ++i) {
            const auto &mv = vectors[i];
            const double dx = mv.motion_scale
                                  ? static_cast<double>(mv.motion_x) /
                                        mv.motion_scale
                                  : 0.0;
            const double dy = mv.motion_scale
                                  ? static_cast<double>(mv.motion_y) /
                                        mv.motion_scale
                                  : 0.0;
            if (dx == 0.0 && dy == 0.0)
              continue;
            std::printf("  i=%zu dst=(%d,%d) src=(%d,%d) block=%ux%u "
                        "raw=(%d,%d) scale=%u delta=(%.3f,%.3f) source=%d\n",
                        i, mv.dst_x, mv.dst_y, mv.src_x, mv.src_y, mv.w, mv.h,
                        mv.motion_x, mv.motion_y, mv.motion_scale, dx, dy,
                        mv.source);
            ++printed;
          }
          if (printed == 0) {
            std::printf("  nonzero vectors=0\n");
          }
        }
        ++frameNumber;
        av_frame_unref(frame);
      }
    }
    av_packet_unref(packet);
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  avformat_close_input(&format);
  return frameNumber >= 4 ? 0 : 1;
}
