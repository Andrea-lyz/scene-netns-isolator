// SPDX-License-Identifier: GPL-3.0
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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

#include "../common/runtime_paths.h"

#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif

#ifndef IP6T_SO_ORIGINAL_DST
#define IP6T_SO_ORIGINAL_DST 80
#endif

namespace {

using scene_netns::kEndpointPath;
using scene_netns::kPidPath;
using scene_netns::kRunDir;
using scene_netns::kUnixPathMax;

constexpr const char *kDefaultShell = "/system/bin/sh";

char g_sock_path[kUnixPathMax] = {};
int g_host_ns_fd = -1;        // captured before unshare(): pinner's original netns
int g_isolated_ns_fd = -1;    // captured after unshare():  the new isolated netns

void die(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s: %s\n", message, std::strerror(errno));
  std::exit(1);
}

void die_msg(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s\n", message);
  std::exit(1);
}

void ensure_run_dir() {
  // 0755 so that processes inside the isolated netns (apps, root daemons,
  // anything we explicitly setns into) can later traverse the directory if we
  // ever publish world-readable status files. Currently nothing in here is
  // world-readable, but 0755 on the directory itself is safe.
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
  if (!out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  int fd = open(kEndpointPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (!quiet) {
      die("[client] open pinner endpoint");
    }
    return false;
  }

  struct stat st {};
  if (fstat(fd, &st) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    if (!quiet) {
      die("[client] stat pinner endpoint");
    }
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    close(fd);
    if (!quiet) {
      die_msg("[client] pinner endpoint is not private");
    }
    return false;
  }

  ssize_t n = read(fd, out, out_size - 1);
  if (n < 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    if (!quiet) {
      die("[client] read pinner endpoint");
    }
    return false;
  }
  close(fd);

  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                   out[n - 1] == ' ' || out[n - 1] == '\t')) {
    out[--n] = '\0';
  }
  if (n == 0 || !path_is_in_run_dir(out)) {
    if (!quiet) {
      die_msg("[client] pinner endpoint path is invalid");
    }
    return false;
  }
  return true;
}

void require_private_socket(const char *path) {
  struct stat st {};
  if (lstat(path, &st) != 0) {
    die("[client] stat pinner socket");
  }
  if (!S_ISSOCK(st.st_mode)) {
    die_msg("[client] pinner endpoint exists but is not a socket");
  }
  if (st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    die_msg("[client] pinner socket is not private");
  }
}

void bring_loopback_up() {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    die("socket");
  }

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

int sys_setns(int fd, int nstype) {
  return static_cast<int>(syscall(__NR_setns, fd, nstype));
}

int sys_unshare(int flags) {
  return static_cast<int>(syscall(__NR_unshare, flags));
}

void write_pid_file() {
  FILE *fp = std::fopen(kPidPath, "w");
  if (!fp) {
    die("[pinner] open pid file");
  }
  std::fprintf(fp, "%d\n", getpid());
  std::fclose(fp);
  chmod(kPidPath, 0600);
}

int open_netns(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    die("[pinner] open netns");
  }
  return fd;
}

int open_current_netns() {
  return open_netns("/proc/self/ns/net");
}

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
  if (sock < 0) {
    die("[client] socket");
  }

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

  if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) < 0) {
    die("[client] recv netns fd");
  }

  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    die_msg("[client] pinner did not send a namespace fd");
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
  if (fd < 0) {
    die_msg("[client] received invalid namespace fd");
  }
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
    std::fprintf(stderr, "scene-netnsctl: [pinner] send netns fd failed: %s\n",
                 std::strerror(errno));
  }
}

