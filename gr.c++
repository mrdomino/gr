#include <algorithm>
#include <array>
#include <atomic>
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

struct Error {
  std::string reason;
};

struct Params {
  std::string_view pattern;
  std::optional<std::vector<std::string_view>> paths;
  bool stdout_is_tty = false;
  bool hflag = false;
  bool lflag = false;
  bool llflag = false;
  bool version = false;
};

struct Args {
  std::string_view argv0;
  std::expected<Params, Error> params;
};

struct ArgParser {
  using arg_func = bool (*)(Args&);
  static constexpr arg_func noop = [](Args&) { return true; };
  static constexpr arg_func do_hflag = [](Args& args) {
    args.params->hflag = true;
    return false;
  };
  static constexpr arg_func do_lflag = [](Args& args) {
    args.params->lflag = true;
    return true;
  };
  static constexpr arg_func do_llflag = [](Args& args) {
    args.params->llflag = true;
    return true;
  };
  static constexpr arg_func do_version = [](Args& args) {
    args.params->version = true;
    return false;
  };

  static constexpr std::array long_opts {
    std::pair {""sv, noop},
    std::pair {"files-with-matches"sv, do_lflag},
    std::pair {"help"sv, do_hflag},
    std::pair {"long-lines"sv, do_llflag},
    std::pair {"version"sv, do_version},
  };

  static constexpr std::array short_opts {
    std::pair {'h', do_hflag},
    std::pair {'l', do_lflag},
  };

  static constexpr bool parse_long_opt(Args& args, std::string_view opt) {
    auto it = std::lower_bound(
        std::begin(long_opts), std::end(long_opts), opt,
        [](auto p, auto o) {
          return p.first < o;
        });
    if (it != std::end(long_opts) && it->first == opt) {
      return it->second(args);
    }
    args.params = std::unexpected{
        Error{std::format("unrecognized option --{}", opt)}};
    return false;
  }

  static constexpr bool parse_short_opts(Args& args, std::string_view opts) {
    for (auto c: opts) {
      auto it = std::lower_bound(
          std::begin(short_opts), std::end(short_opts), c,
          [](auto p, auto c) {
            return p.first < c;
          });
      if (it != std::end(short_opts) && it->first == c) {
        if (!it->second(args)) {
          return false;
        }
      }
      else {
        args.params = std::unexpected{
            Error{std::format("invalid option -{}", c)}};
        return false;
      }
    }
    return true;
  }

  static Args parse_args(int argc, char const* argv[]) {
    Args ret;
    ret.argv0 = argv[0];
    ++argv; --argc;
    ret.params.emplace();
    int postOpts = argc;
    int postDash = argc;
    for (auto it = argv; it < argv + argc; ++it) {
      if (*it == "--"sv) {
        postDash = it - argv + 1;
        postOpts = it - argv + 1;
        break;
      }
    }
    for (auto it = argv; it < argv + postOpts;) {
      std::string_view arg(*it);
      if (!arg.starts_with('-')) {
        std::swap(*it, *(argv + --postOpts));
        continue;
      }
      arg.remove_prefix(1);
      if (!arg.starts_with('-')) {
        if (!parse_short_opts(ret, arg)) {
          break;
        }
      }
      else {
        arg.remove_prefix(1);
        if (!parse_long_opt(ret, arg)) {
          break;
        }
      }
      ++it;
    }
    if (!ret.params.has_value() || ret.params->hflag || ret.params->version) {
      return ret;
    }
    std::reverse(argv + postOpts, argv + postDash);
    argv += postOpts; argc -= postOpts;
    if (!argc) {
      ret.params = std::unexpected{Error{"missing pattern"}};
      return ret;
    }
    ret.params->pattern = *argv++; --argc;
    if (argc) {
      ret.params->paths.emplace(argv, argv + argc);
    }
    ret.params->stdout_is_tty = isatty(fileno(stdout));
    return ret;
  }
};

[[noreturn]] void usage(Args&& args) {
  args.params.transform_error([argv0=args.argv0](auto&& e) {
    mPrintLn(std::cerr, "{}: {}", argv0, e.reason);
    return e;
  });
  mPrintLn(std::cerr, "usage: {} [options] <pattern> [path ...]", args.argv0);
  mPrintLn(
      std::cerr,

      "\nRecursively search for pattern in path.\n"
      "Uses the re2 regular expression library.\n\n"

      "Options:\n"
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
  CompileReJob(GlobalState const& state): state(state) {}

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
    std::unique_ptr<char[]> contents;
    auto fs = std::fstream(path, std::ios_base::in);
    fs.exceptions(fs.failbit | fs.badbit);
    fs.seekg(0, std::ios_base::end);
    const size_t len = fs.tellg();
    fs.seekg(0);
    contents = std::make_unique_for_overwrite<char[]>(len);
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
        state.queue.push(
            std::make_unique<SearchJob>(state, std::move(path)));
      }
      else {
        mPrintLn(std::cerr, "Skipping {}: Permission denied", path.string());
      }
    }
    else if (fs::is_directory(s)) {
      for (auto it{fs::directory_iterator(std::move(path))};
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
  auto args = ArgParser::parse_args(argc, argv);
  if (!args.params.has_value() || args.params->hflag) {
    usage(std::move(args));
  }
  auto& params = args.params.value();
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
