#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

class WorkQueue;

class Job {
 public:
  virtual ~Job() = default;
  virtual void operator()() = 0;
 private:
  std::unique_ptr<Job> next;
  friend class WorkQueue;
};

template <typename F>
struct Defer {
  F f;
  ~Defer() {
    std::move(f)();
  }
};

class WorkQueue {
 public:
  void push(std::unique_ptr<Job> job) {
    std::unique_lock lk(m);
    (void)++pending;
    if (back) {
      back->next = std::move(job);
      back = back->next.get();
    }
    else {
      front = std::move(job);
      back = front.get();
    }
    lk.unlock();
    cv.notify_one();
  }

  bool runOne() {
    auto job = take();
    if (job) {
      Defer d([this]{
        std::unique_lock lk(m);
        assert(pending > 0);
        if (--pending == 0) {
          assert(!front);
          lk.unlock();
          cv.notify_all();
        }
      });
      (*std::move(job))();
      return true;
    }
    return false;
  }

  void runUntilEmpty() {
    while (runOne()) { }
  }

 private:
  std::unique_ptr<Job> take() {
    std::unique_lock lk(m);
    cv.wait(lk, [this]{ return front || !pending; });
    auto ret = std::move(front);
    if (ret) {
      front = std::move(ret->next);
      if (!front) {
        back = nullptr;
      }
    }
    return ret;
  }

  std::mutex m;
  std::condition_variable cv;
  int pending = 0;
  std::unique_ptr<Job> front;
  Job* back = nullptr;
};
