#include <cstddef>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <absl/strings/string_view.h>
#include <re2/re2.h>

#include "io.h"
#include "job.h"

namespace fs = std::filesystem;

using namespace std::string_view_literals;

#define FWD(x) std::forward<decltype(x)>(x)

#define BOLD_ON "\x1b[1m"
#define BOLD_OFF "\x1b[0m"

struct ArgumentError: std::exception {
  std::string reason;

  explicit ArgumentError(auto&& reason): reason(FWD(reason)) {}
};

struct Params {
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
  using arg_func = bool (*)(Params&, std::string_view);
  static constexpr arg_func noop = [](Params&, std::string_view) {
    return true;
  };
  static constexpr auto read_int = [](auto& value, std::string_view arg) {
    auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(),
                                     value);
    if (ec != std::errc() || ptr != arg.data() + arg.size()) {
      throw ArgumentError{std::format("invalid number: {}", arg)};
    }
    return true;
  };
  static constexpr arg_func do_aflag = [](Params& p,
                                          std::string_view arg) {
    return read_int(p.after_context, arg);
  };
  static constexpr arg_func do_bflag = [](Params& p, std::string_view arg) {
    return read_int(p.before_context, arg);
  };
  static constexpr arg_func do_cflag = [](Params& p, std::string_view arg) {
    if (!read_int(p.after_context, arg)) {
      return false;
    }
    p.before_context = p.after_context;
    return true;
  };
  static constexpr arg_func do_hflag = [](Params& p, std::string_view) {
    p.hflag = true;
    return false;
  };
  static constexpr arg_func do_lflag = [](Params& p, std::string_view) {
    p.lflag = true;
    return true;
  };
  static constexpr arg_func do_llflag = [](Params& p, std::string_view) {
    p.llflag = true;
    return true;
  };
  static constexpr arg_func do_version = [](Params& p, std::string_view) {
    p.version = true;
    return false;
  };

  static constexpr std::array long_opts {
    std::tuple {""sv, noop, false},
    std::tuple {"after-context"sv, do_aflag, true},
    std::tuple {"before-context"sv, do_bflag, true},
    std::tuple {"context"sv, do_cflag, true},
    std::tuple {"files-with-matches"sv, do_lflag, false},
    std::tuple {"help"sv, do_hflag, false},
    std::tuple {"long-lines"sv, do_llflag, false},
    std::tuple {"version"sv, do_version, false},
  };

  static constexpr std::string_view short_opt_chars { "ABChl" };
  static constexpr std::array<std::pair<arg_func, bool>, short_opt_chars.size()>
  short_opts {
    std::pair { do_aflag, true },
    std::pair { do_bflag, true },
    std::pair { do_cflag, true },
    std::pair { do_hflag, false },
    std::pair { do_lflag, false },
  };

  static constexpr auto lookup_long_opt(std::string_view opt) {
    auto it = std::lower_bound(std::begin(long_opts), std::end(long_opts),
                               opt,
                               [](auto a, auto b) {
                                 return std::get<0>(a) < b;
                               });
    if (it != std::end(long_opts) && std::get<0>(*it).starts_with(opt)) {
      if (it + 1 != std::end(long_opts)
          && std::get<0>(*(it + 1)).starts_with(opt)) {
        throw ArgumentError{std::format("ambiguous option --{}", opt)};
      }
      return *it;
    }
    throw ArgumentError{std::format("unrecognized option --{}", opt)};
  }

  static void swap_portions(char const* argv[], int& first_nonopt,
                            int& last_nonopt, int optind) {
    int bottom = first_nonopt;
    int middle = last_nonopt;
    int top = optind;

    while (top > middle && middle > bottom) {
      if (top - middle > middle - bottom) {
        std::swap_ranges(argv + bottom, argv + middle,
                         argv + top - (middle - bottom));
        top -= (middle - bottom);
      }
      else {
        std::swap_ranges(argv + bottom, argv + bottom + (top - middle),
                         argv + middle);
        bottom += (top - middle);
      }
    }
    first_nonopt += (optind - last_nonopt);
    last_nonopt = optind;
  }

  static void parse_args(const int argc, char const* argv[], Params& params) {
    params.argv0 = *argv;
    int optind = 1;
    int first_nonopt = 1;
    int last_nonopt = 1;
    while (true) {
      if (first_nonopt != last_nonopt && last_nonopt != optind) {
        swap_portions(argv, first_nonopt, last_nonopt, optind);
      }
      else if (last_nonopt != optind) {
        first_nonopt = optind;
      }
      std::string_view opt;
      while (optind < argc && !(opt = argv[optind]).starts_with('-')) {
        ++optind;
      }
      last_nonopt = optind;

      if (optind != argc && opt == "--") {
        ++optind;
        if (first_nonopt != last_nonopt && last_nonopt != optind) {
          swap_portions(argv, first_nonopt, last_nonopt, optind);
        }
        else if (first_nonopt == last_nonopt) {
          first_nonopt = optind;
        }
        last_nonopt = argc;
        optind = argc;
      }

      if (optind == argc) {
        if (first_nonopt != last_nonopt) {
          optind = first_nonopt;
        }
        break;
      }

      opt.remove_prefix(1);
      if (opt.starts_with('-')) {
        opt.remove_prefix(1);
        auto eq = opt.find('=');
        auto arg = [=]{
          if (eq != opt.npos) {
            return std::string_view(opt.begin() + eq + 1, opt.size() - eq - 1);
          }
          return std::string_view();
        }();
        if (eq != opt.npos) {
          opt.remove_suffix(opt.size() - eq);
        }
        auto [_, func, has_arg] = lookup_long_opt(opt);
        ++optind;
        if (eq != opt.npos) {
          if (!has_arg) {
            throw ArgumentError{std::format("--{} takes no argument", opt)};
          }
        }
        else if (has_arg) {
          if (optind < argc) {
            arg = argv[optind++];
          }
          else {
            throw ArgumentError{std::format("--{} requires an argument", opt)};
          }
        }
        func(params, arg);
      }
      else {
        while (opt.size()) {
          const auto c = opt.front();
          const auto i = short_opt_chars.find(c);
          opt.remove_prefix(1);
          if (!opt.size()) {
            ++optind;
          }
          if (i == short_opt_chars.npos) {
            throw ArgumentError{std::format("invalid option -{}", c)};
          }
          auto [func, has_arg] = short_opts[i];
          std::string_view arg;
          if (has_arg) {
            if (opt.size()) {
              arg = opt;
              opt = std::string_view();
              ++optind;
            }
            else if (optind == argc) {
              throw ArgumentError{std::format("-{} requires an argument", c)};
            }
            else {
              arg = argv[optind++];
            }
          }
          func(params, arg);
        }
      }
    }
    if (params.hflag || params.version) {
      return;
    }
    if (optind == argc) {
      throw ArgumentError{"missing pattern"};
    }
    params.pattern = argv[optind++];
    if (optind < argc) {
      params.paths.emplace(argv + optind, argv + argc);
    }
    params.stdout_is_tty = isatty(fileno(stdout));
  }
};

