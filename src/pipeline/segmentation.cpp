#include "pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

#include <iostream>

void segment(
  const char *file,
  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *segment_queue
)
{
  AVFormatContext *inputFormatContext = NULL;
  AVPacket pkt;
  int videoStreamIndex = -1;
  int audioStreamIndex = -1;
  std::vector<AVPacket *> *pipeline_packets = NULL;

  if (avformat_open_input(&inputFormatContext, file, NULL, NULL) < 0) {
    std::cout << "error opening input file" << std::endl;
    return;
  }

  if (avformat_find_stream_info(inputFormatContext, NULL) < 0) {
    std::cout << "error finding stream info" << std::endl;
    return;
  }

  for (unsigned int i = 0; i < inputFormatContext->nb_streams; i++) {
    if (inputFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStreamIndex = i;
    } else if (inputFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audioStreamIndex = i;
    }
  }

  if (videoStreamIndex == -1 || audioStreamIndex == -1) {
    std::cout << "error finding video or audio stream" << std::endl;
    return;
  }

  if (inputFormatContext->streams[videoStreamIndex] == nullptr ||
      inputFormatContext->streams[audioStreamIndex] == nullptr) {
    std::cout << "error finding video or audio stream" << std::endl;
    return;
  }

  if (inputFormatContext->streams[videoStreamIndex]->codecpar == nullptr ||
      inputFormatContext->streams[audioStreamIndex]->codecpar == nullptr) {
    std::cout << "error finding video or audio codec parameters" << std::endl;
    return;
  }

  AVCodecParameters *videoCodecParametersCopy = avcodec_parameters_alloc();
  AVCodecParameters *audioCodecParametersCopy = avcodec_parameters_alloc();
  if (avcodec_parameters_copy(videoCodecParametersCopy, inputFormatContext->streams[videoStreamIndex]->codecpar) < 0) {
    std::cout << "error copying video codec parameters" << std::endl;
    return;
  }
  if (avcodec_parameters_copy(audioCodecParametersCopy, inputFormatContext->streams[audioStreamIndex]->codecpar) < 0) {
    std::cout << "error copying audio codec parameters" << std::endl;
    return;
  }

  METADATA *metadata = new METADATA();
  metadata->video_stream = inputFormatContext->streams[videoStreamIndex];
  metadata->audio_stream = inputFormatContext->streams[audioStreamIndex];
  METADATA** metadata_ptr = new METADATA*(metadata);
  segment_queue->set_special(metadata_ptr);

  while (av_read_frame(inputFormatContext, &pkt) == 0) {
    if (pkt.stream_index != videoStreamIndex && pkt.stream_index != audioStreamIndex)
    {
      av_packet_unref(&pkt);
      // Skip this packet
      continue;
    }

    AVPacket *packet = new AVPacket();
    av_packet_ref(packet, &pkt);

    // Check if we need to start a new segment
    if (packet->stream_index == videoStreamIndex && packet->flags & AV_PKT_FLAG_KEY) {
      // Send the current segment in the pipeline
      
      if (pipeline_packets) {
        QUEUE_ITEM *item = new QUEUE_ITEM();
        item->packets = pipeline_packets;
        segment_queue->push(item, 1);
      }

      // Start a new segment
      pipeline_packets = new std::vector<AVPacket *>();
    }

    if (!pipeline_packets) {
      av_packet_unref(packet);
      std::cout << "error pipeline_packets is null" << std::endl;
      return;
    }
    
    pipeline_packets->push_back(packet);
  }

  segment_queue->set_done();
}
