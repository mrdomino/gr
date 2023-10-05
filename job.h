#pragma once

#include <cassert>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

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
    (void)++pending;
    bool hadNone;
    { std::lock_guard lk(m);
      if (back) {
        hadNone = false;
        back->next = std::move(job);
        back = back->next.get();
      }
      else {
        hadNone = true;
        front = std::move(job);
        back = front.get();
      }
    }
    if (hadNone) {
      cv.notify_one();
    }
  }

  bool runOne() {
    auto job = take();
    if (job) {
      Defer d([this]{
        auto p = --pending;
        assert(p >= 0);
        if (p == 0) {
          cv.notify_all();
        }
      });
      (*std::move(job))();
      return true;
    }
    return false;
  }

  void runUntilEmpty() {
    while (!empty()) {
      if (!runOne()) {
        std::unique_lock lk(m);
        cv.wait(lk, [this]{ return back || empty(); });
      }
    }
  }

  bool empty() const {
    auto p = pending.load();
    assert(p >= 0);
    return p == 0;
  }

 private:
  std::unique_ptr<Job> take() {
    std::lock_guard lk(m);
    auto ret = std::move(front);
    if (ret) {
      front = std::move(ret->next);
      if (!front) {
        back = nullptr;
      }
    }
    return ret;
  }

  std::atomic_int pending = 0;
  std::mutex m;
  std::condition_variable cv;
  std::unique_ptr<Job> front;
  Job* back = nullptr;
};
