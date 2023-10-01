#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <re2/re2.h>

struct CrParams {
  std::string pattern;
  std::optional<std::vector<std::filesystem::path>> paths;
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
                           args.argv0, args.params.error());
  exit(2);
}

int main(int const argc, char const* const argv[]) {
  auto args = parse_args(argc, argv);
  if (!args.params.has_value()) {
    usage(std::move(args));
  }
  auto& params = args.params.value();
  auto expr = re2::RE2(params.pattern);
  if (!expr.ok()) {
    std::cerr << std::format("{0}: invalid pattern {1}\n", args.argv0,
                             params.pattern);
    exit(2);
  }
  return 0;
}
