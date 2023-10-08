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
      : size_(capacity), full(false), start(0) {
    static_assert(sizeof(*this) == 2 * sizeof(size_t) + sizeof(T*));
    if (!capacity) {
      data = nullptr;
      return;
    }
    if (capacity > std::numeric_limits<ssize_t>::max()) {
      throw std::runtime_error{"CircleQueue capacity too large"};
    }
    data = A::allocate(size_);
  }

  ~CircleQueue() /* noexcept */ {
    if (data) {
      clear();
      A::deallocate(data, size_);
    }
  }

  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      std::ranges::destroy_n(std::launder(data), size());
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
  void emplace(Args&&... args) {
    if (!full) {
      std::construct_at(data + start, std::forward<Args>(args)...);
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
  T* data;  // Owned by this. Not a unique_ptr since there seems to be no
            // straightforward way to have an empty deleter.
};

template <typename T>
class CircleQueue<T>::iterator {
 public:
  using value_type = T;
  using difference_type = ssize_t;

  iterator(): obj(nullptr), i(std::numeric_limits<size_t>::max()) {}

  iterator(iterator const& r) noexcept: obj(r.obj), i(r.i) {}

  iterator& operator=(iterator r) noexcept {
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
    iterator r;
    ++*this;
    return r;
  }

  bool operator==(iterator r) const noexcept {
    assert(obj == r.obj);
    return i == r.i;
  }

 private:
  iterator(CircleQueue* obj, size_t i): obj(obj), i(i) {
    // XX actually a random-access iterator but we don't care.
    static_assert(std::forward_iterator<iterator>);
  }
  friend class CircleQueue;

  CircleQueue* obj;
  size_t i;
};