[[noreturn]] void usage(std::string_view argv0) {
  mPrintLn(std::cerr, "usage: {} [options] <pattern> [path ...]", argv0);
  mPrintLn(
      std::cerr,

      "\nRecursively search for pattern in path.\n"
      "Uses the re2 regular expression library.\n\n"

      "Options:\n"
#if 0
      "  -A --after-context <num> Show num lines of context after each match\n"
      "  -B --before-context <num>\n"
      "                           Show num lines of context before each match\n"
      "  -C --context <num>       Show num lines before and after each match\n"
#endif
      "  -l --files-with-matches  Only print filenames that contain matches\n"
      "                           (don't print the matching lines)\n"
      "     --long-lines          Print long lines (default truncates to ~2k)\n"
      "  -h --help                Print this usage message and exit.\n"
      "     --version             Print the program version.\n"
  );
  exit(2);
}

[[noreturn]] void version() {
  mPrintLn("gr version 0.1.0");
  exit(0);
}

static bool is_binary(std::string_view buf) {
  if (!buf.size()) {
    return false;
  }
  if (buf.starts_with("\xef\xbb\xbf")) {
    // UTF-8 BOM
    return false;
  }
  if (buf.starts_with("%PDF-")) {
    return true;
  }
  if (buf.find('\0') != buf.npos) {
    return true;
  }
  return false;
}

inline constexpr absl::string_view to_absl(std::string_view view) {
  return absl::string_view(view.begin(), view.size());
}

class SyncedRe {
 public:
  explicit SyncedRe(std::string_view pattern): pattern(std::move(pattern)) {}

  operator const re2::RE2&() const {
    std::call_once(compile_expr, [this]{
      expr = std::make_unique<re2::RE2>(to_absl(pattern));
      if (!expr->ok()) {
        mPrintLn(std::cerr, "Failed to compile regexp /{}/: {}",
                 pattern, expr->error());
        exit(2);
      }
    });
    return *expr;
  }

