#include "pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

void transcode_audio(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list
)
{
  int queue_id = output_queue->set_working();

  // don't relay metadata (video thread does that)
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;

  QUEUE_ITEM in_ctx[1];
  int64_t time_skipped = 0;

  cut *last_cut = cut_list->cuts + (cut_list->num_cuts - 1);
  cut *current_cut = cut_list->cuts;

  while (input_queue->pop(in_ctx))
  {
    QUEUE_ITEM *out_ctx = new QUEUE_ITEM[1];
    out_ctx->packets = new std::vector<AVPacket*>();

    for (AVPacket *packet : *in_ctx->packets) {
      // check if we're in a cut
      // convert pts to seconds
      int64_t packet_time = packet->pts * metadata->video_stream->time_base.num / metadata->video_stream->time_base.den;
      
      // seek to the next cut if we're past the current one
      while (current_cut < last_cut && packet_time > current_cut->end) {
        current_cut++;
      }
      
      if (packet_time >= current_cut->start && packet_time <= current_cut->end) {
        // we're in a cut, keep this packet
        packet->dts -= time_skipped;
        packet->pts -= time_skipped;
        
        out_ctx->packets->push_back(packet);

        continue;
      }
      
      time_skipped += packet->duration;
      av_packet_free(&packet);
    }

    // delete in_ctx->packets;
    // delete in_ctx;
    output_queue->push(out_ctx);
  }

  output_queue->set_done(queue_id);
}