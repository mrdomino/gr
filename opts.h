#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace std::string_view_literals;

struct ArgumentError: std::runtime_error {
  template <typename... Args>
  explicit ArgumentError(std::format_string<Args...> fmt, Args&&... args)
      : runtime_error(std::format(fmt, std::forward<Args>(args)...)) {}
};

struct Opts {
  std::string_view argv0;
  std::string_view pattern;
  std::vector<std::string_view> paths;
  bool stdout_is_tty = false;
  uint16_t before_context = 0;
  uint16_t after_context = 0;
  bool count = false;
  bool hflag = false;
  bool lflag = false;
  bool llflag = false;
  bool multiline = false;
  bool qflag = false;
  bool version = false;

  Opts() = default;
  Opts(Opts&&) = default;
};

struct ArgParser {
  using opt_func = void (*)(Opts&);
  using arg_func = void (*)(Opts&, std::string_view);
  using func = std::variant<opt_func, arg_func>;
  static constexpr auto read_int = [](auto& value, std::string_view arg) {
    auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(),
                                     value);
    if (ec != std::errc() || ptr != arg.data() + arg.size()) {
      throw ArgumentError{"invalid number: '{}'", arg};
    }
  };
  static constexpr arg_func do_aflag = [](Opts& o, std::string_view arg) {
    read_int(o.after_context, arg);
  };
  static constexpr arg_func do_bflag = [](Opts& o, std::string_view arg) {
    read_int(o.before_context, arg);
  };
  static constexpr arg_func do_cflag = [](Opts& o, std::string_view arg) {
    read_int(o.after_context, arg);
    o.before_context = o.after_context;
  };
  static constexpr opt_func do_count = [](Opts& o) { o.count = true; };
  static constexpr opt_func do_hflag = [](Opts& o) { o.hflag = true; };
  static constexpr opt_func do_lflag = [](Opts& o) { o.lflag = true; };
  static constexpr opt_func do_llflag = [](Opts& o) { o.llflag = true; };
  static constexpr opt_func do_qflag = [](Opts& o) { o.qflag = true; };
  static constexpr opt_func do_multiline = [](Opts& o) { o.multiline = true; };
  static constexpr opt_func do_version = [](Opts& o) { o.version = true; };

  static constexpr std::array long_opts {
    std::pair {"after-context"sv, func(do_aflag)},
    std::pair {"before-context"sv, func(do_bflag)},
    std::pair {"context"sv, func(do_cflag)},
    std::pair {"count"sv, func(do_count)},
    std::pair {"files-with-matches"sv, func(do_lflag)},
    std::pair {"help"sv, func(do_hflag)},
    std::pair {"literal"sv, func(do_qflag)},
    std::pair {"long-lines"sv, func(do_llflag)},
    std::pair {"multiline"sv, func(do_multiline)},
    std::pair {"version"sv, func(do_version)},
  };

  static constexpr auto short_opt_chars { "ABCQchl"sv };
  static constexpr std::array<func, short_opt_chars.size()> short_opts {
    do_aflag,
    do_bflag,
    do_cflag,
    do_qflag,
    do_count,
    do_hflag,
    do_lflag,
  };

  // Guaranteed to populate opts.argv0, even if an exception is thrown.
  //
  static void parse_args(const int argc, char const* argv[], Opts& opts);
};

[[noreturn]] void usage(std::string_view argv0);
