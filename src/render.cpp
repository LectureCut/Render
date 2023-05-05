#include "render.h"
#include "definitions.h"
#include "pipeline/pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

const char* version()
{
  return VERSION;
}

void init()
{
  // av_log_set_level(AV_LOG_QUIET);
  
  avformat_network_init();
}

void render(
    const char *file,
    const char *output,
    cut_list cuts,
    int quality,
    progress_callback *progress
)
{
  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> segment_queue;
  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> join_queue;

  std::thread segment_thread(segment, file, &segment_queue);
  std::thread transcode_thread(transcode, &segment_queue, &join_queue, quality, &cuts);
  std::thread join_thread(join, &join_queue, output);

  segment_thread.join();
  transcode_thread.join();
  join_thread.join();
}