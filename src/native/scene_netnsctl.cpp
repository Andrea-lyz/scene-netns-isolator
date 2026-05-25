// SPDX-License-Identifier: GPL-3.0
#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../common/runtime_paths.h"

namespace {

using scene_netns::kEndpointPath;
using scene_netns::kPidPath;
using scene_netns::kRunDir;
using scene_netns::kUnixPathMax;

constexpr const char *kDefaultShell = "/system/bin/sh";

// veth pair / addressing.  Names are kept short (IFNAMSIZ = 16).
//
// CIDR maths:  10.99.99.0/30  -> .1 (host)  .2 (isolated)  .3 (broadcast)
constexpr const char *kVethHost      = "scn-h";
constexpr const char *kVethIso       = "scn-i";
constexpr const char *kVethNetCidr   = "10.99.99.0/30";
constexpr const char *kVethHostCidr  = "10.99.99.1/30";
constexpr const char *kVethIsoCidr   = "10.99.99.2/30";
constexpr const char *kVethGateway   = "10.99.99.1";

char g_sock_path[kUnixPathMax] = {};
int g_host_ns_fd = -1;
int g_isolated_ns_fd = -1;

// ---------------------------------------------------------------------------
// Logging helpers (line-buffered, immediately flushed).
// ---------------------------------------------------------------------------

void log_line(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void die(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s: %s\n", message, std::strerror(errno));
  std::fflush(stderr);
  std::exit(1);
}

void die_msg(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s\n", message);
  std::fflush(stderr);
  std::exit(1);
}

// ---------------------------------------------------------------------------
// /run dir + endpoint file management.
// ---------------------------------------------------------------------------

void ensure_run_dir() {
  if (mkdir(kRunDir, 0755) != 0 && errno != EEXIST) {
    die("mkdir run dir");
  }
  chmod(kRunDir, 0755);
}

bool path_is_in_run_dir(const char *path) {
  const size_t dir_len = std::strlen(kRunDir);
  if (std::strncmp(path, kRunDir, dir_len) != 0 || path[dir_len] != '/') {
    return false;
  }
  const char *leaf = path + dir_len + 1;
  return leaf[0] != '\0' && std::strchr(leaf, '/') == nullptr &&
         std::strlen(path) < kUnixPathMax;
}

bool read_endpoint_path(char *out, size_t out_size, bool quiet) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';

  int fd = open(kEndpointPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (!quiet) die("[client] open pinner endpoint");
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    if (!quiet) die("[client] stat pinner endpoint");
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    close(fd);
    if (!quiet) die_msg("[client] pinner endpoint is not private");
    return false;
  }
  ssize_t n = read(fd, out, out_size - 1);
  if (n < 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    if (!quiet) die("[client] read pinner endpoint");
    return false;
  }
  close(fd);
  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                   out[n - 1] == ' ' || out[n - 1] == '\t')) {
    out[--n] = '\0';
  }
  if (n == 0 || !path_is_in_run_dir(out)) {
    if (!quiet) die_msg("[client] pinner endpoint path is invalid");
    return false;
  }
  return true;
}

void require_private_socket(const char *path) {
  struct stat st {};
  if (lstat(path, &st) != 0) die("[client] stat pinner socket");
  if (!S_ISSOCK(st.st_mode)) die_msg("[client] pinner endpoint exists but is not a socket");
  if (st.st_uid != 0 || (st.st_mode & 0077) != 0) die_msg("[client] pinner socket is not private");
}

// ---------------------------------------------------------------------------
// netns helpers.
// ---------------------------------------------------------------------------

int sys_setns(int fd, int nstype) {
  return static_cast<int>(syscall(__NR_setns, fd, nstype));
}

int sys_unshare(int flags) {
  return static_cast<int>(syscall(__NR_unshare, flags));
}

int open_netns(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) die("[pinner] open netns");
  return fd;
}

int open_current_netns() { return open_netns("/proc/self/ns/net"); }

