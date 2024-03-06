#include "pipeline.h"
#include "../definitions.h"
#include <ranges>
#include <syncstream>
#include <thread>
#include <iomanip>
#include <string>

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
const static AVRational TIMEBASE_1MS = av_make_q(1, 1000);

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

  #if PRINT_VERBOSE
  { // DEBUGGING
    cout_sync << "Video Timebase: " << native_stream_timebase.num << "/" << native_stream_timebase.den << std::endl;
  }
  #endif

  // loop prep
  QUEUE_ITEM* in_ctx = new QUEUE_ITEM();
  
  auto first_cut_in_cur_seg = cuts.begin();
  auto cut_to_be_exited = cuts.begin();
  std::vector<local_cut> segment_cuts {};

  int64_t centiseconds_of_complete_cuts_kept_before_segment = 0;

  int64_t time_video_delay_prev = 0;

  int64_t dts_prev = std::numeric_limits<int64_t>::min();

  while (input_queue->pop(in_ctx))
  {

    const auto [segment_packets_first_by_pts, segment_packets_last_by_pts] = 
      std::minmax_element(
        (*in_ctx->packets).begin(), (*in_ctx->packets).end(), 
        [](AVPacket *a, AVPacket *b) { 
          return a->pts < b->pts; 
        }
      );
    int64_t segment_start = (*segment_packets_first_by_pts)->pts;
    int64_t segment_end = (*segment_packets_last_by_pts)->pts + (*segment_packets_last_by_pts)->duration;

    // find all cuts in this segment
    segment_cuts.clear();
    {
      auto cut_current = first_cut_in_cur_seg;
      for (
        ; 
        cut_current != cuts.end() && cut_current->start < segment_end; 
        cut_current++
      ) {
        segment_cuts.push_back(*cut_current);
        if (cut_current->end > segment_end)
          break;
      }
      first_cut_in_cur_seg = cut_current;
    }
    
    // find the cut that will be completed / exited from next
    while (cut_to_be_exited->end <= segment_start) {
      const auto& last_cut_in_prev_seg = cut_list_span[cut_to_be_exited-cuts.begin()];
      centiseconds_of_complete_cuts_kept_before_segment += last_cut_in_prev_seg.end - last_cut_in_prev_seg.start;
      cut_to_be_exited++;
    }

    // this segment isn't part of any cuts
    if (segment_cuts.empty()) {
      // the segment will be removed entirely
    }
    // this segment is entirely contained in one cut
    else if (segment_cuts.size() == 1 && segment_cuts.front().start <= segment_start && segment_cuts.front().end >= segment_end) // segment  will be kept
    {
      auto time_discarded_before_first_and_only_cut_in_segment = segment_cuts.front().start - av_rescale_q(centiseconds_of_complete_cuts_kept_before_segment, CUT_TIMEBASE, native_stream_timebase);

      auto time_video_delay = time_video_delay_prev;
      // determine how much later the end of the cut occurs compared to the end of the last packet falling in it
      if (segment_cuts.front().end <= segment_end) {
        auto last_packet_in_cut = *((*in_ctx->packets).end()-1);
        auto end_pts_of_last_packet_in_cut = last_packet_in_cut->pts + last_packet_in_cut->duration;
        time_video_delay_prev = segment_cuts.front().end - end_pts_of_last_packet_in_cut;
      }
      // determine how much sooner the start of the first packet falling into a cut occurs compared to the start of that cut
      if (segment_cuts.front().start >= segment_start) {
        auto first_packet_in_cut = *(*in_ctx->packets).begin();
        time_video_delay += first_packet_in_cut->pts - segment_cuts.front().start;
      }

      for (AVPacket *packet : *in_ctx->packets)
      {
        packet->dts -= time_discarded_before_first_and_only_cut_in_segment + time_video_delay;

        // just checking in case the input video is faulty
        if (packet->pts != AV_NOPTS_VALUE)
          packet->pts -= time_discarded_before_first_and_only_cut_in_segment + time_video_delay;
        else
          packet->flags |= AV_PKT_FLAG_DISCARD;
      }

      // start marking packets going backwards from the end the end of the cut as to not be displayed until the delay is accounted for
      for (
        auto packet = (*in_ctx->packets).rbegin(); 
        packet != (*in_ctx->packets).rend() && abs(time_video_delay) >= abs(time_video_delay - (*packet)->duration); 
        packet++
      ) {
        time_video_delay -= (*packet)->duration;
        (*packet)->flags |= AV_PKT_FLAG_DISCARD;
      }

      // if dts_prev has not been set yet, set it to the earliest dts in the segment
      if (dts_prev == std::numeric_limits<int64_t>::min())
        dts_prev = (*in_ctx->packets)[0]->dts;

      for (auto packet : *in_ctx->packets) {
        // if the dts hasn't increased
        if (packet->dts <= dts_prev) {
          packet->dts = dts_prev + 1;
          // if the above line leads 'pts >= dts' to be violated
          if (packet->pts < packet->dts) {
            packet->pts = packet->dts;
          }
        }
        dts_prev = packet->dts;
      }

      // set it to the latest dts in the segment as it will only ever be read in partially cut segments
      dts_prev = (*((*in_ctx->packets).end()-1))->dts;

      #if PRINT_VERBOSE
      { // DEBUGGING
        const static size_t decimal_digits_in_int64_t = std::to_string(std::numeric_limits<int64_t>::max()).length();
        char fill = '.';
        cout_sync.fill(fill);
        cout_sync << std::endl;
        cout_sync << std::format("{:>14}", "Segment Info") << " | Keeping as is" << std::endl;
        cout_sync << std::endl;
        cout_sync << std::format("{:>14}", "Cut Info") << 
          " | Time discarded by now:" << fill << std::setw(decimal_digits_in_int64_t) << time_discarded_before_first_and_only_cut_in_segment <<
          " | Drift before :" << fill << std::setw(decimal_digits_in_int64_t) << time_video_delay <<
          " | Idx: " << cut_to_be_exited-cuts.begin() << " (" << 0 << " in segment)" <<
          " | Start: " << segment_cuts.begin()->start <<
          " | End: " << segment_cuts.begin()->end <<  std::endl; 
        cout_sync << std::endl;
        auto packets_sorted_pts = *in_ctx->packets;
        std::sort(packets_sorted_pts.begin(), packets_sorted_pts.end(), [](AVPacket *a, AVPacket *b) { return a->pts < b->pts; });
        for (auto packet : packets_sorted_pts) {
          cout_sync << std::format("{:>14}", "Packet Info") <<
            " | PTS in s: " << std::format("{:10.5f}", av_rescale_q(packet->pts, native_stream_timebase, TIMEBASE_1MS)/1000.0) << 
            " | PTS:" << fill << std::setw(decimal_digits_in_int64_t) << packet->pts << 
            " | DTS:" << fill << std::setw(decimal_digits_in_int64_t) << packet->dts << 
            " | PTS geq DTS: " << (packet->pts >= packet->dts) << 
            std::endl;
        }
      }
      #endif

      output_queue->push(in_ctx);
    }
    // the segment is part of multiple cuts
    else {
      auto out_ctx = new QUEUE_ITEM();
      out_ctx->packets = new std::vector<AVPacket *>();

      int64_t time_of_complete_cuts_kept_before_segment = av_rescale_q(centiseconds_of_complete_cuts_kept_before_segment, CUT_TIMEBASE, native_stream_timebase);

      // copy pointers to input packets
      auto packets_sorted_pts = *in_ctx->packets;
      // sort pointers to input packets by their presentation timestamps
      std::sort(packets_sorted_pts.begin(), packets_sorted_pts.end(), [](AVPacket *a, AVPacket *b) { return a->pts < b->pts; });

      auto packets_sorted_pts_chunked_by_cut_starts = std::vector<std::vector<AVPacket *>>(segment_cuts.size()+1);
      for (
        auto cut_current = segment_cuts.begin(); 
        auto packet : packets_sorted_pts
      ) {
        bool hide_packet = true;

        // advance cut_current until we find the cut before whose end the current packet lies in terms of pts
        while (cut_current->end < packet->pts && cut_current < segment_cuts.end())
          cut_current++;

        // if the current cut is the last cut in the segment
        if (cut_current == segment_cuts.end()) {
          // mark the packet as to be removed
          packet->flags |= AV_PKT_FLAG_DISPOSABLE;
        }
        // if the end of the presentation of the packet is after the start of the cut
        else if (packet->pts + packet->duration > cut_current->start) {
          // if the end of the presentation of the packet is before or on the end of the cut
          if (packet->pts + packet->duration <= cut_current->end) {
            // do not hide it, it needs to be displayed
            hide_packet = false;
          }
          // else keep the packet for decoding but do not display it
        }
        // else keep the packet for decoding but do not display it

        if (hide_packet) {
          // use packet for decoding but do not display it
          packet->flags |= AV_PKT_FLAG_DISCARD;
        }

        // save the packet as part of this cut
        packets_sorted_pts_chunked_by_cut_starts[cut_current - segment_cuts.begin()].push_back(packet);
      }

      // initialize vector to carry the time discarded up until each cut in the segment
      auto time_discarded_before_cuts = std::vector<int64_t>(segment_cuts.size());
      // initialize vector to carry the video delay up until each cut in the segment
      auto time_video_delay_before_cuts = std::vector<int64_t>(segment_cuts.size());
      // initialize variable to store the sum of time from all cuts passed in the segment
      int64_t time_of_complete_cuts_kept_within_segment = 0;
      for (
        auto cut_current = segment_cuts.begin();
        cut_current != segment_cuts.end();
        cut_current++
      ) {
        size_t cut_idx = cut_current-segment_cuts.begin();
        time_discarded_before_cuts[cut_idx] = cut_current->start - (time_of_complete_cuts_kept_before_segment + time_of_complete_cuts_kept_within_segment);
        time_of_complete_cuts_kept_within_segment += cut_current->end - cut_current->start;

        time_video_delay_before_cuts[cut_idx] = time_video_delay_prev;
        // determine how much later the end of the cut occurs compared to the end of the last packet falling in it
        // requires searching for the first displayable packet from the end of the cut first
        if (cut_current->end <= segment_end) {
          auto it = packets_sorted_pts_chunked_by_cut_starts[cut_idx].rbegin();
          for (;
           it != packets_sorted_pts_chunked_by_cut_starts[cut_idx].rend() 
           && (
            ((*it)->flags & AV_PKT_FLAG_DISPOSABLE) == AV_PKT_FLAG_DISPOSABLE || 
            ((*it)->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD || 
            (*it)->pts == AV_NOPTS_VALUE
           );
           it++
          ) { }
          if (it != packets_sorted_pts_chunked_by_cut_starts[cut_idx].rend()) {
            auto last_packet_in_cut = *it;
            auto end_pts_of_last_packet_in_cut = last_packet_in_cut->pts + last_packet_in_cut->duration;
            time_video_delay_prev = cut_current->end - end_pts_of_last_packet_in_cut;
          } 
        }
        // determine how much sooner the start of the first packet falling into a cut occurs compared to the start of that cut
        // requires searching for the first displayable packet from the start of the cut first
        if (cut_current->start >= segment_start) {
          auto it = packets_sorted_pts_chunked_by_cut_starts[cut_idx].begin();
          for (;
            it != packets_sorted_pts_chunked_by_cut_starts[cut_idx].end() 
            && (
              ((*it)->flags & AV_PKT_FLAG_DISPOSABLE) == AV_PKT_FLAG_DISPOSABLE || 
              ((*it)->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD || 
              (*it)->pts == AV_NOPTS_VALUE
            );
            it++
          ) { }
          if (it != packets_sorted_pts_chunked_by_cut_starts[cut_idx].end()) {
            auto first_packet_in_cut = *it;
            time_video_delay_before_cuts[cut_idx] += first_packet_in_cut->pts - cut_current->start;
          }
        }
      }

      // shift all timestamps by the offset appropriate for the cut they're in
      // packets that will be deleted are shifted by the offset of the last cut
      for (size_t cut_idx = 0; cut_idx < packets_sorted_pts_chunked_by_cut_starts.size(); cut_idx++)
      {
        size_t cut_idx_clamped = std::min(segment_cuts.size() - 1, cut_idx);

        int64_t time_discarded_before_cut = time_discarded_before_cuts[cut_idx_clamped];
        int64_t time_video_delay = time_video_delay_before_cuts[cut_idx_clamped];
        for (auto packet_sorted_pts_after_cut : packets_sorted_pts_chunked_by_cut_starts[cut_idx])
        {
          packet_sorted_pts_after_cut->dts -= time_discarded_before_cut + time_video_delay;

          // just checking in case the input video is faulty
          if (packet_sorted_pts_after_cut->pts != AV_NOPTS_VALUE) 
            packet_sorted_pts_after_cut->pts -= time_discarded_before_cut + time_video_delay;
          else
            packet_sorted_pts_after_cut->flags |= AV_PKT_FLAG_DISCARD;
        }

        // start marking packets going backwards from the end the end of the cut as to not be displayed until the delay is accounted for
        for (
          auto packet = packets_sorted_pts_chunked_by_cut_starts[cut_idx].rbegin(); 
          packet != packets_sorted_pts_chunked_by_cut_starts[cut_idx].rend() && abs(time_video_delay - (*packet)->duration) < abs(time_video_delay); 
          packet++
        ) {
          bool packet_not_displayed = 
            ((*packet)->flags & AV_PKT_FLAG_DISPOSABLE) == AV_PKT_FLAG_DISPOSABLE || 
            ((*packet)->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD || 
            (*packet)->pts == AV_NOPTS_VALUE;
          // if the packet is one that will be displayed
          if (!packet_not_displayed) {
            time_video_delay -= (*packet)->duration;
            (*packet)->flags |= AV_PKT_FLAG_DISCARD;
          }
        }
      }
   
      // if dts_prev has not been set yet, set it to the earliest dts in the segment
      if (dts_prev == std::numeric_limits<int64_t>::min())
        dts_prev = (*in_ctx->packets)[0]->dts;

      for (auto packet : *in_ctx->packets) {
        if ((packet->flags & AV_PKT_FLAG_DISPOSABLE) != AV_PKT_FLAG_DISPOSABLE) {
          // if the packet will be discarded
          if ((packet->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD) {
            // tightly pack it after the last one
            packet->dts = dts_prev + 1;
            packet->pts = packet->dts;
          }
          // if the packet won't be discarded and the dts hasn't increased
          else if (packet->dts <= dts_prev) {
            packet->dts = dts_prev + 1;
            // if the above line leads 'pts >= dts' to be violated
            if (packet->pts < packet->dts) {
              packet->pts = packet->dts;
            }
          }
          dts_prev = packet->dts;
        }
      }

      #if PRINT_VERBOSE
      { // DEBUGGING
        for (int cut_idx = 0; cut_idx < packets_sorted_pts_chunked_by_cut_starts.size()-1; cut_idx++)
        {
          const static size_t decimal_digits_in_int64_t = std::to_string(std::numeric_limits<int64_t>::max()).length();
          char fill = '.';
          cout_sync.fill(fill);
          cout_sync << std::endl;
          cout_sync << std::format("{:>14}", "Segment Info") << " | Cutting up" << std::endl;
          cout_sync << std::endl;
          cout_sync << std::format("{:>14}", "Cut Info") << 
            " | Time discarded by now:" << fill << std::setw(decimal_digits_in_int64_t) << time_discarded_before_cuts[cut_idx] << 
            " | Drift before :" << fill << std::setw(decimal_digits_in_int64_t) << time_video_delay_before_cuts[cut_idx] <<
            " | Idx: " << cut_to_be_exited-cuts.begin() << " (" << cut_idx << " in segment)" <<
            " | Start: " << (segment_cuts.begin() + cut_idx)->start <<
            " | End: " << (segment_cuts.begin() + cut_idx)->end << std::endl;
          cout_sync << std::endl;
          for (auto packet_sorted_pts_after_cut : packets_sorted_pts_chunked_by_cut_starts[cut_idx])
            cout_sync << std::format("{:>14}", "Packet Info") <<
              " | PTS in s: " << std::format("{:10.5f}", av_rescale_q(packet_sorted_pts_after_cut->pts, native_stream_timebase, TIMEBASE_1MS)/1000.0) <<           
              " | PTS:" << fill << std::setw(decimal_digits_in_int64_t) << packet_sorted_pts_after_cut->pts << 
              " | DTS:" << fill << std::setw(decimal_digits_in_int64_t) << packet_sorted_pts_after_cut->dts << 
              " | PTS geq DTS: " << (packet_sorted_pts_after_cut->pts >= packet_sorted_pts_after_cut->dts) << 
              " | Hide: " << ((packet_sorted_pts_after_cut->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD) << 
              " | Drop: " << ((packet_sorted_pts_after_cut->flags & AV_PKT_FLAG_DISPOSABLE) == AV_PKT_FLAG_DISPOSABLE) << 
              std::endl;
        }
      }
      #endif

      for (auto packet : *in_ctx->packets)
      {
        if ((packet->flags & AV_PKT_FLAG_DISPOSABLE) == AV_PKT_FLAG_DISPOSABLE)
            av_packet_free(&packet);
        else {
          if ((packet->flags & AV_PKT_FLAG_DISCARD) == AV_PKT_FLAG_DISCARD)
              packet->pts = AV_NOPTS_VALUE;
          out_ctx->packets->push_back(packet);
        }
      }
      
      output_queue->push(out_ctx);
    }

  }

  #if PRINT_VERBOSE
  {  // DEBUGGING
    cout_sync << "Centiseconds kept in thread #" << std::this_thread::get_id() << " up until the start of the last segment: " << centiseconds_of_complete_cuts_kept_before_segment << std::endl;
  }
  #endif

  output_queue->set_done(queue_id);
}