bool peer_is_allowed(int sock) {
  struct PeerCred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
  } cred {};
  socklen_t len = sizeof(cred);
  if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    std::fprintf(stderr, "scene-netnsctl: [pinner] peer credential check failed: %s\n",
                 std::strerror(errno));
    return false;
  }
  if (cred.uid != 0) {
    std::fprintf(stderr,
                 "scene-netnsctl: [pinner] rejected non-root peer: pid=%d uid=%u gid=%u\n",
                 static_cast<int>(cred.pid), static_cast<unsigned>(cred.uid),
                 static_cast<unsigned>(cred.gid));
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

uint64_t random_u64() {
  uint64_t value = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, &value, sizeof(value));
    close(fd);
    if (n == static_cast<ssize_t>(sizeof(value))) {
      return value;
    }
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
    if (lstat(path, &st) != 0 && errno == ENOENT) {
      return path;
    }
  }
  die_msg("[pinner] unable to allocate private socket path");
  return {};
}

void write_endpoint_file(const char *sock_path) {
  int fd = open(kEndpointPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    die("[pinner] open endpoint file");
  }

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
  if (read_endpoint_path(stale_path, sizeof(stale_path), true)) {
    unlink(stale_path);
  }
  unlink(kEndpointPath);
  unlink(kPidPath);
}

// ---------------------------------------------------------------------------
// In-netns transparent TCP proxy.
//
// We rely on iptables nat OUTPUT REDIRECT to bend every non-loopback TCP
// connect inside the isolated netns to our local listener. The kernel keeps
// the original destination accessible via getsockopt(SO_ORIGINAL_DST), so the
// app process needs no userspace cooperation: it just calls connect() on
// the real address as if the host network was reachable.
//
// On accept() the proxy:
//   1. reads the original destination via SO_ORIGINAL_DST
//   2. setns(host) so the outbound socket is born in host netns
//   3. dial the real destination
//   4. setns(isolated) so the next accept() runs in the right ns
//   5. splice bytes between client and upstream until either side closes
// ---------------------------------------------------------------------------

bool write_full_blocking(int fd, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

void splice_loop(int a, int b) {
  bool a_to_b_open = true;
  bool b_to_a_open = true;
  uint8_t buf[16384];

  while (a_to_b_open || b_to_a_open) {
    pollfd pfds[2];
    int n = 0;
    if (a_to_b_open) {
      pfds[n].fd = a;
      pfds[n].events = POLLIN;
      pfds[n].revents = 0;
      ++n;
    }
    if (b_to_a_open) {
      pfds[n].fd = b;
      pfds[n].events = POLLIN;
      pfds[n].revents = 0;
      ++n;
    }
    int rv = poll(pfds, n, -1);
    if (rv < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int from = pfds[i].fd;
      int to = (from == a) ? b : a;
      bool *side_open = (from == a) ? &a_to_b_open : &b_to_a_open;
      ssize_t r = recv(from, buf, sizeof(buf), 0);
      if (r > 0) {
        if (!write_full_blocking(to, buf, static_cast<size_t>(r))) {
          a_to_b_open = false;
          b_to_a_open = false;
        }
      } else if (r == 0 || (r < 0 && errno != EINTR)) {
        *side_open = false;
        shutdown(to, SHUT_WR);
      }
    }
  }
}

struct ProxyJob {
  int client_fd;
  int family;  // AF_INET or AF_INET6
};

void *proxy_worker(void *arg) {
  ProxyJob *job = static_cast<ProxyJob *>(arg);
  int client = job->client_fd;
  int family = job->family;
  delete job;

  // Pull the original destination out of the connection-tracker. This works
  // for v4 and v6 once the corresponding REDIRECT rule fired.
  sockaddr_storage orig {};
  socklen_t orig_len = sizeof(orig);
  bool got_orig = false;
  if (family == AF_INET) {
    if (getsockopt(client, SOL_IP, SO_ORIGINAL_DST, &orig, &orig_len) == 0) {
      got_orig = true;
    } else {
      std::fprintf(stderr,
                   "scene-netnsctl: [proxy] SO_ORIGINAL_DST(v4) failed: %s\n",
                   std::strerror(errno));
    }
  } else if (family == AF_INET6) {
    orig_len = sizeof(orig);
    if (getsockopt(client, SOL_IPV6, IP6T_SO_ORIGINAL_DST, &orig, &orig_len) == 0) {
      got_orig = true;
    } else {
      std::fprintf(stderr,
                   "scene-netnsctl: [proxy] SO_ORIGINAL_DST(v6) failed: %s\n",
                   std::strerror(errno));
    }
  }

  if (!got_orig) {
    close(client);
    return nullptr;
  }

  // Switch this thread into host netns to make the outbound connection.
  if (sys_setns(g_host_ns_fd, CLONE_NEWNET) != 0) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] setns(host) failed: %s\n",
                 std::strerror(errno));
    close(client);
    return nullptr;
  }

  int upstream = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int connect_err = 0;
  if (upstream < 0) {
    connect_err = errno;
  } else {
    socklen_t addr_size = (family == AF_INET) ? sizeof(sockaddr_in)
                                              : sizeof(sockaddr_in6);
    if (connect(upstream, reinterpret_cast<sockaddr *>(&orig), addr_size) != 0) {
      connect_err = errno;
      close(upstream);
      upstream = -1;
    }
  }

  // Restore namespace before doing anything that might touch sockets we
  // expect to live in the isolated ns.
  if (sys_setns(g_isolated_ns_fd, CLONE_NEWNET) != 0) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] setns(isolated) restore failed: %s\n",
                 std::strerror(errno));
  }

  if (upstream < 0) {
    (void)connect_err;  // logged via stderr above by the kernel; nothing more we can do
    close(client);
    return nullptr;
  }

  splice_loop(client, upstream);
  close(upstream);
  close(client);
  return nullptr;
}

