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
    auto s = ss_.value_or(fs::symlink_status(path));
    if (fs::is_regular_file(s)) {
      value.emplace_back(FWD(path));
    }
    else if (fs::is_directory(s)) {
      for (auto it{fs::directory_iterator(FWD(path))};
           it != fs::directory_iterator(); ++it) {
        addPath(*it, it->symlink_status());
      }
    }
    else if (fs::is_symlink(s)) {
      addPath(fs::read_symlink(FWD(path)));
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

class Cr {
 public:
  Cr(std::unique_ptr<re2::RE2> expr, FullPaths&& fp):
      expr(std::move(expr)), paths(std::move(fp.value)) {}

  int operator()() const {
    bool foundAny = false;
    for (auto& p: paths) {
      foundAny |= do_path(p);
    }
    return foundAny ? EXIT_SUCCESS : EXIT_FAILURE;
  }

 private:
  std::string pretty_path(fs::path const& p) const {
    if (p.is_relative()) {
      return canonical(p).lexically_relative(fs::current_path()).string();
    }
    return canonical(p);
  }

  bool do_path(fs::path const& p) const {
    bool matched = false;
    std::string text;
    size_t line = 0;
    auto fs = std::fstream(p, std::ios_base::in);
    if (is_binary(fs)) {
      return false;
    }
    while (std::getline(fs, text)) {
      ++line;
      if (re2::RE2::PartialMatch(text, *expr)) {
        if (!matched) {
          matched = true;
          std::cout << std::format("{}:\n", pretty_path(p));
        }
        std::cout << std::format("{0:3}: {1}\n", line, text);
      }
    }
    if (fs.bad()) {
      throw std::runtime_error{std::format("IO error on {}", p.string())};
    }
    return matched;
  }

  std::unique_ptr<re2::RE2> expr;
  std::vector<fs::path> paths;
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
