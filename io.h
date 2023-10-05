#pragma once

#include <format>
#include <iostream>
#include <mutex>

extern std::recursive_mutex io_mutex;

template <typename... Args>
void mPrint(std::ostream& os, std::format_string<Args...> fmt, Args&&... args) {
  auto s = std::format(fmt, std::forward<Args>(args)...);
  std::lock_guard lk(io_mutex);
  os << std::move(s);
}

template <typename... Args>
void mPrintLn(std::ostream& os, std::format_string<Args...> fmt,
              Args&&... args) {
  auto s = std::format(fmt, std::forward<Args>(args)...);
  std::lock_guard lk(io_mutex);
  os << std::move(s) << '\n';
}

template <typename... Args>
void mPrint(std::format_string<Args...> fmt, Args&&... args) {
  mPrint(std::cout, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void mPrintLn(std::format_string<Args...> fmt, Args&&... args) {
  mPrintLn(std::cout, fmt, std::forward<Args>(args)...);
}
