#include "render.h"
#include "definitions.h"
#include "pipeline/pipeline.h"

extern "C" {
  #include "libavcodec/avcodec.h"
  #include "libavformat/avformat.h"
}

const char* version(error_callback* _)
{
  return VERSION;
}

void init(error_callback* _)
{
  #if PRINT_VERBOSE
  av_log_set_level(AV_LOG_VERBOSE);
  #else
  av_log_set_level(AV_LOG_ERROR);
  #endif
  
  avformat_network_init();
}

ArgumentList get_arguments(error_callback* _)
{
  Argument *args = new Argument[1]{
    { 'q', "quality", "The quality of the output video", false, false }
  };

  return { 1, args };
}

void render(
    const char *file,
    const char *output,
    cut_list cuts,
    ArgumentResultList args,
    progress_callback *progress,
    error_callback *_
)
{
  int quality = 23;
  for (int i = 0; i < args.num_args; i++)
  {
    if (strcmp(args.args[i].name, "quality") == 0)
    {
      quality = atoi(args.args[i].value);
    }
  }

  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> segment_video_queue;
  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> segment_audio_queue;
  PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> join_queue;

  std::thread segment_thread(segment, file, &segment_video_queue, &segment_audio_queue);
  std::thread video_thread(transcode_video, &segment_video_queue, &join_queue, quality, &cuts);
  std::thread audio_thread(transcode_audio, &segment_audio_queue, &join_queue, quality, &cuts);
  std::thread join_thread(join, &join_queue, output);

  segment_thread.join();
  video_thread.join();
  audio_thread.join();
  join_thread.join();
}