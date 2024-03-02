#include "pipeline.h"
#include <ranges>
#include <syncstream>
#include <thread>


extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

struct local_cut
{
  int64_t start;
  int64_t end;
};

const static AVRational CUT_TIMEBASE = av_make_q(1, 100);

void transcode_audio(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list
)
{
  std::osyncstream cout_sync(std::cout);

  size_t queue_id = output_queue->set_working();

  // don't relay metadata (video thread does that)
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  METADATA *metadata = *metadata_ptr;

  const AVRational native_stream_timebase = metadata->audio_stream->time_base;

  auto cuts = std::vector<local_cut>(cut_list->num_cuts);
  auto cut_list_span = std::span<cut>(cut_list->cuts, cut_list->num_cuts);

  // convert all cuts to time base
  int64_t offset = (metadata->audio_stream->start_time == AV_NOPTS_VALUE) ? 0 : metadata->audio_stream->start_time;
  for (int i = 0; i < cut_list->num_cuts; i++)
  {
    cuts[i].start = 
      av_rescale_q(cut_list->cuts[i].start, CUT_TIMEBASE, native_stream_timebase) + offset;
    cuts[i].end = 
      av_rescale_q(cut_list->cuts[i].end, CUT_TIMEBASE, native_stream_timebase) + offset;
  }
  auto cut_current = cuts.begin();

  std::cout << "Audio Timebase: " << native_stream_timebase.num << "/" << native_stream_timebase.den << std::endl;

  // loop prep
  QUEUE_ITEM *in_ctx = new QUEUE_ITEM();
  int64_t time_skipped = 0;
  int64_t centiseconds_kept_before_seg = 0;

  while (input_queue->pop(in_ctx))
  {
    QUEUE_ITEM *out_ctx = new QUEUE_ITEM();
    out_ctx->packets = new std::vector<AVPacket*>();

    for (AVPacket *packet : *in_ctx->packets) {    
      // seek to the next cut if we're past the current one
      while (cut_current != cuts.end() && packet->pts > cut_current->end) {
        const auto& last_cut_in_prev_seg = cut_list_span[cut_current-cuts.begin()];
        centiseconds_kept_before_seg += last_cut_in_prev_seg.end - last_cut_in_prev_seg.start;
        cut_current++;
      }

      if (packet->pts + packet->duration > cut_current->start && packet->pts < cut_current->end) // if packet is within in a cut that's to be kept
      {
        int64_t time_delta_cut_and_first_pkt = packet->pts - cut_current->start;
        int64_t time_delta_last_pkt_and_cut = (cut_current->end - 1) - packet->pts;
        bool is_frame_first_in_cut = packet->pts <= cut_current->start;
        bool is_frame_last_in_cut = time_delta_last_pkt_and_cut < packet->duration;
        // check if this is the first frame in the cut, and if so decrease dts and pts by difference between cut start and frame pts
        if (is_frame_first_in_cut)
          time_skipped -= time_delta_cut_and_first_pkt;
        // check if this is the last frame in the cut, and if so decrease dts and pts of the next frame by difference between cut end and frame pts
        if (is_frame_last_in_cut)
          time_skipped -= -(time_delta_last_pkt_and_cut + 1 - packet->duration); // the time the frame hangs over the end of the cut is subtracted from the skipped time
        packet->dts -= time_skipped;
        packet->pts -= time_skipped;
        
        out_ctx->packets->push_back(packet);
      }
      else { // if packet is NOT within a cut that's to be kept
        time_skipped += packet->duration;
        av_packet_free(&packet);
      }
    }

    // delete in_ctx->packets;
    // delete in_ctx;
    output_queue->push(out_ctx);
  }

  output_queue->set_done(queue_id);
}