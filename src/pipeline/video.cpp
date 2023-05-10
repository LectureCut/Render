#include "pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

void transcode_video(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list
)
{
  int queue_id = output_queue->set_working();

  // relay metadata
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;
  metadata_ptr = new METADATA*(metadata);
  output_queue->set_special(metadata_ptr);

  // codec setup
  const AVCodec* decodeCodec = avcodec_find_decoder(metadata->video_stream->codecpar->codec_id);
  const AVCodec* encodeCodec = avcodec_find_encoder(metadata->video_stream->codecpar->codec_id);
  AVCodecContext* decodeCtx = avcodec_alloc_context3(decodeCodec);
  AVCodecContext* encodeCtx = avcodec_alloc_context3(encodeCodec);
  avcodec_parameters_to_context(decodeCtx, metadata->video_stream->codecpar);
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
  avcodec_open2(decodeCtx, decodeCodec, 0);
  avcodec_open2(encodeCtx, encodeCodec, 0);

  // loop prep
  QUEUE_ITEM in_ctx[1];
  int64_t time_skipped = 0;

  cut *last_cut = cut_list->cuts + (cut_list->num_cuts - 1);
  cut *current_cut = cut_list->cuts;

  while (input_queue->pop(in_ctx))
  {
    QUEUE_ITEM *out_ctx = new QUEUE_ITEM[1];
    out_ctx->packets = new std::vector<AVPacket*>();

    bool has_keyframe = false;

    for (AVPacket *packet : *in_ctx->packets) {
      // TODO: remove this
      // detect B frames
      if (packet->dts != packet->pts) {
        std::cout << "B frame detected" << std::endl;
        exit(1);
      }
      

      // check if we're in a cut
      // convert pts to seconds
      int64_t packet_start_time = packet->pts * metadata->video_stream->time_base.num / metadata->video_stream->time_base.den;
      int64_t packet_end_time = packet_start_time + packet->duration * metadata->video_stream->time_base.num / metadata->video_stream->time_base.den;

      // seek to the next cut if we're past the current one
      while (current_cut < last_cut && packet_start_time > current_cut->end) {
        current_cut++;
      }

      if (packet_end_time > current_cut->start && packet_start_time < current_cut->end) {
        // we're in a cut, keep this packet

        // if we haven't exported a keyframe yet, check if we can use the original one or
        // if we need to make a new one
        if (!has_keyframe && packet != in_ctx->packets->front()) {
          int ret;
          char errbuff[64] = {0};

          AVFrame *final_frame = nullptr;
          AVFrame *tmp_frame = av_frame_alloc();
          bool got_frame = false;
          for (auto pkt = in_ctx->packets->begin(); *pkt != packet; pkt++) {
            if ((ret = avcodec_send_packet(decodeCtx, *pkt)) != 0) {
              av_make_error_string(errbuff, 64, ret);
              std::cout << "Error sending packet: " << errbuff << std::endl;
              exit(1);
            }
            if ((ret = avcodec_receive_frame(decodeCtx, tmp_frame)) == 0) {
              final_frame = av_frame_clone(tmp_frame);
              got_frame = true;
            } else {
              av_make_error_string(errbuff, 64, ret);
              std::cout << "Error receiving frame: " << errbuff << std::endl;
              if (ret != AVERROR(EAGAIN)) exit(1);
            }
          }
          AVPacket *keyframe = new AVPacket[1];
          if (!got_frame) {
            std::cout << "Couldn't find keyframe, using original" << std::endl;
            // NOTE: should never happen on correct input
            keyframe = *in_ctx->packets->begin();
          } else {
            final_frame->pict_type = AV_PICTURE_TYPE_I;
            av_frame_make_writable(final_frame);
            if ((ret = avcodec_send_frame(encodeCtx, final_frame)) != 0) {
              av_make_error_string(errbuff, 64, ret);
              std::cout << "Error sending frame: " << errbuff << std::endl;
              exit(1);
            }
            av_init_packet(keyframe);
            while (avcodec_receive_packet(encodeCtx, keyframe) == 0);
          }

          packet->data = keyframe->data;
          packet->size = keyframe->size;
          packet->flags |= AV_PKT_FLAG_KEY;
          has_keyframe = true;
        }

        packet->dts -= time_skipped;
        packet->pts -= time_skipped;

        if (packet_start_time < current_cut->start) {
          // need to shorten duration
          int64_t offset = current_cut->start - packet_start_time;
          // convert offset to output stream time base
          offset = offset * metadata->video_stream->time_base.den / metadata->video_stream->time_base.num;
          packet->duration -= offset;
          time_skipped += offset;
        }

        if (packet_end_time > current_cut->end) {
          // need to shorten duration
          int64_t offset = packet_end_time - current_cut->end;
          // convert offset to output stream time base
          offset = offset * metadata->video_stream->time_base.den / metadata->video_stream->time_base.num;
          packet->duration -= offset;
          time_skipped += offset;
        }

        out_ctx->packets->push_back(packet);

        continue;
      }

      time_skipped += packet->duration;
      // TODO: unref later
      // av_packet_free(&packet);
    }

    // delete in_ctx->packets;
    // delete in_ctx;
    output_queue->push(out_ctx);
  }

  output_queue->set_done(queue_id);
}