 private:
  mutable std::string_view pattern;
  mutable std::unique_ptr<re2::RE2> expr;
  mutable std::once_flag compile_expr;
};

struct GlobalState {
  const Params params;
  const SyncedRe expr;
  WorkQueue queue;
  std::atomic_flag matched_one = ATOMIC_FLAG_INIT;
};

class CompileReJob : public Job {
 public:
  CompileReJob(const GlobalState& state): state(state) {}

  void operator()() override {
    // Just observe the expression to trigger the call_once. The abort() should
    // be unreachable, and is there to try to prevent this from being optimized
    // out.
    if (!static_cast<const re2::RE2&>(state.expr).ok()) {
      abort();
    }
  }

 private:
  const GlobalState& state;
};

class SearchJob : public Job {
 public:
  SearchJob(GlobalState& state, auto&& path)
      : state(state), path(FWD(path)) {}

  void operator()() override {
    try {
      run_unchecked();
    }
    catch (const std::ios_base::failure& e) {
      mPrintLn(std::cerr, "Error on {}: {}", path.string(), e.what());
    }
    catch (const std::exception& e) {
      mPrintLn(std::cerr,
               "Unexpected exception on {}: {}", path.string(), e.what());
      throw;
    }
  }

 private:
  void run_unchecked() {
    auto fs = std::fstream(path, std::ios_base::in);
    fs.exceptions(fs.failbit | fs.badbit);
    fs.seekg(0, std::ios_base::end);
    const size_t len = fs.tellg();
    fs.seekg(0);
    const auto contents = std::make_unique_for_overwrite<char[]>(len);
    const auto pre_len = std::min(512uz, len);
    fs.read(contents.get(), pre_len);
    if (is_binary(std::string_view(contents.get(), pre_len))) {
      return;
    }
    if (len != pre_len) {
      fs.seekg(0);
      fs.read(contents.get(), len);
    }
    std::string_view view(contents.get(), len);
    if (!re2::RE2::PartialMatch(to_absl(view), state.expr)) {
      return;
    }

    if (state.params.lflag) {
      (void) state.matched_one.test_and_set();
      mPrintLn("{}", pretty_path());
      return;
    }

    // TODO multiline
    size_t line = 0;
    struct Match {
      size_t line;
      std::string_view text;
      bool truncated;
    };
    std::vector<Match> matches;
    uint8_t maxWidth = 0;
    auto try_add_match = [&, this](size_t end) {
      auto text = truncate_span(view, end);
      if (re2::RE2::PartialMatch(to_absl(text), state.expr)) {
        matches.emplace_back(line, text, end != text.size());
        maxWidth = calcWidth(line);
      }
    };
    while (view.size()) {
      ++line;
      auto nl = view.find('\n');
      if (nl == view.npos) {
        try_add_match(view.size());
        break;
      }
      try_add_match(nl);
      view.remove_prefix(nl + 1);
    }

    std::lock_guard lk(io_mutex);
    if (state.matched_one.test_and_set()) {
      mPrintLn("");
    }
    if (state.params.stdout_is_tty) {
      mPrintLn(BOLD_ON "{}" BOLD_OFF, pretty_path());
    }
    else mPrintLn("{}", pretty_path());
    if (matches.size()) {
      for (auto [line, text, truncated]: matches) {
        if (state.params.stdout_is_tty) {
          mPrint(BOLD_ON "{:{}}" BOLD_OFF ":{}", line, maxWidth, text);
        }
        else mPrint("{:{}}:{}", line, maxWidth, text);
        if (truncated) {
          mPrintLn("{}", ellipses());
        }
        else mPrintLn("");
      }
    }
    else {
      mPrintLn("(matched too far into line to display)");
    }
  }

  std::string_view truncate_span(std::string_view view, size_t end) {
    if (state.params.llflag || end <= 2048uz) {
      return std::string_view(view.begin(), end);
    }
    std::string_view ret(view.begin(), 2048uz);
    // try to truncate to the nearest UTF-8 code point
    auto it = std::rbegin(ret);
    int i = 0;
    // scan until we're at something that is not a utf8-tail
    for (; i < 4 && (*it & 0xc0) == 0x80; ++it, ++i) { }
    static constexpr std::array<std::pair<uint8_t, uint8_t>, 5> mask_check {{
      {0b10000000, 0},            // 1 from end: must be ASCII
      {0b11100000, 0b11000000},   // 2 from end: ok if it's a 2-byte code point
      {0b11110000, 0b11100000},   // 3 from end
      {0b11111000, 0b11110000},   // 4 from end
      {0, 0},                     // 5? TODO(display): not valid utf8; passthru
    }};
    assert(i < 5);
    auto [mask, check] = mask_check[i];
    if ((*it & mask) != check) {
      ret.remove_suffix(i + 1);
    }
    return ret;
  }

