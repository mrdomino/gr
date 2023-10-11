#pragma once

#include <limits>
#include <memory>
#include <new>
#include <type_traits>

template <typename T>
class CircleQueue {
 public:
  class iterator;

  explicit CircleQueue(const size_t capacity): size_(capacity), data(alloc()) {}

  ~CircleQueue() /* noexcept */ {
    clear();
  }

  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      std::ranges::destroy_n(std::launder(data.get()), size());
    }
    full = false;
    start = 0;
  }

  T& operator[](const size_t i) {
    return *std::launder(data.get() + index(i));
  }

  T const& operator[](const size_t i) const {
    return *std::launder(data.get() + index(i));
  }

  size_t size() const noexcept {
    return full ? size_ : start;
  }

  template <typename... Args>
  void emplace(Args&&... args) {
    if (!full) {
      std::construct_at(data.get() + start, std::forward<Args>(args)...);
    }
    else {
      // construct-and-move so that the container remains in a consistent state
      // on exception.
      data[start] = T(std::forward<Args>(args)...);
    }
    if (++start == size_) {
      full = true;
      start = 0;
    }
  }

  iterator begin() noexcept {
    static_assert(std::forward_iterator<iterator>);
    return iterator(this, 0);
  }

  iterator end() noexcept {
    return iterator(this, size());
  }

 private:
  inline size_t index(const size_t i) const noexcept {
    return full ? (start + i) % size_ : i;
  }

  struct Deleter {
    void operator()(T* ptr) const noexcept {
      ::operator delete(ptr, std::align_val_t(alignof(T)));
    }
  };

  std::unique_ptr<T[], Deleter> alloc() {
    if (!size_) {
      return nullptr;
    }
    if (size_ > std::numeric_limits<ssize_t>::max()) [[unlikely]] {
      throw std::runtime_error{"CircleQueue capacity too large"};
    }
    return std::unique_ptr<T[], Deleter>(
        static_cast<T*>(
            ::operator new(size_ * sizeof(T), std::align_val_t(alignof(T)))),
        Deleter());
  }

  const size_t size_;
  bool full : 1 = false;
  size_t start : std::numeric_limits<ssize_t>::digits = 0;
  const std::unique_ptr<T[], Deleter> data;
};

template <typename T>
class CircleQueue<T>::iterator {
 public:
  using value_type = T;
  using difference_type = ssize_t;

  constexpr iterator() noexcept
      : obj(nullptr), i(1ull << std::numeric_limits<ssize_t>::digits) {}

  iterator(iterator const& r) noexcept: obj(r.obj), i(r.i) {}

  iterator& operator=(const iterator r) noexcept {
    obj = r.obj;
    i = r.i;
    return *this;
  }

  T& operator*() const {
    return (*obj)[i];
  }

  T* operator->() const {
    return std::addressof((*obj)[i]);
  }

  iterator& operator++() noexcept {
    ++i;
    return *this;
  }

  iterator operator++(int) noexcept {
    iterator r(*this);
    ++*this;
    return r;
  }

  bool operator==(const iterator r) const noexcept {
    return i == r.i && obj == r.obj;
  }

 private:
  iterator(CircleQueue* obj, size_t i) noexcept: obj(obj), i(i) {}
  friend class CircleQueue;

  CircleQueue* obj;
  size_t i;
};
