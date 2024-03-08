#pragma once

#include "../render.h"

#include <deque>
#include <condition_variable>
#include <vector>
#include <algorithm>
#include <iostream>

struct AVPacket;
struct AVStream;
struct AVFormatContext;

template <typename T, typename S>
class PIPELINE_QUEUE
{
  std::deque<T> queue;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<bool> done = {};
  S *special = nullptr;

public:
  void push(T* data) {
    std::unique_lock<std::mutex> lock(mutex);
    // 36 is arbitrary (should hold around 3 min of video assunming a keyframe every 5 seconds)
    cv.wait(lock, [&]
            { return std::all_of(done.begin(), done.end(), [](bool x){ return x; }) ||
                     queue.size() < 36; });

    queue.push_back(*data);

    lock.unlock();
    cv.notify_one();
  }

  bool pop(T* data) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return std::all_of(done.begin(), done.end(), [](bool x){ return x; }) || queue.size() >= 1; });
    
    if (queue.size() == 0) return false;

    *data = queue.front();
    queue.pop_front();

    lock.unlock();
    cv.notify_one();
    return true;
  }

  size_t size() {
     std::lock_guard<std::mutex> lock(mutex);
     return queue.size();
  }

  size_t set_working() {
    std::lock_guard<std::mutex> lock(mutex);
    done.push_back(false);
    return done.size() - 1;
  }

  void set_done(size_t index) {
    std::unique_lock<std::mutex> lock(mutex);
    done[index] = true;
    lock.unlock();
    cv.notify_one();
  }

  bool all_done() {
    std::lock_guard<std::mutex> lock(mutex);
    return std::all_of(done.begin(), done.end(), [](bool x){ return x; });
  }

  void set_special(S *special) {
    std::unique_lock<std::mutex> lock(mutex);
    this->special = special;
    lock.unlock();
    cv.notify_one();
  }

  // blocks until special is set
  void get_special(S **special) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return std::all_of(done.begin(), done.end(), [](bool x){ return x; }) || this->special != nullptr; });
    *special = this->special;
    lock.unlock();
  }
};

struct QUEUE_ITEM {
  std::vector<AVPacket*> *packets;
};

struct METADATA {
  AVFormatContext *format_ctx;
  AVStream *video_stream;
  AVStream *audio_stream;
};

void segment(
    const char *filename,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *video_output_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *audio_output_queue
);

void transcode_video(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list);

void transcode_audio(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list);

void join(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    const char *filename);