  std::string_view ellipses() {
    static constexpr std::array<std::u8string_view, 2> ellipses {{
      u8"…",
      BOLD_ON u8"…" BOLD_OFF,
    }};
    const auto ret = ellipses[state.params.stdout_is_tty];
    return std::string_view(
        reinterpret_cast<const char*>(ret.data()), ret.size());
  }

  size_t calcWidth(size_t n) {
    if (n < 10) {
      return 1;
    }
    if (n < 100) {
      return 2;
    }
    if (n < 1000) {
      return 3;
    }
    if (n < 10000) {
      return 4;
    }
    if (n < 100000) {
      return 5;
    }
    if (n < 1000000) {
      return 6;
    }
    if (n < 10000000) {
      return 7;
    }
    return 8;
  }

  std::string pretty_path() const {
    if (*std::begin(path) == ".") {
      // XX surprisingly painful.... we don't want relative() since it
      // canonicalizes symlinks
      fs::path q;
      for (auto it = ++std::begin(path); it != std::end(path); ++it) {
        q += *it;
      }
      return q.string();
    }
    return path.string();
  }

  GlobalState& state;
  fs::path path;
};

class AddPathsJob : public Job {
 public:
  AddPathsJob(GlobalState& state, auto&& path,
              std::optional<fs::file_status> ss_ = std::nullopt)
      : state(state), path(FWD(path)), ss_(ss_) {}

  void operator()() override {
    try {
      run_unchecked();
    }
    catch (const fs::filesystem_error& e) {
      mPrintLn(std::cerr,
               "Skipping {}: error: {}", path.string(), e.code().message());
    }
    catch (const std::exception& e) {
      mPrintLn(std::cerr,
               "Unexpected exception on {}: {}", path.string(), e.what());
      throw;
    }
  }

 private:
  void run_unchecked() {
    if (is_ignored()) {
      return;
    }
    auto s = ss_.value_or(fs::status(path));
    if (fs::is_regular_file(s)) {
      if (0 == access(path.c_str(), R_OK)) {
        state.queue.push(std::make_unique<SearchJob>(state, path));
      }
      else {
        mPrintLn(std::cerr, "Skipping {}: Permission denied", path.string());
      }
    }
    else if (fs::is_directory(s)) {
      for (auto it{fs::directory_iterator(path)};
           it != fs::directory_iterator(); ++it) {
        auto itS = it->status();
        state.queue.push(
            std::make_unique<AddPathsJob>(
                state, std::move(*it), std::move(itS)));
      }
    }
    else if (!fs::exists(s)) {
      mPrintLn(std::cerr, "Skipping {}: nonexistent", path.string());
    }
  }

  bool is_ignored() const {
    auto name = path.filename().string();
    return name != "." && name != ".." && name.starts_with('.');
  }

  GlobalState& state;
  fs::path path;
  std::optional<fs::file_status> ss_;
};

struct JobRunner {
  JobRunner(WorkQueue& queue): queue(queue) {}

  void operator()() {
    queue.runUntilEmpty();
  }

  WorkQueue& queue;
};

int main(int const argc, char const* argv[]) {
  Params params;
  try {
    ArgParser::parse_args(argc, argv, params);
  }
  catch (const ArgumentError& e) {
    mPrintLn(std::cerr, "{}: {}", params.argv0, e.reason);
    usage(params.argv0);
  }
  if (params.hflag) {
    usage(params.argv0);
  }
  if (params.version) {
    version();
  }
  const auto nThreads = std::thread::hardware_concurrency();
  auto state = GlobalState{params, SyncedRe(params.pattern), {}};
  auto paths = params.paths.value_or(std::vector{"."sv});
  for (auto path: std::move(paths)) {
    state.queue.push(std::make_unique<AddPathsJob>(state, path));
  }
  state.queue.push(std::make_unique<CompileReJob>(state));
  std::vector<std::thread> threads;
  for (auto i = 0uz; i < nThreads; ++i) {
    threads.emplace_back(JobRunner(state.queue));
  }
  for (auto& thread: threads) {
    thread.join();
  }
  return state.matched_one.test() ? 0 : 1;
}
