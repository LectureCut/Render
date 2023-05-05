#pragma once

#include "../render.h"

#include <deque>
#include <condition_variable>
#include <vector>
#include <iostream>

struct AVPacket;
struct AVStream;
struct AVCodecParameters;

template <typename T, typename S>
class PIPELINE_QUEUE
{
  std::deque<T> queue;
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  S *special = nullptr;

public:
  void push(T* data, int size) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done || queue.size() < 36; });
    for (int i = 0; i < size; i++) {
      queue.push_back(data[i]);
    }
    lock.unlock();
    cv.notify_one();
  }

  int pop(T* data, int size) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done || queue.size() >= size; });
    int i = 0;
    while (i < size && !queue.empty()) {
      data[i] = queue.front();
      queue.pop_front();
      i++;
    }
    lock.unlock();
    cv.notify_one();
    return i;
  }

  int size() {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
  }

  void set_done() {
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
  }

  bool is_done() {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
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
    cv.wait(lock, [&] { return done || this->special != nullptr; });
    *special = this->special;
    lock.unlock();
  }
};

struct QUEUE_ITEM {
  std::vector<AVPacket*> *packets;
};

struct METADATA {
  AVStream *video_stream;
  AVStream *audio_stream;
};

void segment(
    const char *filename,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue);

void transcode(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *output_queue,
    int quality,
    cut_list *cut_list);

void join(
    PIPELINE_QUEUE<QUEUE_ITEM, METADATA*> *input_queue,
    const char *filename);