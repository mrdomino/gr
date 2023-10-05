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

#include <absl/strings/string_view.h>
#include <re2/re2.h>

#include "io.h"
#include "job.h"

namespace fs = std::filesystem;

#define FWD(x) std::forward<decltype(x)>(x)

static const std::vector<std::string> ignored_files {
  ".git",
};

static bool is_ignored_file(std::string const& s) {
  return std::binary_search(
      std::begin(ignored_files), std::end(ignored_files), s);
}

static bool is_ignored(fs::path const& p) {
  return is_ignored_file(p.filename().string());
}

static bool is_binary(std::fstream& fs) {
  char buf[512];
  fs.seekg(0, std::ios_base::end);
  auto len = std::min(static_cast<size_t>(fs.tellg()), 512uz);
  fs.seekg(0);
  fs.read(buf, len);
  fs.seekg(0);
  if (len == 0) {
    return false;
  }
  if (len >= 3 && std::string(buf, buf + 3) == "\xef\xbb\xbf") {
    // UTF-8 BOM
    return false;
  }
  if (len >= 4 && std::string(buf, buf + 4) == "\x7f" "ELF") {
    // ELF header
    return true;
  }
  if (len >= 4 && std::string(buf, buf + 4) == "\xcf\xfa\xed\xfe") {
    // Mach-O header, 64-bit little-endian
    return true;
  }
  for (auto i = 0uz; i < len; ++i) {
    if (buf[i] == 0) {
      return true;
    }
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

class SearchJob : public Job {
 public:
  SearchJob(GlobalState& state, auto&& path)
      : state(state), path(FWD(path)) {}

  void operator()() override {
    std::unique_ptr<char[]> contents;
    auto fs = std::fstream(path, std::ios_base::in);
    if (is_binary(fs)) {
      return;
    }
    fs.seekg(0, std::ios_base::end);
    auto len = fs.tellg();
    fs.seekg(0);
    contents = std::make_unique_for_overwrite<char[]>(len);
    fs.read(contents.get(), len);
    if (fs.fail()) {
      mPrintLn("IO error on {}", path.string());
      return;
    }
    absl::string_view view(contents.get(), len);
    if (!re2::RE2::PartialMatch(view, state.expr)) {
      return;
    }

    // TODO multiline
    size_t line = 0;
    std::vector<std::pair<size_t, absl::string_view>> matches;
    while (view.size()) {
      ++line;
      auto nl = view.find('\n');
      if (nl == absl::string_view::npos) {
        auto text = absl::string_view(
            view.begin(), std::min(2048uz, view.size()));
        if (re2::RE2::PartialMatch(text, state.expr)) {
          matches.emplace_back(line, text);
        }
        break;
      }
      auto text = absl::string_view(view.begin(), std::min(2048uz, nl));
      if (re2::RE2::PartialMatch(text, state.expr)) {
        matches.emplace_back(line, text);
      }
      view.remove_prefix(nl + 1);
    }

    std::lock_guard lk(io_mutex);
    if (state.matched_one.test_and_set()) {
      mPrintLn("");
    }
    mPrintLn("{}", pretty_path());
    for (auto [line, text]: matches) {
      mPrintLn("{:3}:{}", line, std::string_view(text.begin(), text.size()));
    }
  }

 private:
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
    if (is_ignored(path)) {
      return;
    }
    auto s = ss_.value_or(fs::status(path));
    if (fs::is_regular_file(s)) {
      state.queue.push(
          std::make_unique<SearchJob>(
              state, std::move(path)));
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
    else {
      mPrintLn("Skipping {}", std::move(path).string());
    }
  }

 private:
  GlobalState& state;
  fs::path path;
  std::optional<fs::file_status> ss_;
};

struct Error {
  std::string reason;
};

struct Params {
  std::string pattern;
  std::optional<std::vector<std::string>> paths;
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
  ret.params = std::move(params);
  return ret;
}

[[noreturn]] void usage(Args&& args) {
  mPrintLn(std::cerr, "{0}: {1}\nusage: {0} <pattern> [filename...]",
           args.argv0, args.params.error().reason);
  exit(2);
}

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
  auto state = GlobalState{SyncedRe(std::move(params.pattern)), {}};
  auto paths = std::move(params).paths.value_or(std::vector<std::string>{"."});
  state.queue.push(std::make_unique<CompileReJob>(state));
  for (auto&& path: std::move(paths)) {
    state.queue.push(std::make_unique<AddPathsJob>(state, std::move(path)));
  }
  std::vector<std::thread> threads;
  threads.reserve(nThreads);
  for (auto i = 0uz; i < nThreads; ++i) {
    threads.emplace_back(JobRunner(state.queue));
  }
  for (auto& thread: threads) {
    thread.join();
  }
  return state.matched_one.test() ? 0 : 1;
}