void bring_loopback_up() {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) die("socket");
  ifreq ifr {};
  std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");
  if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
    close(fd);
    die("SIOCGIFFLAGS lo");
  }
  ifr.ifr_flags = static_cast<short>(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
  if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
    close(fd);
    die("SIOCSIFFLAGS lo");
  }
  close(fd);
}

void write_pid_file() {
  FILE *fp = std::fopen(kPidPath, "w");
  if (!fp) die("[pinner] open pid file");
  std::fprintf(fp, "%d\n", getpid());
  std::fclose(fp);
  chmod(kPidPath, 0600);
}

// ---------------------------------------------------------------------------
// Unix-socket "give me the netns fd" service (unchanged from the previous
// design; Zygisk uses this to fetch the pinned netns fd over SCM_RIGHTS).
// ---------------------------------------------------------------------------

void fill_sockaddr(sockaddr_un *addr, const char *path) {
  std::memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  std::snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
}

int connect_pinner_socket() {
  char sock_path[kUnixPathMax] {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path), false)) {
    die_msg("[client] pinner endpoint is unavailable");
  }
  require_private_socket(sock_path);
  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) die("[client] socket");
  sockaddr_un addr {};
  fill_sockaddr(&addr, sock_path);
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(sock);
    die("[client] connect pinner socket");
  }
  return sock;
}

int recv_fd(int sock) {
  char data = 0;
  iovec iov {};
  iov.iov_base = &data;
  iov.iov_len = sizeof(data);
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
  msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) < 0) die("[client] recv netns fd");
  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    die_msg("[client] pinner did not send a namespace fd");
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
  if (fd < 0) die_msg("[client] received invalid namespace fd");
  return fd;
}

int request_netns_fd() {
  int sock = connect_pinner_socket();
  int fd = recv_fd(sock);
  close(sock);
  return fd;
}

void send_fd(int sock, int fd) {
  char data = 'N';
  iovec iov {};
  iov.iov_base = &data;
  iov.iov_len = sizeof(data);
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
  msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  msg.msg_controllen = CMSG_SPACE(sizeof(int));
  if (sendmsg(sock, &msg, MSG_NOSIGNAL) < 0) {
    log_line("scene-netnsctl: [pinner] send netns fd failed: %s", std::strerror(errno));
  }
}

bool peer_is_allowed(int sock) {
  struct PeerCred { pid_t pid; uid_t uid; gid_t gid; } cred {};
  socklen_t len = sizeof(cred);
  if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    log_line("scene-netnsctl: [pinner] peer credential check failed: %s", std::strerror(errno));
    return false;
  }
  if (cred.uid != 0) {
    log_line("scene-netnsctl: [pinner] rejected non-root peer pid=%d uid=%u",
             static_cast<int>(cred.pid), static_cast<unsigned>(cred.uid));
    return false;
  }
  return true;
}

void enter_netns() {
  int fd = request_netns_fd();
  if (sys_setns(fd, CLONE_NEWNET) != 0) {
    close(fd);
    die("[client] setns");
  }
  close(fd);
  bring_loopback_up();
}

void exec_command(int argc, char **argv, int first) {
  if (first >= argc) {
    char *const shell_argv[] = {const_cast<char *>(kDefaultShell), nullptr};
    execv(kDefaultShell, shell_argv);
    die("exec shell");
  }
  execvp(argv[first], &argv[first]);
  die("exec command");
}

// ---------------------------------------------------------------------------
// Random / temp helpers (used for the unix socket name).
// ---------------------------------------------------------------------------

uint64_t random_u64() {
  uint64_t value = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, &value, sizeof(value));
    close(fd);
    if (n == static_cast<ssize_t>(sizeof(value))) return value;
  }
  return (static_cast<uint64_t>(std::time(nullptr)) << 32) ^
         (static_cast<uint64_t>(getpid()) << 16) ^
         static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&value));
}