uint16_t bind_listener(int family, int *out_listener_fd) {
  int listener = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    die("[proxy] socket");
  }
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  uint16_t port = 0;
  if (family == AF_INET) {
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      die("[proxy] bind v4");
    }
    socklen_t l = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &l);
    port = ntohs(addr.sin_port);
  } else {
    sockaddr_in6 addr {};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      die("[proxy] bind v6");
    }
    socklen_t l = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &l);
    port = ntohs(addr.sin6_port);
  }
  if (listen(listener, 64) != 0) {
    die("[proxy] listen");
  }
  *out_listener_fd = listener;
  return port;
}

struct AcceptArg {
  int listener;
  int family;
};

void *proxy_acceptor(void *arg) {
  AcceptArg *a = static_cast<AcceptArg *>(arg);
  int listener = a->listener;
  int family = a->family;
  delete a;

  for (;;) {
    int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "scene-netnsctl: [proxy] accept failed: %s\n",
                   std::strerror(errno));
      continue;
    }
    int flags = fcntl(client, F_GETFD);
    if (flags >= 0) fcntl(client, F_SETFD, flags | FD_CLOEXEC);

    auto *job = new ProxyJob{client, family};
    pthread_t tid;
    pthread_attr_t pattr;
    pthread_attr_init(&pattr);
    pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &pattr, proxy_worker, job) != 0) {
      std::fprintf(stderr, "scene-netnsctl: [proxy] pthread_create failed\n");
      delete job;
      close(client);
    }
    pthread_attr_destroy(&pattr);
  }
  return nullptr;
}

bool run_iptables(const char *bin, const char *family_label,
                  const char *const *argv) {
  // Capture child output so we can print it on failure and learn why the OEM
  // kernel rejected the rule (typical: "table 'nat' does not exist", or "no
  // chain/target/match by that name").
  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) {
    pipefd[0] = pipefd[1] = -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
    std::fprintf(stderr, "scene-netnsctl: [proxy] fork %s failed: %s\n",
                 bin, std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    if (pipefd[1] >= 0) {
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
    }
    execv(bin, const_cast<char *const *>(argv));
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
    std::fprintf(stderr, "scene-netnsctl: [proxy] waitpid %s: %s\n",
                 bin, std::strerror(errno));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] %s (%s) failed exit=%d output=<<%s>>\n",
                 bin, family_label, exit_code,
                 captured.empty() ? "(empty)" : captured.c_str());
    return false;
  }
  return true;
}

