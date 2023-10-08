#include <absl/strings/string_view.h>
#include <re2/re2.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
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

#include "circle_queue.h"
#include "io.h"
#include "job.h"
#include "opts.h"

namespace fs = std::filesystem;

using namespace std::string_view_literals;

#define FWD(x) std::forward<decltype(x)>(x)

namespace {

constexpr std::string_view BOLD_ON { "\x1b[1m" };
constexpr std::string_view BOLD_OFF { "\x1b[0m" };

[[noreturn]] void version() {
  mPrintLn("gr version 0.2.0");
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
  explicit SyncedRe(std::string_view pattern, const RE2::Options& options)
      : pattern(std::move(pattern)), options(options) {}

  operator const re2::RE2&() const {
    init();
    return *expr;
  }

  inline void init() const {  // must be const since it's called from ^
    std::call_once(compile_expr, [this]{
      expr = std::make_unique<re2::RE2>(to_absl(pattern), options);
      if (!expr->ok()) {
        mPrintLn(std::cerr, "Failed to compile regexp /{}/: {}",
                 pattern, expr->error());
        exit(2);
      }
    });
  }

 private:
  const std::string_view pattern;
  const RE2::Options& options;
  mutable std::unique_ptr<re2::RE2> expr;
  mutable std::once_flag compile_expr;
};

struct GlobalState {
  const Opts opts;
  const SyncedRe expr;
  WorkQueue queue;
  std::atomic_flag matched_one = ATOMIC_FLAG_INIT;
};

class CompileReJob : public Job {
 public:
  explicit CompileReJob(const GlobalState& state): state(state) {}

  void operator()() override {
    state.expr.init();
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
    if (state.opts.multiline
        && !re2::RE2::PartialMatch(to_absl(view), state.expr)) {
      return;
    }

    if (state.opts.lflag) {
      (void) state.matched_one.test_and_set();
      mPrintLn("{}", pretty_path());
      return;
    }

    // TODO multiline
    size_t line = 0;
    size_t last_match = SIZE_MAX;
    struct Match {
      size_t line;
      std::string_view text;
      bool truncated;
      bool is_context;
    };
    struct Context {
      std::string_view text;
      bool truncated;
    };
    std::vector<Match> matches;
    CircleQueue<Context> before_context(state.opts.before_context);
    uint8_t maxWidth = 0;
    auto try_add_match = [&, this](size_t end) {
      auto text = truncate_span(view, end);
      if (re2::RE2::PartialMatch(to_absl(text), state.expr)) {
        auto pre_line = line - before_context.size();
        for (const auto [pre_text, trunc]: before_context) {
          matches.emplace_back(pre_line++, pre_text, trunc, true);
        }
        before_context.clear();
        matches.emplace_back(line, text, end != text.size(), false);
        maxWidth = calcWidth(line);
        last_match = 0;
      }
      else if (last_match < state.opts.after_context) {
        ++last_match;
        matches.emplace_back(line, text, end != text.size(), true);
      }
      else {
        if (state.opts.before_context) {
          before_context.emplace(text, end != text.size());
        }
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

    if (!state.opts.multiline && matches.empty()) {
      return;
    }

    const auto bold_on = state.opts.stdout_is_tty ? BOLD_ON : ""sv;
    const auto bold_off = state.opts.stdout_is_tty ? BOLD_OFF : ""sv;

    std::lock_guard lk(io_mutex);
    if (state.matched_one.test_and_set()) {
      mPrintLn("");
    }
    mPrintLn("{}{}{}", bold_on, pretty_path(), bold_off);
    if (matches.size()) {
      auto last_line = 0uz;
      for (auto [line, text, truncated, is_context]: matches) {
        if ((state.opts.before_context || state.opts.after_context)
            && last_line && line != last_line + 1) {
          mPrintLn("--");
        }
        last_line = line;
        static const auto ellipses = reinterpret_cast<const char*>(u8"…");
        const auto delim = is_context ? '-' : ':';
        const auto pre_line = is_context ? ""sv : bold_on;
        const auto post_line = is_context ? ""sv : bold_off;
        const auto pre_trunc = truncated ? bold_on : ""sv;
        const auto post_trunc = truncated ? bold_off : ""sv;
        const auto trunc = truncated ? ellipses : "";
        mPrintLn("{}{:{}}{}" "{}{}" "{}{}{}",
                 pre_line, line, maxWidth, post_line,
                 delim, text,
                 pre_trunc, trunc, post_trunc);
      }
    }
    else {
      mPrintLn("(file matched, but no lines matched)");
    }
  }

  std::string_view truncate_span(std::string_view view, size_t end) {
    if (state.opts.llflag || end <= 2048uz) {
      return std::string_view(view.begin(), end);
    }
    std::string_view ret(view.begin(), 2048uz);
    // try to truncate to the nearest UTF-8 code point
    auto it = std::rbegin(ret);
    int i = 0;
    // scan until we're at something that is not a utf8-tail
    for (; i < 4 && (*it & 0xc0) == 0x80; ++it, ++i) { }
    static constexpr std::array<std::pair<uint8_t, uint8_t>, 5> mask_check {{
      {0x80, 0},      // 1 from end: must be ASCII
      {0xe0, 0xc0},   // 2 from end: ok if it's a 2-byte code point
      {0xf0, 0xe0},   // 3 from end
      {0xf8, 0xf0},   // 4 from end
      {0, 0},         // 5? TODO(display): not valid utf8; passthru
    }};
    assert(i < 5);
    auto [mask, check] = mask_check[i];
    if ((*it & mask) != check) {
      ret.remove_suffix(i + 1);
    }
    return ret;
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
  AddPathsJob(GlobalState& state, auto&& path, bool requested,
              std::optional<fs::file_status> ss_ = std::nullopt)
      : state(state), path(FWD(path)), requested(requested), ss_(ss_) {}

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
    if (!requested && is_ignored()) {
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
                state, std::move(*it), false, std::move(itS)));
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
  const fs::path path;
  const bool requested;
  const std::optional<fs::file_status> ss_;
};

struct JobRunner {
  explicit JobRunner(WorkQueue& queue): queue(queue) {}

  void operator()() {
    queue.runUntilEmpty();
  }

  WorkQueue& queue;
};

}   // namespace

int main(int const argc, char const* argv[]) {
  auto opts = std::make_unique<Opts>();
  try {
    ArgParser::parse_args(argc, argv, *opts);
  }
  catch (const ArgumentError& e) {
    mPrintLn(std::cerr, "{}: {}", opts->argv0, e.reason);
    usage(opts->argv0);
  }
  if (opts->hflag) {
    usage(opts->argv0);
  }
  if (opts->version) {
    version();
  }
  const auto nThreads = std::thread::hardware_concurrency();
  auto options = RE2::Options();
  options.set_literal(opts->qflag);
  const auto pattern = opts->pattern;
  auto state = GlobalState{std::move(*opts), SyncedRe(pattern, options), {}};
  opts.reset();
  if (!state.opts.paths.size()) {
    state.queue.push(std::make_unique<AddPathsJob>(state, ".", true));
  }
  for (const auto path: state.opts.paths) {
    state.queue.push(std::make_unique<AddPathsJob>(state, path, true));
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