std::string make_socket_path() {
  for (int attempt = 0; attempt < 32; ++attempt) {
    char path[kUnixPathMax] {};
    std::snprintf(path, sizeof(path), "%s/%016llx", kRunDir,
                  static_cast<unsigned long long>(random_u64() ^
                                                  static_cast<uint64_t>(attempt)));
    struct stat st {};
    if (lstat(path, &st) != 0 && errno == ENOENT) return path;
  }
  die_msg("[pinner] unable to allocate private socket path");
  return {};
}

void write_endpoint_file(const char *sock_path) {
  int fd = open(kEndpointPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) die("[pinner] open endpoint file");
  const size_t len = std::strlen(sock_path);
  if (write(fd, sock_path, len) != static_cast<ssize_t>(len) ||
      write(fd, "\n", 1) != 1) {
    int saved = errno;
    close(fd);
    errno = saved;
    die("[pinner] write endpoint file");
  }
  close(fd);
  chmod(kEndpointPath, 0600);
}

void cleanup_stale_endpoint() {
  char stale_path[kUnixPathMax] {};
  if (read_endpoint_path(stale_path, sizeof(stale_path), true)) unlink(stale_path);
  unlink(kEndpointPath);
  unlink(kPidPath);
}

// ---------------------------------------------------------------------------
// veth + NAT setup.
//
// Strategy:
//   * Already in the isolated netns (caller unshare()d before us).
//   * Fork a helper child, switch the child into the host netns, and run all
//     the host-side commands (`ip`, `iptables`) there.  The helper passes the
//     veth's isolated end into our (parent) netns by `ip link set ... netns
//     <pid>` where pid is the parent pinner.  Because the parent stays in
//     the isolated netns the helper's pid arg is the easiest way to address
//     the target ns without having to share an fd with the helper.
//   * After the helper exits we configure the isolated end of the veth from
//     the parent (which is in the isolated netns).
//
// We deliberately call out to the system `ip` and `iptables` binaries; that
// keeps the implementation small and matches what every Android router /
// VPN module on the planet does.  All netlink configuration is hidden behind
// these binaries.
// ---------------------------------------------------------------------------

bool find_bin(const char *name, char *out_path, size_t out_size) {
  static const char *prefixes[] = {
      "/system/bin/", "/system/xbin/", "/vendor/bin/", nullptr,
  };
  for (const char **p = prefixes; *p; ++p) {
    char candidate[256];
    std::snprintf(candidate, sizeof(candidate), "%s%s", *p, name);
    if (access(candidate, X_OK) == 0) {
      std::snprintf(out_path, out_size, "%s", candidate);
      return true;
    }
  }
  return false;
}

// Prefer the legacy iptables binary because Android's nft backend on many
// OEM kernels is missing the `nat` table.  iptables-legacy talks ip_tables.so
// directly which is more widely available.
bool find_iptables(char *out_path, size_t out_size) {
  if (find_bin("iptables-legacy", out_path, out_size)) return true;
  return find_bin("iptables", out_path, out_size);
}

// Run a binary with argv, capture stdout+stderr, return exit code (-1 on
// fork/wait error).  Output is logged when status != 0.
int run_capture(const char *bin, std::vector<const char *> argv,
                const char *tag) {
  argv.push_back(nullptr);

  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) pipefd[0] = pipefd[1] = -1;

  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
    log_line("scene-netnsctl: [%s] fork %s failed: %s", tag, bin, std::strerror(errno));
    return -1;
  }
  if (pid == 0) {
    if (pipefd[1] >= 0) {
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
    }
    execv(bin, const_cast<char *const *>(argv.data()));
    _exit(127);
  }

  if (pipefd[1] >= 0) close(pipefd[1]);
  std::string captured;
  if (pipefd[0] >= 0) {
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
      captured.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    log_line("scene-netnsctl: [%s] waitpid %s: %s", tag, bin, std::strerror(errno));
    return -1;
  }
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exit_code != 0) {
    log_line("scene-netnsctl: [%s] %s exit=%d output=<<%s>>",
             tag, bin, exit_code,
             captured.empty() ? "(empty)" : captured.c_str());
  }
  return exit_code;
}

