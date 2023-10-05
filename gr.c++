#include <cmath>

#include <algorithm>
#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <absl/strings/string_view.h>
#include <re2/re2.h>

#include "io.h"
#include "job.h"

namespace fs = std::filesystem;

#define FWD(x) std::forward<decltype(x)>(x)

#define BOLD_ON "\x1b[1m"
#define BOLD_OFF "\x1b[0m"

struct Error {
  std::string reason;
};

struct Params {
  std::string pattern;
  std::optional<std::vector<std::string>> paths;
  bool stdout_is_tty;
};

struct Args {
  std::string argv0;
  std::expected<Params, Error> params;
};

Args parse_args(int const argc, char const* const argv[]) {
  Args ret;
  ret.argv0 = argv[0];
  if (argc < 2) {
    ret.params = std::unexpected{Error{"missing pattern"}};
    return ret;
  }
  Params params;
  params.pattern = argv[1];
  if (argc > 2) {
    params.paths.emplace(argv + 2, argv + argc);
  }
  params.stdout_is_tty = isatty(fileno(stdout));
  ret.params = std::move(params);
  return ret;
}

[[noreturn]] void usage(Args&& args) {
  mPrintLn(std::cerr, "{0}: {1}\nusage: {0} <pattern> [filename...]",
           args.argv0, args.params.error().reason);
  exit(2);
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

class SyncedRe {
 public:
  explicit SyncedRe(std::string pattern): pattern(std::move(pattern)) {}

  operator const re2::RE2&() const {
    std::call_once(compile_expr, [this]{
      expr = std::make_unique<re2::RE2>(pattern);
      if (!expr->ok()) {
        mPrintLn(std::cerr, "Failed to compile regexp /{}/: {}",
                 pattern, expr->error());
        exit(2);
      }
      pattern.clear();
    });
    return *expr;
  }

 private:
  mutable std::string pattern;
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
    if (!static_cast<const re2::RE2&>(state.expr).ok()) {
      // Should be impossible; abort to have an effect.
      abort();
    }
  }

 private:
  const GlobalState& state;
};

inline constexpr absl::string_view to_absl(std::string_view view) {
  return absl::string_view(view.begin(), view.size());
}

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
      auto text = std::string_view(view.begin(), std::min(2048uz, end));
      if (re2::RE2::PartialMatch(to_absl(text), state.expr)) {
        matches.emplace_back(line, text, end != text.size());
        maxWidth = std::ceil(std::log10(line + 1));
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
    for (auto [line, text, truncated]: matches) {
      const auto trunc =
          truncated ? reinterpret_cast<const char*>(u8"…") : "";
      if (state.params.stdout_is_tty) {
        mPrintLn(BOLD_ON "{:{}}" BOLD_OFF ":{}" BOLD_ON "{}" BOLD_OFF,
                 line, maxWidth, text, trunc);
      }
      else mPrintLn("{:{}}:{}{}", line, maxWidth, text, trunc);
    }
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
            std::make_unique<SearchJob>(
                state, std::move(path)));
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

int main(int const argc, char const* const argv[]) {
  auto args = parse_args(argc, argv);
  if (!args.params.has_value()) {
    usage(std::move(args));
  }
  auto& params = args.params.value();
  const auto nThreads = std::thread::hardware_concurrency();
  auto state = GlobalState{params, SyncedRe(params.pattern), {}};
  auto paths = params.paths.value_or(std::vector<std::string>{"."});
  for (auto&& path: std::move(paths)) {
    state.queue.push(std::make_unique<AddPathsJob>(state, std::move(path)));
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
