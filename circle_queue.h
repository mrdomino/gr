#pragma once

#include <cassert>
#include <limits>
#include <memory>
#include <type_traits>

template <typename T>
class CircleQueue: private std::allocator<T> {
  using A = std::allocator<T>;
 public:
  class iterator;

  CircleQueue(size_t capacity)
      : size_(capacity)
      , full(false)
      , start(0)
      , data(size_ ? A::allocate(size_) : nullptr) {
    static_assert(sizeof(*this) == 2 * sizeof(size_t) + sizeof(T*));
    const auto deleter = [this](T* p) {
      if (p) {
        A::deallocate(p, size_);
      }
    };
    auto hold = std::unique_ptr<T, decltype(deleter)>(data, deleter);
    if (capacity > std::numeric_limits<ssize_t>::max()) {
      throw std::runtime_error{"CircleQueue capacity too large"};
    }
    hold.release();
  }

  ~CircleQueue() /* noexcept */ {
    if (data) {
      clear();
      A::deallocate(data, size_);
    }
  }

  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      std::destroy_n(std::launder(data), size());
    }
    full = false;
    start = 0;
  }

  T& operator[](size_t i) {
    return *std::launder(data + index(i));
  }

  T const& operator[](size_t i) const {
    return *std::launder(data + index(i));
  }

  size_t size() const noexcept {
    return full ? size_ : start;
  }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      if (full) {
        std::destroy_at(std::launder(data + start));
      }
    }
    std::construct_at(data + start, std::forward<Args>(args)...);
    if (++start == size_) {
      full = true;
      start = 0;
    }
  }

  iterator begin() noexcept {
    return iterator(this, 0);
  }

  iterator end() noexcept {
    return iterator(this, size());
  }

 private:
  inline size_t index(size_t i) const noexcept {
    return full ? (start + i) % size_ : i;
  }

  const size_t size_;
  bool full : 1;
  size_t start : std::numeric_limits<ssize_t>::digits;
  T* data;
};

template <typename T>
class CircleQueue<T>::iterator {
 public:
  using value_type = T;

  iterator(iterator const& r) noexcept: obj(r.obj), i(r.i) {}

  T& operator*() {
    return obj[i];
  }

  iterator& operator++() noexcept {
    ++i;
    return *this;
  }

  bool operator==(iterator that) const noexcept {
    assert(std::addressof(obj) == std::addressof(that.obj));
    return i == that.i;
  }

 private:
  iterator(CircleQueue* obj, size_t i): obj(*obj), i(i) {}
  friend class CircleQueue;

  CircleQueue& obj;
  size_t i;
};