// Build argv for run_capture from a string list.
struct Cmd {
  std::vector<std::string> args;
  Cmd(std::initializer_list<const char *> list) {
    for (const char *s : list) args.emplace_back(s);
  }
  std::vector<const char *> argv() const {
    std::vector<const char *> v;
    v.reserve(args.size() + 1);
    for (const auto &s : args) v.push_back(s.c_str());
    return v;
  }
};

bool write_proc_file(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    log_line("scene-netnsctl: [veth] open %s failed: %s", path, std::strerror(errno));
    return false;
  }
  size_t len = std::strlen(value);
  bool ok = write(fd, value, len) == static_cast<ssize_t>(len);
  close(fd);
  if (!ok) {
    log_line("scene-netnsctl: [veth] write %s failed: %s", path, std::strerror(errno));
  }
  return ok;
}

// Helper child entry: switch to host netns, set up veth + NAT, then exit.
[[noreturn]] void host_setup_child(pid_t parent_pid_in_isolated) {
  // Switch this child process into the host netns.
  if (sys_setns(g_host_ns_fd, CLONE_NEWNET) != 0) {
    log_line("scene-netnsctl: [veth-host] setns(host) failed: %s", std::strerror(errno));
    _exit(2);
  }

  char ip_bin[256] = {};
  char iptables_bin[256] = {};
  if (!find_bin("ip", ip_bin, sizeof(ip_bin))) {
    log_line("scene-netnsctl: [veth-host] no `ip` binary available");
    _exit(3);
  }
  if (!find_iptables(iptables_bin, sizeof(iptables_bin))) {
    log_line("scene-netnsctl: [veth-host] no `iptables` binary available");
    _exit(4);
  }

  // Best-effort cleanup of any stale state from previous runs.  The kernel
  // auto-removes the veth pair when its peer's netns disappears, but if the
  // pinner restarted and the host side leaked we want to clear it manually.
  // All these can fail silently.
  Cmd c_link_del = {ip_bin, "link", "del", kVethHost};
  run_capture(ip_bin, c_link_del.argv(), "veth-host-cleanup");

  Cmd c_nat_del = {iptables_bin, "-t", "nat", "-D", "POSTROUTING",
                   "-s", kVethNetCidr, "-j", "MASQUERADE"};
  run_capture(iptables_bin, c_nat_del.argv(), "veth-host-cleanup");
  Cmd c_fwd_in_del = {iptables_bin, "-D", "FORWARD",
                      "-i", kVethHost, "-j", "ACCEPT"};
  run_capture(iptables_bin, c_fwd_in_del.argv(), "veth-host-cleanup");
  Cmd c_fwd_out_del = {iptables_bin, "-D", "FORWARD",
                       "-o", kVethHost, "-j", "ACCEPT"};
  run_capture(iptables_bin, c_fwd_out_del.argv(), "veth-host-cleanup");

  // Create veth pair.  Both ends start in host netns.
  Cmd c_link_add = {ip_bin, "link", "add", kVethHost,
                    "type", "veth", "peer", "name", kVethIso};
  if (run_capture(ip_bin, c_link_add.argv(), "veth-host") != 0) _exit(5);

  // Move the iso end to the parent (which lives in the isolated netns).
  char pid_str[32];
  std::snprintf(pid_str, sizeof(pid_str), "%d",
                static_cast<int>(parent_pid_in_isolated));
  Cmd c_link_to_ns = {ip_bin, "link", "set", kVethIso, "netns", pid_str};
  if (run_capture(ip_bin, c_link_to_ns.argv(), "veth-host") != 0) _exit(6);

  // Configure host-side veth.
  Cmd c_addr = {ip_bin, "addr", "add", kVethHostCidr, "dev", kVethHost};
  if (run_capture(ip_bin, c_addr.argv(), "veth-host") != 0) _exit(7);
  Cmd c_up = {ip_bin, "link", "set", kVethHost, "up"};
  if (run_capture(ip_bin, c_up.argv(), "veth-host") != 0) _exit(8);

  // Enable forwarding in host netns (per-netns sysctl).
  if (!write_proc_file("/proc/sys/net/ipv4/ip_forward", "1\n")) _exit(9);

  // NAT + forward rules.  We use -I (insert at top) for FORWARD so that any
  // OEM-supplied DROP/REJECT rules later in the chain don't shadow ours.
  Cmd c_nat = {iptables_bin, "-t", "nat", "-A", "POSTROUTING",
               "-s", kVethNetCidr, "-j", "MASQUERADE"};
  if (run_capture(iptables_bin, c_nat.argv(), "veth-host") != 0) _exit(10);
  Cmd c_fwd_in = {iptables_bin, "-I", "FORWARD", "1",
                  "-i", kVethHost, "-j", "ACCEPT"};
  if (run_capture(iptables_bin, c_fwd_in.argv(), "veth-host") != 0) _exit(11);
  Cmd c_fwd_out = {iptables_bin, "-I", "FORWARD", "1",
                   "-o", kVethHost, "-j", "ACCEPT"};
  if (run_capture(iptables_bin, c_fwd_out.argv(), "veth-host") != 0) _exit(12);

  // Android does not put a default route in the main routing table; instead
  // each network has its own per-network table selected by uid/mark/oif
  // policy rules set up by netd.  When forwarded packets enter the host from
  // scn-h they have no fwmark and no matching uid, so the kernel cannot find
  // any default route and silently drops them BEFORE FORWARD ever runs.
  //
  // Build a private routing table that mirrors whichever default network the
  // device currently uses, then add an `iif scn-h` rule that selects it.
  // A reasonable preference (11000) puts us before the main lookup but after
  // netd's per-network rules so we don't disturb anything else.
  //
  // We discover the default upstream by inspecting `ip route show table all
  // default` and taking the first entry's gateway and oif.  This is the same
  // approach Android's own ConnectivityService uses internally and survives
  // wifi/mobile data switching well enough for the pinner's lifetime; if the
  // user roams onto a different network the proxy keeps using the original
  // upstream until pinner is restarted.
  auto run_capture_text = [](const char *bin, std::vector<const char *> argv,
                              std::string *out) -> int {
    argv.push_back(nullptr);
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
      close(pipefd[0]); close(pipefd[1]);
      return -1;
    }
    if (pid == 0) {
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[0]); close(pipefd[1]);
      execv(bin, const_cast<char *const *>(argv.data()));
      _exit(127);
    }
    close(pipefd[1]);
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out->append(buf, n);
    close(pipefd[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  };

  std::string upstream_routes;
  std::vector<const char *> ip_show_argv = {ip_bin, "route", "show",
                                            "table", "all", "default"};
  if (run_capture_text(ip_bin, ip_show_argv, &upstream_routes) != 0 ||
      upstream_routes.empty()) {
    log_line("scene-netnsctl: [veth-host] no default route discovered; "
             "outbound forward will fail");
    _exit(13);
  }

  // Parse the first non-empty line: "default via <gw> dev <oif> ..."
  std::string upstream_gw, upstream_oif;
  {
    size_t pos = 0;
    while (pos < upstream_routes.size()) {
      size_t eol = upstream_routes.find('\n', pos);
      if (eol == std::string::npos) eol = upstream_routes.size();
      std::string line = upstream_routes.substr(pos, eol - pos);
      pos = eol + 1;
      // Tokenize.
      std::vector<std::string> toks;
      size_t i = 0;
      while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (start < i) toks.push_back(line.substr(start, i - start));
      }
      if (toks.size() >= 5 && toks[0] == "default" && toks[1] == "via" &&
          toks[3] == "dev") {
        upstream_gw = toks[2];
        upstream_oif = toks[4];
        break;
      }
    }
  }
  if (upstream_gw.empty() || upstream_oif.empty()) {
    log_line("scene-netnsctl: [veth-host] could not parse default route from: %s",
             upstream_routes.c_str());
    _exit(14);
  }
  log_line("scene-netnsctl: [veth-host] upstream gw=%s oif=%s",
           upstream_gw.c_str(), upstream_oif.c_str());

  // Pre-clean any stale entries we may have left from a previous run.
  Cmd c_rule_del = {ip_bin, "rule", "del", "iif", kVethHost,
                    "table", "99"};
  for (int i = 0; i < 4; ++i) {
    if (run_capture(ip_bin, c_rule_del.argv(), "veth-host-cleanup") != 0) break;
  }
  Cmd c_table_flush = {ip_bin, "route", "flush", "table", "99"};
  run_capture(ip_bin, c_table_flush.argv(), "veth-host-cleanup");

  // Build table 99 with default + the scn-h connected route.
  Cmd c_route_dflt = {ip_bin, "route", "add", "default",
                      "via", upstream_gw.c_str(),
                      "dev", upstream_oif.c_str(),
                      "table", "99"};
  if (run_capture(ip_bin, c_route_dflt.argv(), "veth-host") != 0) _exit(15);

  Cmd c_route_link = {ip_bin, "route", "add", kVethNetCidr,
                      "dev", kVethHost, "table", "99"};
  if (run_capture(ip_bin, c_route_link.argv(), "veth-host") != 0) _exit(16);

  // Steer traffic that arrives via scn-h into table 99.
  Cmd c_rule_add = {ip_bin, "rule", "add", "iif", kVethHost,
                    "pref", "11000", "table", "99"};
  if (run_capture(ip_bin, c_rule_add.argv(), "veth-host") != 0) _exit(17);

  log_line("scene-netnsctl: [veth-host] host side ready (%s on %s, MASQ for %s)",
           kVethHostCidr, kVethHost, kVethNetCidr);
  _exit(0);
}

