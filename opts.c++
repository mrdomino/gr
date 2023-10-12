#include "opts.h"

#include <unistd.h>

#include <algorithm>
#include <iostream>

#include "io.h"

static_assert(
    std::is_sorted(
        std::begin(ArgParser::long_opts), std::end(ArgParser::long_opts),
        [](auto a, auto b) {
          return std::get<0>(a) < std::get<0>(b);
        }), "long_opts must be sorted");

static_assert(
    std::all_of(
        std::begin(ArgParser::short_opts), std::end(ArgParser::short_opts),
        [](auto v) {
          return std::visit([](auto&& f) { return f != nullptr; }, v);
        }), "missing short_opts func");

[[noreturn]] void usage(std::string_view argv0) {
  mPrintLn(std::cerr, "usage: {} [options] <pattern> [path ...]", argv0);
  mPrintLn(
      std::cerr,

      "\nRecursively search for pattern in path.\n"
      "Uses the re2 regular expression library.\n\n"

      "Options:\n"
      "  -A --after-context <num> Show num lines of context after each match\n"
      "  -B --before-context <num>\n"
      "                           Show num lines of context before each match\n"
      "  -C --context <num>       Show num lines before and after each match\n"
      "  -c --count               Show count of matches only\n"
      "  -l --files-with-matches  Only print filenames that contain matches\n"
      "                           (don't print the matching lines)\n"
      "     --long-lines          Print long lines (default truncates to ~2k)\n"
      "  -Q --literal             Match pattern as literal, not regexp\n"
      "  -h --help                Print this usage message and exit.\n"
      "     --version             Print the program version.");
  exit(2);
}

namespace {

constexpr std::pair<std::string_view, ArgParser::func>
lookup_long_opt(std::string_view opt) {
  auto it = std::lower_bound(
      std::begin(ArgParser::long_opts), std::end(ArgParser::long_opts), opt,
      [](auto a, auto b) {
        return std::get<0>(a) < b;
      });
  if (it != std::end(ArgParser::long_opts)
      && std::get<0>(*it).starts_with(opt)) {
    if (it + 1 != std::end(ArgParser::long_opts)
        && std::get<0>(*(it + 1)).starts_with(opt)) {
      throw ArgumentError{"ambiguous option --{}", opt};
    }
    return *it;
  }
  throw ArgumentError{"unrecognized option --{}", opt};
}

void swap_portions(char const* argv[], int* first_nonopt, int* last_nonopt,
                   int optind) {
  int bottom = *first_nonopt;
  int middle = *last_nonopt;
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
  *first_nonopt += (optind - *last_nonopt);
  *last_nonopt = optind;
}

}   // namespace

void ArgParser::parse_args(const int argc, char const* argv[], Opts& opts) {
  opts.argv0 = *argv;
  opts.stdout_is_tty = isatty(fileno(stdout));
  int optind = 1;
  int first_nonopt = 1;
  int last_nonopt = 1;
  while (true) {
    if (first_nonopt != last_nonopt && last_nonopt != optind) {
      swap_portions(argv, &first_nonopt, &last_nonopt, optind);
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
        swap_portions(argv, &first_nonopt, &last_nonopt, optind);
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
      ++optind;
      opt.remove_prefix(1);
      auto eq = opt.find('=');
      auto arg = [=] {
        if (eq != opt.npos) {
          return std::string_view(opt.begin() + eq + 1, opt.size() - eq - 1);
        }
        return std::string_view();
      }();
      if (eq != opt.npos) {
        opt.remove_suffix(opt.size() - eq);
      }
      auto [optopt, func] = lookup_long_opt(opt);
      std::visit([&](auto&& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, opt_func>) {
          if (eq != opt.npos) {
            throw ArgumentError{"--{} takes no argument", optopt};
          }
          f(opts);
        }
        else {
          static_assert(std::is_same_v<T, arg_func>);
          if (eq == opt.npos) {
            if (optind < argc) {
              arg = argv[optind++];
            }
            else {
              throw ArgumentError{"--{} requires argument", optopt};
            }
          }
          f(opts, arg);
        }
      }, func);
    }
    else {
      while (opt.size()) {
        const auto c = opt.front();
        const auto i = short_opt_chars.find(c);
        if (i == short_opt_chars.npos) {
          throw ArgumentError{"invalid option -{}", c};
        }
        opt.remove_prefix(1);
        if (!opt.size()) {
          ++optind;
        }
        auto func = short_opts[i];
        std::visit([&](auto&& f) {
          using T = std::decay_t<decltype(f)>;
          if constexpr (std::is_same_v<T, opt_func>) {
            f(opts);
          }
          else {
            static_assert(std::is_same_v<T, arg_func>);
            std::string_view arg;
            if (opt.size()) {
              std::swap(arg, opt);
              ++optind;
            }
            else if (optind == argc) {
              throw ArgumentError{"-{} requires argument", c};
            }
            else {
              arg = argv[optind++];
            }
            f(opts, arg);
          }
        }, func);
      }
    }
  }
  if (opts.hflag || opts.version) {
    return;
  }
  if (optind == argc) {
    throw ArgumentError{"missing pattern"};
  }
  opts.pattern = argv[optind++];
  if (optind < argc) {
    opts.paths = std::vector<std::string_view>(argv + optind, argv + argc);
  }

  if (opts.count || opts.lflag) {
    opts.before_context = opts.after_context = 0;
  }
}