bool find_iptables_binary(const char *family,
                          char *out_path, size_t out_size) {
  // Order: prefer the legacy binary because Android's nf_tables backend on
  // many OEM kernels lacks the `nat` table.  iptables-legacy talks ip_tables.so
  // which still exposes nat OUTPUT in most ROMs.
  const char *prefixes[] = {
      "/system/bin/",
      "/system/xbin/",
      "/vendor/bin/",
      nullptr,
  };
  const char *names_v4[] = {"iptables-legacy", "iptables", nullptr};
  const char *names_v6[] = {"ip6tables-legacy", "ip6tables", nullptr};
  const char **names = (strcmp(family, "ipv6") == 0) ? names_v6 : names_v4;
  for (const char **p = prefixes; *p; ++p) {
    for (const char **n = names; *n; ++n) {
      char candidate[256];
      std::snprintf(candidate, sizeof(candidate), "%s%s", *p, *n);
      if (access(candidate, X_OK) == 0) {
        std::snprintf(out_path, out_size, "%s", candidate);
        return true;
      }
    }
  }
  return false;
}

bool install_redirect_rules(uint16_t v4_port, uint16_t v6_port) {
  char v4_port_str[8];
  char v6_port_str[8];
  std::snprintf(v4_port_str, sizeof(v4_port_str), "%u", v4_port);
  std::snprintf(v6_port_str, sizeof(v6_port_str), "%u", v6_port);

  char v4_bin[256] = {};
  char v6_bin[256] = {};
  bool have_v4_bin = find_iptables_binary("ipv4", v4_bin, sizeof(v4_bin));
  bool have_v6_bin = find_iptables_binary("ipv6", v6_bin, sizeof(v6_bin));
  if (!have_v4_bin) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] no iptables binary available; "
                 "outbound redirect cannot be installed\n");
    return false;
  }

  std::fprintf(stderr, "scene-netnsctl: [proxy] using v4=%s v6=%s\n",
               v4_bin, have_v6_bin ? v6_bin : "(none)");

  const char *v4_args[] = {
      v4_bin, "-t", "nat", "-A", "OUTPUT",
      "-p", "tcp", "!", "-d", "127.0.0.0/8",
      "-j", "REDIRECT", "--to-ports", v4_port_str, nullptr};

  bool ok_v4 = run_iptables(v4_bin, "ipv4", v4_args);

  bool ok_v6 = false;
  if (have_v6_bin) {
    const char *v6_args[] = {
        v6_bin, "-t", "nat", "-A", "OUTPUT",
        "-p", "tcp", "!", "-d", "::1",
        "-j", "REDIRECT", "--to-ports", v6_port_str, nullptr};
    ok_v6 = run_iptables(v6_bin, "ipv6", v6_args);
  }

  if (!ok_v6) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] ipv6 redirect unavailable; "
                 "v6 outbound traffic will not work\n");
  }
  return ok_v4;
}

