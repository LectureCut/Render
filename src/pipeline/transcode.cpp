#include "pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
}

void transcode(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list
)
{
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  output_queue->set_special(metadata_ptr);

  QUEUE_ITEM in_ctx[1];

  while (input_queue->pop(in_ctx, 1) == 1) {
    QUEUE_ITEM *out_ctx = new QUEUE_ITEM[1];
    out_ctx->packets = new std::vector<AVPacket*>();

    for (AVPacket *packet : *in_ctx->packets) {
      out_ctx->packets->push_back(packet);
    }

    output_queue->push(out_ctx, 1);
  }

  output_queue->set_done();
}