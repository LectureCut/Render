#include "pipeline.h"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

struct local_cut
{
  int64_t start;
  int64_t end;
};

AVCodecContext *create_encoder_context(METADATA *metadata, int quality)
{
  const AVCodec *encodeCodec = avcodec_find_encoder(metadata->video_stream->codecpar->codec_id);
  AVCodecContext *encodeCtx = avcodec_alloc_context3(encodeCodec);
  avcodec_parameters_to_context(encodeCtx, metadata->video_stream->codecpar);
  encodeCtx->time_base = metadata->video_stream->time_base;
  encodeCtx->width = metadata->video_stream->codecpar->width;
  encodeCtx->height = metadata->video_stream->codecpar->height;
  encodeCtx->gop_size = 12;
  encodeCtx->max_b_frames = 2;
  encodeCtx->bit_rate = metadata->video_stream->codecpar->bit_rate;
  encodeCtx->qmin = 1;
  encodeCtx->qmax = 1;
  encodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  avcodec_open2(encodeCtx, encodeCodec, 0);
  return encodeCtx;
}

void transcode_video(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA *> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA *> *output_queue,
    int quality,
    cut_list *cut_list)
{
  int queue_id = output_queue->set_working();
  int ret;
  char errbuff[64] = {0};

  // relay metadata
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;
  metadata_ptr = new METADATA *(metadata);
  output_queue->set_special(metadata_ptr);

  std::vector<local_cut> *cuts = new std::vector<local_cut>(cut_list->num_cuts);

  // convert all cuts to time base
  for (int i = 0; i < cut_list->num_cuts; i++)
  {
    cuts->at(i).start = (long double)cut_list->cuts[i].start * metadata->video_stream->time_base.den / metadata->video_stream->time_base.num;
    cuts->at(i).end = (long double)cut_list->cuts[i].end * metadata->video_stream->time_base.den / metadata->video_stream->time_base.num;
  }

  // codec setup
  const AVCodec *decodeCodec = avcodec_find_decoder(metadata->video_stream->codecpar->codec_id);
  AVCodecContext *decodeCtx = avcodec_alloc_context3(decodeCodec);
  avcodec_parameters_to_context(decodeCtx, metadata->video_stream->codecpar);
  avcodec_open2(decodeCtx, decodeCodec, 0);

  // loop prep
  QUEUE_ITEM in_ctx[1];
  int64_t time_skipped = 0;

  auto current_cut = cuts->begin();

  std::vector<local_cut> segment_cuts = std::vector<local_cut>();

  while (input_queue->pop(in_ctx))
  {
    int64_t segment_start = in_ctx->packets->front()->pts;
    int64_t segment_end = in_ctx->packets->back()->pts + in_ctx->packets->back()->duration;

    // find all cuts in this segment
    segment_cuts.clear();
    while (current_cut != cuts->end() && current_cut->start < segment_end)
    {
      segment_cuts.push_back(*current_cut);
      if (current_cut->end > segment_end)
      {
        break;
      }
      current_cut++;
    }

    if (segment_cuts.size() == 0)
    {
      // no cuts in this segment, skip everything
      for (AVPacket *packet : *in_ctx->packets)
      {
        time_skipped += packet->duration;
      }
      continue;
    }

    if (segment_cuts.size() == 1 && segment_cuts.front().start <= segment_start && segment_cuts.front().end >= segment_end)
    {
      // only one cut in this segment, and it covers the whole segment
      // just pass the segment through
      for (AVPacket *packet : *in_ctx->packets)
      {
        packet->pts -= time_skipped;
        packet->dts -= time_skipped;
      }
      output_queue->push(in_ctx);
      continue;
    }

    QUEUE_ITEM *out_ctx = new QUEUE_ITEM[1];
    out_ctx->packets = new std::vector<AVPacket *>();

    // ========================================================
    // sadly transcode everything, because B frames exist... :(
    // ========================================================

    std::vector<AVFrame *> frames = std::vector<AVFrame *>();

    // decode all packets in this segment
    for (AVPacket *packet : *in_ctx->packets)
    {
      if ((ret = avcodec_send_packet(decodeCtx, packet)) != 0)
      {
        av_make_error_string(errbuff, 64, ret);
        std::cout << "Error sending packet: " << errbuff << std::endl;
        exit(1);
      }

      AVFrame *frame = av_frame_alloc();
      if ((ret = avcodec_receive_frame(decodeCtx, frame)) != 0)
      {
        if (ret != AVERROR(EAGAIN))
        {
          av_make_error_string(errbuff, 64, ret);
          std::cout << "Error receiving frame: " << errbuff << std::endl;
          exit(1);
        }
      }
      frames.push_back(frame);
    }

    // receive all frames in this segment
    while (true)
    {
      AVFrame *frame = av_frame_alloc();
      if ((ret = avcodec_receive_frame(decodeCtx, frame)) != 0)
      {
        if (ret != AVERROR(EAGAIN))
        {
          av_make_error_string(errbuff, 64, ret);
          std::cout << "Error receiving frame: " << errbuff << std::endl;
          exit(1);
        }
        break;
      }
      frames.push_back(frame);
    }

    if ((*in_ctx->packets).size() != frames.size())
    {
      std::cout << "Warning: packet count does not match frame count" << std::endl;
      if ((*in_ctx->packets).size() < frames.size())
      {
        std::cout << "Warning: more frames than packets, frames will be skipped" << std::endl;
      }
      else
      {
        std::cout << "Warning: more packets than frames, frames will be duplicated" << std::endl;
        for (int i = frames.size(); i < (*in_ctx->packets).size(); i++)
        {
          frames.push_back(av_frame_clone(frames.back()));
        }
      }
    }

    // sort packets by pts (because some idiot decided to make B frames)
    std::sort((*in_ctx->packets).begin(), (*in_ctx->packets).end(), [](const AVPacket *a, const AVPacket *b) -> bool
              { return a->pts < b->pts; });

    // TODO: check if all this is needed, seems like a lot of work for nothing
    // Open encoder
    AVCodecContext *encoder = create_encoder_context(metadata, quality);

    auto current_segment_cut = segment_cuts.begin();
    bool cut_has_keyframe = false;
    bool new_encoder = false;

    for (int i = 0; i < frames.size(); i++)
    {
      AVFrame *frame = frames[i];
      AVPacket *packet = (*in_ctx->packets)[i];

      int64_t packet_start_time = packet->pts;
      int64_t packet_end_time = packet->pts + packet->duration;

      // seek next segment cut
      while (current_segment_cut != segment_cuts.end() && current_segment_cut->end < packet_start_time)
      {
        if (current_segment_cut == segment_cuts.end() - 1)
        {
          break;
        }
        current_segment_cut++;
        cut_has_keyframe = false;
        new_encoder = true;
      }
      if (new_encoder)
      {
        // close old encoder
        avcodec_free_context(&encoder);
        // open new encoder
        encoder = create_encoder_context(metadata, quality);
      }

      // check if we're in a cut
      if (packet_end_time >= current_segment_cut->start && packet_start_time <= current_segment_cut->end)
      {
        // we're in a cut, send the frame to the encoder

        // if we haven't exported a keyframe yet, check if we can use the original one or
        // if we need to make a new one
        if (!cut_has_keyframe)
        {
          frame->pict_type = AV_PICTURE_TYPE_I;
          cut_has_keyframe = true;
        }
        av_frame_make_writable(frame);
        if ((ret = avcodec_send_frame(encoder, frame)) != 0)
        {
          av_make_error_string(errbuff, 64, ret);
          std::cout << "Error sending frame: " << errbuff << std::endl;
          exit(1);
        }
        av_frame_free(&frame);
        AVPacket *encoded_packet = new AVPacket[1];
        av_init_packet(encoded_packet);
        if ((ret = avcodec_receive_packet(encoder, encoded_packet)) != 0)
        {
          if (ret != AVERROR(EAGAIN))
          {
            av_make_error_string(errbuff, 64, ret);
            std::cout << "Error receiving packet: " << errbuff << std::endl;
            exit(1);
          }
          std::cout << "No packet received" << std::endl;
          continue;
        }
        encoded_packet->pts = packet->pts - time_skipped;
        encoded_packet->dts = encoded_packet->pts;
        out_ctx->packets->push_back(encoded_packet);
      }
      else
      {
        time_skipped += packet->duration;
      }

      av_packet_free(&packet);
    }

    // delete in_ctx->packets;
    // delete in_ctx;
    output_queue->push(out_ctx);
  }

  output_queue->set_done(queue_id);
}