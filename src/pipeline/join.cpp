#include "pipeline.h"

#include <iostream>

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
  #include "libavutil/error.h"
}

void join(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    const char *filename
)
{
  AVFormatContext *out_ctx;
  AVIOContext *io_ctx;
  int ret;
  char errbuff[64] = {0};

  if ((ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, filename)) < 0) {
    std::cout << "error allocating output context" << std::endl;
    std::cout << av_make_error_string(errbuff, 64, ret) << std::endl;
    return;
  };

  if ((ret = avio_open(&io_ctx, filename, AVIO_FLAG_WRITE)) < 0) {
    std::cout << "error opening output file" << std::endl;
    std::cout << av_make_error_string(errbuff, 64, ret) << std::endl;
    return;
  }

  if (out_ctx->oformat->flags & AVFMT_NOFILE) {
    std::cout << "output format does not support writing to file" << std::endl;
    return;
  }

  out_ctx->pb = io_ctx;

  AVStream *videoSteam = avformat_new_stream(out_ctx, nullptr);
  AVStream *audioStream = avformat_new_stream(out_ctx, nullptr);

  if (videoSteam == nullptr || audioStream == nullptr) {
    std::cout << "error creating streams" << std::endl;
    return;
  }

  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;

  if ((ret = avcodec_parameters_copy(videoSteam->codecpar, metadata->video_stream->codecpar)) < 0) {
    std::cout << av_make_error_string(errbuff, 64, ret) << std::endl;
  }
  if ((ret = avcodec_parameters_copy(audioStream->codecpar, metadata->audio_stream->codecpar)) < 0) {
    std::cout << av_make_error_string(errbuff, 64, ret) << std::endl;
  }

  videoSteam->codecpar->codec_tag = 0;
  audioStream->codecpar->codec_tag = 0;

  videoSteam->time_base = metadata->video_stream->time_base;
  audioStream->time_base = metadata->audio_stream->time_base;

  videoSteam->index = metadata->video_stream->index;
  audioStream->index = metadata->audio_stream->index;

  avformat_write_header(out_ctx, NULL);

  QUEUE_ITEM in_ctx[1];

  bool take = true;
  // Loop through each input context and write its packets to the output file
  while (input_queue->pop(in_ctx, 1) == 1) {
    if (!take) {
      for (auto pkt : *in_ctx->packets) {
        av_packet_unref(pkt);
      }
      take = true;
      continue;
    }
    take = false;
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