bool setup_veth_and_routes() {
  // Caller (us) is currently in the isolated netns.  Fork a helper that
  // setns()es into host and configures the host side + creates the veth.
  pid_t my_pid = getpid();
  pid_t helper = fork();
  if (helper < 0) {
    log_line("scene-netnsctl: [veth] fork helper failed: %s", std::strerror(errno));
    return false;
  }
  if (helper == 0) {
    host_setup_child(my_pid);  // [[noreturn]]
  }

  int status = 0;
  if (waitpid(helper, &status, 0) < 0) {
    log_line("scene-netnsctl: [veth] waitpid helper failed: %s", std::strerror(errno));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    log_line("scene-netnsctl: [veth] host helper exited with %d", code);
    return false;
  }

  // The iso peer is now sitting in our (isolated) netns, down, no addrs.
  char ip_bin[256] = {};
  if (!find_bin("ip", ip_bin, sizeof(ip_bin))) {
    log_line("scene-netnsctl: [veth-iso] no `ip` binary available");
    return false;
  }

  Cmd c_addr = {ip_bin, "addr", "add", kVethIsoCidr, "dev", kVethIso};
  if (run_capture(ip_bin, c_addr.argv(), "veth-iso") != 0) return false;
  Cmd c_up = {ip_bin, "link", "set", kVethIso, "up"};
  if (run_capture(ip_bin, c_up.argv(), "veth-iso") != 0) return false;

  // Add a default route via the host side.
  Cmd c_def = {ip_bin, "route", "add", "default", "via", kVethGateway,
               "dev", kVethIso};
  if (run_capture(ip_bin, c_def.argv(), "veth-iso") != 0) return false;

  log_line("scene-netnsctl: [veth-iso] iso side ready (%s on %s, default via %s)",
           kVethIsoCidr, kVethIso, kVethGateway);
  return true;
}

