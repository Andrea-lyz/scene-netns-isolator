#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kCtl = "/data/adb/modules/scene-netns-isolator/bin/scene-netnsctl";
constexpr const char *kShell = "/system/bin/sh";

std::string shell_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

const char *find_real_su() {
  static const char *candidates[] = {
      "/system/bin/su",
      "/system/xbin/su",
      "/sbin/su",
      "/debug_ramdisk/su",
      nullptr,
  };

  for (const char **p = candidates; *p; ++p) {
    if (access(*p, X_OK) == 0) {
      return *p;
    }
  }
  return nullptr;
}

[[noreturn]] void exec_argv(const char *path, std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  execv(path, argv.data());
  std::fprintf(stderr, "scene su wrapper: [su-wrapper] exec %s failed: %s\n",
               path, std::strerror(errno));
  std::exit(127);
}

int find_command_index(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      return i + 1;
    }
  }
  return -1;
}

}  // namespace

int main(int argc, char **argv) {
  const char *real_su = find_real_su();
  if (!real_su) {
    std::fprintf(stderr, "scene su wrapper: [su-wrapper] real su not found\n");
    return 127;
  }

  std::vector<std::string> out;
  out.emplace_back("su");

  const char *isolated = std::getenv("SCENE_NETNS_ISOLATED");
  if (!isolated || std::strcmp(isolated, "1") != 0) {
    for (int i = 1; i < argc; ++i) {
      out.emplace_back(argv[i]);
    }
    exec_argv(real_su, out);
  }

  int command_index = find_command_index(argc, argv);
  if (command_index >= 0) {
    for (int i = 1; i < argc; ++i) {
      if (i == command_index) {
        std::string wrapped = std::string(kCtl) + " enter -- " + kShell + " -c " +
                              shell_quote(argv[i]);
        out.emplace_back(wrapped);
      } else {
        out.emplace_back(argv[i]);
      }
    }
    exec_argv(real_su, out);
  }

  for (int i = 1; i < argc; ++i) {
    out.emplace_back(argv[i]);
  }

  out.emplace_back("-c");
  out.emplace_back(std::string(kCtl) + " enter -- " + kShell);
  exec_argv(real_su, out);
}