void pin_forever() {
  ensure_run_dir();
  cleanup_stale_endpoint();

  // Capture the host netns fd BEFORE unshare so proxy workers can switch
  // back into it to dial real destinations.
  g_host_ns_fd = open_current_netns();

  if (sys_unshare(CLONE_NEWNET) != 0) {
    die("[pinner] unshare(CLONE_NEWNET)");
  }
  bring_loopback_up();
  g_isolated_ns_fd = open_current_netns();

  // Bind the legacy unix socket used to hand out the netns fd.
  int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0) {
    die("[pinner] socket");
  }
  std::string sock_path = make_socket_path();
  std::snprintf(g_sock_path, sizeof(g_sock_path), "%s", sock_path.c_str());
  unlink(g_sock_path);
  sockaddr_un addr {};
  fill_sockaddr(&addr, g_sock_path);
  if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    die("[pinner] bind socket");
  }
  chmod(g_sock_path, 0600);
  if (listen(server, 16) != 0) {
    die("[pinner] listen socket");
  }
  write_endpoint_file(g_sock_path);
  write_pid_file();

  // Start the in-netns transparent TCP proxy: one v4 listener and one v6.
  int v4_listener = -1;
  int v6_listener = -1;
  uint16_t v4_port = bind_listener(AF_INET, &v4_listener);
  uint16_t v6_port = bind_listener(AF_INET6, &v6_listener);
  std::fprintf(stderr, "scene-netnsctl: [proxy] v4 listener on 127.0.0.1:%u\n",
               static_cast<unsigned>(v4_port));
  std::fprintf(stderr, "scene-netnsctl: [proxy] v6 listener on [::1]:%u\n",
               static_cast<unsigned>(v6_port));

  // Plumb iptables nat OUTPUT inside the isolated netns. Any TCP connect
  // (other than to loopback) is now redirected to our listener; the kernel
  // remembers the original destination for SO_ORIGINAL_DST.
  //
  // If the kernel does not allow nat OUTPUT inside our netns we deliberately
  // do NOT die here.  The unix-socket netns service still works (so Zygisk
  // can put Scene in this netns) and the proxy listener exists; outbound
  // traffic will fail, but Scene's loopback isolation is preserved and the
  // operator gets a chance to inspect the situation.
  bool redirect_ok = install_redirect_rules(v4_port, v6_port);
  if (!redirect_ok) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] iptables redirect not installed; "
                 "outbound traffic will not work until this is resolved\n");
  } else {
    std::fprintf(stderr, "scene-netnsctl: [proxy] iptables redirect installed\n");
  }

  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);

  pthread_t v4_tid, v6_tid;
  auto *v4_arg = new AcceptArg{v4_listener, AF_INET};
  if (pthread_create(&v4_tid, &pattr, proxy_acceptor, v4_arg) != 0) {
    delete v4_arg;
    die("[proxy] pthread_create v4");
  }
  auto *v6_arg = new AcceptArg{v6_listener, AF_INET6};
  if (pthread_create(&v6_tid, &pattr, proxy_acceptor, v6_arg) != 0) {
    delete v6_arg;
    std::fprintf(stderr, "scene-netnsctl: [proxy] v6 acceptor not started\n");
  }
  pthread_attr_destroy(&pattr);

  std::signal(SIGTERM, [](int) {
    if (g_sock_path[0] != '\0') {
      unlink(g_sock_path);
    }
    unlink(kEndpointPath);
    unlink(kPidPath);
    _exit(0);
  });

  for (;;) {
    int client = accept(server, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "scene-netnsctl: [pinner] accept failed: %s\n",
                   std::strerror(errno));
      continue;
    }
    int flags = fcntl(client, F_GETFD);
    if (flags >= 0) {
      fcntl(client, F_SETFD, flags | FD_CLOEXEC);
    }
    if (peer_is_allowed(client)) {
      send_fd(client, g_isolated_ns_fd);
    }
    close(client);
  }
}

void status() {
  char sock_path[kUnixPathMax] {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path), false)) {
    die_msg("[client] pinner endpoint is unavailable");
  }
  struct stat sock_st {};
  if (lstat(sock_path, &sock_st) != 0) {
    die("[client] stat pinner socket");
  }
  if (!S_ISSOCK(sock_st.st_mode)) {
    die_msg("[client] pinner endpoint exists but is not a socket");
  }
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
    if (std::fgets(pid, sizeof(pid), fp)) {
      std::printf("pinner_pid=%s", pid);
    }
    std::fclose(fp);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    die_msg("usage: scene-netnsctl {pin|enter|status} [-- command...]");
  }

  std::string cmd = argv[1];
  if (cmd == "pin") {
    pin_forever();
  } else if (cmd == "enter") {
    enter_netns();
    int first = 2;
    if (first < argc && std::strcmp(argv[first], "--") == 0) {
      ++first;
    }
    exec_command(argc, argv, first);
  } else if (cmd == "status") {
    status();
  } else {
    die_msg("unknown command");
  }
}