// ---------------------------------------------------------------------------
// Pin loop.
// ---------------------------------------------------------------------------

void pin_forever() {
  ensure_run_dir();
  cleanup_stale_endpoint();

  g_host_ns_fd = open_current_netns();

  if (sys_unshare(CLONE_NEWNET) != 0) die("[pinner] unshare(CLONE_NEWNET)");
  bring_loopback_up();
  g_isolated_ns_fd = open_current_netns();

  // Plumb veth + NAT.  Failure is non-fatal: the unix endpoint still works
  // and Zygisk can still place Scene into the netns; outbound traffic will
  // simply fail closed, which is the same situation as previous proxy
  // attempts but at least the operator can inspect the situation.
  bool net_ok = setup_veth_and_routes();
  if (!net_ok) {
    log_line("scene-netnsctl: [pinner] veth setup failed; outbound will not work");
  }

  int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0) die("[pinner] socket");
  std::string sock_path = make_socket_path();
  std::snprintf(g_sock_path, sizeof(g_sock_path), "%s", sock_path.c_str());
  unlink(g_sock_path);
  sockaddr_un addr {};
  fill_sockaddr(&addr, g_sock_path);
  if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    die("[pinner] bind socket");
  }
  chmod(g_sock_path, 0600);
  if (listen(server, 16) != 0) die("[pinner] listen socket");
  write_endpoint_file(g_sock_path);
  write_pid_file();

  std::signal(SIGTERM, [](int) {
    if (g_sock_path[0] != '\0') unlink(g_sock_path);
    unlink(kEndpointPath);
    unlink(kPidPath);
    _exit(0);
  });

  log_line("scene-netnsctl: [pinner] ready (net_ok=%d)", net_ok ? 1 : 0);

  for (;;) {
    int client = accept(server, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      log_line("scene-netnsctl: [pinner] accept failed: %s", std::strerror(errno));
      continue;
    }
    int flags = fcntl(client, F_GETFD);
    if (flags >= 0) fcntl(client, F_SETFD, flags | FD_CLOEXEC);
    if (peer_is_allowed(client)) send_fd(client, g_isolated_ns_fd);
    close(client);
  }
}

