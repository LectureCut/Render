#include "pipeline.h"

#include <iostream>

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

void join(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    const char *filename
)
{
  AVFormatContext *out_ctx;

  if (avformat_alloc_output_context2(&out_ctx, NULL, NULL, filename) < 0) {
    throw std::runtime_error("error allocating output context");
  };

  if (avio_open(&out_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
    throw std::runtime_error("error opening output file");
  }

  AVStream *videoSteam = avformat_new_stream(out_ctx, nullptr);
  AVStream *audioStream = avformat_new_stream(out_ctx, nullptr);

  if (videoSteam == nullptr || audioStream == nullptr) {
    throw std::runtime_error("error allocating output streams");
  }

  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;

  if (avcodec_parameters_copy(videoSteam->codecpar, metadata->video_stream->codecpar) < 0) {
    throw std::runtime_error("error copying video codec parameters");
  }
  if (avcodec_parameters_copy(audioStream->codecpar, metadata->audio_stream->codecpar) < 0) {
    throw std::runtime_error("error copying audio codec parameters");
  }

  videoSteam->codecpar->codec_tag = 0;
  audioStream->codecpar->codec_tag = 0;

  videoSteam->time_base = metadata->video_stream->time_base;
  audioStream->time_base = metadata->audio_stream->time_base;

  videoSteam->index = metadata->video_stream->index;
  audioStream->index = metadata->audio_stream->index;

  avformat_write_header(out_ctx, NULL);

  QUEUE_ITEM in_ctx[1];

  // Loop through each input context and write its packets to the output file
  while (input_queue->pop(in_ctx)) {
    for (auto pkt : *in_ctx->packets) {
      av_interleaved_write_frame(out_ctx, pkt);
      av_packet_unref(pkt);
    }
  }

  // // Write the trailer to the output file
  av_write_trailer(out_ctx);

  // // Close the output file
  avio_closep(&out_ctx->pb);
  avformat_free_context(out_ctx);
}