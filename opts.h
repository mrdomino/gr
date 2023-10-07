#include <charconv>
#include <format>
#include <optional>
#include <string_view>
#include <variant>

using namespace std::string_view_literals;

struct ArgumentError: std::exception {
  std::string reason;

  explicit ArgumentError(std::string&& reason): reason(std::move(reason)) {}
};

struct Opts {
  std::string_view argv0;
  std::string_view pattern;
  std::optional<std::vector<std::string_view>> paths;
  bool stdout_is_tty = false;
  uint16_t before_context = 0;
  uint16_t after_context = 0;
  bool hflag = false;
  bool lflag = false;
  bool llflag = false;
  bool version = false;
};

struct ArgParser {
  using opt_func = void (*)(Opts&);
  using arg_func = void (*)(Opts&, std::string_view);
  using func = std::variant<opt_func, arg_func>;
  static constexpr auto read_int = [](auto& value, std::string_view arg) {
    auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(),
                                     value);
    if (ec != std::errc() || ptr != arg.data() + arg.size()) {
      throw ArgumentError{std::format("invalid number: '{}'", arg)};
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
  static constexpr opt_func do_hflag = [](Opts& o) {
    o.hflag = true;
  };
  static constexpr opt_func do_lflag = [](Opts& o) {
    o.lflag = true;
  };
  static constexpr opt_func do_llflag = [](Opts& o) {
    o.llflag = true;
  };
  static constexpr opt_func do_version = [](Opts& o) {
    o.version = true;
  };

  static constexpr std::array long_opts {
    std::pair {"after-context"sv, func(do_aflag)},
    std::pair {"before-context"sv, func(do_bflag)},
    std::pair {"context"sv, func(do_cflag)},
    std::pair {"files-with-matches"sv, func(do_lflag)},
    std::pair {"help"sv, func(do_hflag)},
    std::pair {"long-lines"sv, func(do_llflag)},
    std::pair {"version"sv, func(do_version)},
  };

  static constexpr std::string_view short_opt_chars { "ABChl" };
  static constexpr std::array<func, short_opt_chars.size()> short_opts {
    do_aflag,
    do_bflag,
    do_cflag,
    do_hflag,
    do_lflag,
  };

  static constexpr std::pair<std::string_view, func>
  lookup_long_opt(std::string_view opt);

  static void swap_portions(char const* argv[], int& first_nonopt,
                            int& last_nonopt, int optind);

  static void parse_args(const int argc, char const* argv[], Opts& opts);
};

[[noreturn]] void usage(std::string_view argv0);