// ---------------------------------------------------------------------------
// status subcommand.
// ---------------------------------------------------------------------------

void status() {
  char sock_path[kUnixPathMax] {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path), false)) {
    die_msg("[client] pinner endpoint is unavailable");
  }
  struct stat sock_st {};
  if (lstat(sock_path, &sock_st) != 0) die("[client] stat pinner socket");
  if (!S_ISSOCK(sock_st.st_mode)) die_msg("[client] pinner endpoint exists but is not a socket");
  if (sock_st.st_uid != 0 || (sock_st.st_mode & 0077) != 0) {
    die_msg("[client] pinner socket is not private");
  }

  int fd = request_netns_fd();
  struct stat ns_st {};
  if (fstat(fd, &ns_st) != 0) {
    close(fd);
    die("[client] fstat netns fd");
  }
  close(fd);

  std::printf("pinner_socket=private\n");
  std::printf("socket_mode=%o\n", sock_st.st_mode & 0777);
  std::printf("netns_source=pinner-fd\n");
  std::printf("netns_dev=%llu netns_ino=%llu\n",
              static_cast<unsigned long long>(ns_st.st_dev),
              static_cast<unsigned long long>(ns_st.st_ino));

  FILE *fp = std::fopen(kPidPath, "r");
  if (fp) {
    char pid[64] {};
    if (std::fgets(pid, sizeof(pid), fp)) std::printf("pinner_pid=%s", pid);
    std::fclose(fp);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) die_msg("usage: scene-netnsctl {pin|enter|status} [-- command...]");
  std::string cmd = argv[1];
  if (cmd == "pin") {
    pin_forever();
  } else if (cmd == "enter") {
    enter_netns();
    int first = 2;
    if (first < argc && std::strcmp(argv[first], "--") == 0) ++first;
    exec_command(argc, argv, first);
  } else if (cmd == "status") {
    status();
  } else {
    die_msg("unknown command");
  }
}
