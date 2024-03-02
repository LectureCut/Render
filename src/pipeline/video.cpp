#include "pipeline.h"
#include <ranges>
#include <syncstream>
#include <thread>

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

const static AVRational CUT_TIMEBASE = av_make_q(1, 100);

void transcode_video(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA *> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA *> *output_queue,
    int quality,
    cut_list *cut_list)
{
  std::osyncstream cout_sync(std::cout);

  size_t queue_id = output_queue->set_working();
  char errbuff[64] = {0};

  // relay metadata
  METADATA **metadata_ptr;
  input_queue->get_special(&metadata_ptr);
  output_queue->set_special(metadata_ptr);
  METADATA *metadata = *metadata_ptr;

  const AVRational native_stream_timebase = metadata->video_stream->time_base;

  auto cuts = std::vector<local_cut>(cut_list->num_cuts);
  auto cut_list_span = std::span<cut>(cut_list->cuts, cut_list->num_cuts);

  // convert all cuts to time base
  int64_t offset = (metadata->video_stream->start_time == AV_NOPTS_VALUE) ? 0 : metadata->video_stream->start_time;
  for (int i = 0; i < cut_list->num_cuts; i++)
  {
    cuts[i].start = 
      av_rescale_q(cut_list->cuts[i].start, CUT_TIMEBASE, native_stream_timebase) + offset;
    cuts[i].end = 
      av_rescale_q(cut_list->cuts[i].end, CUT_TIMEBASE, native_stream_timebase) + offset;
  }

  cout_sync << "Video Timebase: " << native_stream_timebase.num << "/" << native_stream_timebase.den << std::endl;

  // loop prep
  QUEUE_ITEM* in_ctx = new QUEUE_ITEM();
  
  auto first_cut_in_cur_seg = cuts.begin();
  auto cut_to_be_exited = cuts.begin();
  std::vector<local_cut> segment_cuts {};

  int64_t time_skipped = 0;
  int64_t centiseconds_kept_before_seg = 0;
  int64_t dts_previous = std::numeric_limits<int64_t>::min();

  while (input_queue->pop(in_ctx))
  {
    auto segment_packets_first = in_ctx->packets->front();
    auto segment_packets_last = in_ctx->packets->back();
    int64_t segment_start = segment_packets_first->pts;
    int64_t segment_end = segment_packets_last->pts + segment_packets_last->duration;

    // find all cuts in this segment
    segment_cuts.clear();
    while (first_cut_in_cur_seg != cuts.end() && first_cut_in_cur_seg->start < segment_end)
    {
      segment_cuts.push_back(*first_cut_in_cur_seg);
      if (cut_to_be_exited->end <= segment_start) {
        const auto& last_cut_in_prev_seg = cut_list_span[cut_to_be_exited-cuts.begin()];
        centiseconds_kept_before_seg += last_cut_in_prev_seg.end - last_cut_in_prev_seg.start;
        cut_to_be_exited++;
      }
      if (first_cut_in_cur_seg->end > segment_end)
        break;
      first_cut_in_cur_seg++;
    }

    if (segment_cuts.empty()) // segment will be removed entirely 
    {
      for (AVPacket *packet : *in_ctx->packets)
        time_skipped += packet->duration;
    }
    else if (segment_cuts.size() == 1 && segment_cuts.front().start <= segment_start && segment_cuts.front().end >= segment_end) // segment  will be kept
    {
      for (AVPacket *packet : *in_ctx->packets)
      {
        packet->pts -= time_skipped;
        packet->dts -= time_skipped;

        if (packet->dts <= dts_previous) // if decoding timestamps are not strictly monotonically rising
          packet->dts = dts_previous + 1;
        dts_previous = packet->dts;
      }

      output_queue->push(in_ctx);
    }
    else { // segment will be partially kept
      auto out_ctx = new QUEUE_ITEM();
      out_ctx->packets = new std::vector<AVPacket *>();

      for (auto &packet : *in_ctx->packets)
      {
        // if packet pts after last cut in segment, it can be dropped
        if (packet->pts > (--segment_cuts.end())->end)
        {
          time_skipped += packet->duration;
          continue;
        }

        // find the cut from the list of cuts in this segment that this packet falls into
        auto cut = std::find_if(segment_cuts.begin(), segment_cuts.end(), [packet](local_cut &cut) {
          return packet->pts + packet->duration > cut.start && packet->pts < cut.end;
        });

        bool is_pkt_in_cut = cut == segment_cuts.end();

        if (is_pkt_in_cut) // if packet is NOT within a cut that's to be kept
        {
          packet->dts -= time_skipped;
          packet->pts = AV_NOPTS_VALUE; // -= time_skipped; // Using no pts at all so video player seek bars do not break
          packet->flags |= AV_PKT_FLAG_DISCARD; // use packet for decoding but do not display it
          time_skipped += packet->duration;
        } 
        else /* if (cut != segment_cuts.end()) */ { // if packet is within in a cut that's to be kept
          int64_t time_delta_cut_and_first_pkt = packet->pts - cut->start;
          int64_t time_delta_last_pkt_and_cut = (cut->end - 1) - packet->pts;
          bool is_frame_first_in_cut = packet->pts <= cut->start;
          bool is_frame_last_in_cut = time_delta_last_pkt_and_cut < packet->duration;
          // check if this is the first frame in the cut, and if so decrease dts and pts by difference between cut start and frame pts
          if (is_frame_first_in_cut)
            time_skipped -= time_delta_cut_and_first_pkt; // the time from the start of the packet which starts within or hangs over the start of the cut is added to the time skipped
          // check if this is the last frame in the cut, and if so decrease dts and pts of the next frame by difference between cut end and frame pts
          if (is_frame_last_in_cut)
            time_skipped -= -(time_delta_last_pkt_and_cut + 1 - packet->duration); // the time the frame hangs over the end of the cut is subtracted from the skipped time
          packet->dts -= time_skipped;
          packet->pts -= time_skipped;
        }

        // TODO: maybe find a nicer solution for this. But for now it is required to ensure monotonic dts.
        if (packet->dts <= dts_previous) { // if decoding timestamps are not strictly monotonically rising
          if (!is_pkt_in_cut) {
            int64_t inv_delta_dts = dts_previous - packet->dts;
            packet->pts += inv_delta_dts + 1;
          }
          packet->dts = dts_previous + 1;
        }
        dts_previous = packet->dts;

        out_ctx->packets->push_back(packet);
      }
      output_queue->push(out_ctx);
    }
  }

  cout_sync << "Centiseconds kept in thread #" << std::this_thread::get_id() << " up until the start of the last segment: " << centiseconds_kept_before_seg << std::endl;

  output_queue->set_done(queue_id);
}