#include <cmath>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <re2/re2.h>

namespace fs = std::filesystem;

#define FWD(x) std::forward<decltype(x)>(x)

const std::unordered_set<fs::path> g_ignored {
  ".git",
};

struct CrParams {
  std::string pattern;
  std::optional<std::vector<fs::path>> paths;
};

struct FullPaths {
  std::vector<fs::path> value;

  FullPaths(std::optional<std::vector<fs::path>>&& paths) {
    for (auto&& path: std::move(paths).value_or(std::vector<fs::path>{"."})) {
      addPath(std::move(path));
    }
  }

  void addPath(auto&& path,
               std::optional<fs::file_status> ss_ = std::nullopt) {
    if (g_ignored.contains(fs::path(path).filename())) {
      return;
    }
    auto s = ss_.value_or(fs::status(path));
    if (fs::is_regular_file(s)) {
      value.emplace_back(FWD(path));
    }
    else if (fs::is_directory(s)) {
      for (auto it{fs::directory_iterator(FWD(path))};
           it != fs::directory_iterator(); ++it) {
        addPath(*it, it->status());
      }
    }
    else {
      std::cerr << "Skipping " << FWD(path) << '\n';
    }
  }
};

struct Error {
  std::string reason;
};

struct Args {
  std::string argv0;
  std::expected<CrParams, Error> params;
};

Args parse_args(int const argc, char const* const argv[]) {
  Args ret;
  ret.argv0 = argv[0];
  if (argc < 2) {
    ret.params = std::unexpected{Error{"missing pattern"}};
    return ret;
  }
  CrParams params;
  params.pattern = argv[1];
  if (argc > 2) {
    params.paths.emplace(argv + 2, argv + argc);
  }
  ret.params = params;
  return ret;
}

[[noreturn]] void usage(Args&& args) {
  std::cerr << std::format("{0}: {1}\nusage: {0} <pattern> [filename...]\n",
                           args.argv0, args.params.error().reason);
  exit(2);
}

bool is_binary(std::fstream& fs) {
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
  return false;
}

struct MatchResult {
  fs::path path;
  std::vector<std::pair<size_t, std::string>> lines;
};

class Cr {
 public:
  Cr(std::unique_ptr<re2::RE2> expr, FullPaths&& fp):
      expr(std::move(expr)), paths(std::move(fp.value)) {}

  int operator()() {
    for (auto& p: paths) {
      do_path(p);
    }
    int ret = results.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    while (results.size()) {
      auto result = std::move(results.front());
      results.pop();
      std::cout << std::format("{}:\n", pretty_path(result.path));
      for (auto& [line, text]: result.lines) {
        std::cout << std::format("{:3}: {}\n", line, text);
      }
      if (results.size()) {
        std::cout << std::endl;
      }
    }
    return ret;
  }

 private:
  std::string pretty_path(fs::path const& p) const {
    if (*std::begin(p) == ".") {
      // XX surprisingly painful.... we don't want relative() since it
      // canonicalizes symlinks
      fs::path q;
      for (auto it = ++std::begin(p); it != std::end(p); ++it) {
        q += *it;
      }
      return q.string();
    }
    return p.string();
  }

  void do_path(fs::path const& p) {
    std::optional<MatchResult> res;
    std::string text;
    size_t line = 0;
    auto fs = std::fstream(p, std::ios_base::in);
    if (is_binary(fs)) {
      return;
    }
    while (std::getline(fs, text)) {
      ++line;
      if (re2::RE2::PartialMatch(text, *expr)) {
        if (!res) {
          res.emplace(p);
        }
        res->lines.emplace_back(line, std::move(text));
      }
    }
    if (res) {
      results.emplace(std::move(*res));
    }
    if (fs.bad()) {
      throw std::runtime_error{std::format("IO error on {}", p.string())};
    }
  }

  const std::unique_ptr<re2::RE2> expr;
  const std::vector<fs::path> paths;
  std::queue<MatchResult> results;
};

int main(int const argc, char const* const argv[]) {
  auto args = parse_args(argc, argv);
  if (!args.params.has_value()) {
    usage(std::move(args));
  }
  auto& params = args.params.value();
  auto expr = std::make_unique<re2::RE2>(params.pattern);
  if (!expr->ok()) {
    std::cerr << std::format("{0}: invalid pattern {1}\n", args.argv0,
                             params.pattern);
    exit(2);
  }
  auto fp = FullPaths(std::move(params.paths));
  return Cr(std::move(expr), std::move(fp))();
}
