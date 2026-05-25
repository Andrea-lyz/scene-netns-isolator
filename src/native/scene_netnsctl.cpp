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
#include <unistd.h>

#include "../common/proxy_protocol.h"
#include "../common/runtime_paths.h"

namespace {

using scene_netns::kEndpointPath;
using scene_netns::kPidPath;
using scene_netns::kProxyFamilyV4;
using scene_netns::kProxyFamilyV6;
using scene_netns::kProxyHandshakeSize;
using scene_netns::kProxyHandshakeVersion;
using scene_netns::kProxyPortPath;
using scene_netns::kProxyStatusErr;
using scene_netns::kProxyStatusOk;
using scene_netns::kRunDir;
using scene_netns::kUnixPathMax;

constexpr const char *kDefaultShell = "/system/bin/sh";

char g_sock_path[kUnixPathMax] = {};
int g_host_ns_fd = -1;        // captured before unshare(): pinner's original netns
int g_isolated_ns_fd = -1;    // captured after unshare(): the new isolated netns

void die(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s: %s\n", message, std::strerror(errno));
  std::exit(1);
}

void die_msg(const char *message) {
  std::fprintf(stderr, "scene-netnsctl: %s\n", message);
  std::exit(1);
}

void ensure_run_dir() {
  if (mkdir(kRunDir, 0700) != 0 && errno != EEXIST) {
    die("mkdir run dir");
  }
  chmod(kRunDir, 0700);
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

void write_proxy_port_file(uint16_t port) {
  // World-readable: any process inside the isolated netns must be able to
  // discover the proxy port without needing privileged IPC.  The data leaked
  // is just a port number, which is meaningless outside the isolated netns.
  int fd = open(kProxyPortPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) {
    die("[pinner] open proxy port file");
  }
  char buf[16];
  int n = std::snprintf(buf, sizeof(buf), "%u\n", static_cast<unsigned>(port));
  if (write(fd, buf, n) != n) {
    int saved = errno;
    close(fd);
    errno = saved;
    die("[pinner] write proxy port file");
  }
  close(fd);
  chmod(kProxyPortPath, 0644);
}

void cleanup_stale_endpoint() {
  char stale_path[kUnixPathMax] {};
  if (read_endpoint_path(stale_path, sizeof(stale_path), true)) {
    unlink(stale_path);
  }
  unlink(kEndpointPath);
  unlink(kPidPath);
  unlink(kProxyPortPath);
}

// ---------------------------------------------------------------------------
// Transparent TCP proxy living inside the isolated netns.
//
// When the Zygisk module wants to reach the public internet it pretends to
// connect() to its real target but is silently redirected (in user space, via
// PLT hooks) to 127.0.0.1:<proxy_port> in the isolated netns.  It then writes
// a fixed-size handshake describing the original destination.  The proxy
// reads the handshake, dials the target from the host netns, replies with
// the status byte, and pipes bytes between the two halves until either side
// closes.
// ---------------------------------------------------------------------------

bool read_full_blocking(int fd, void *buf, size_t len) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
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

void send_proxy_error(int client_fd, int err) {
  uint8_t status = kProxyStatusErr;
  uint32_t err_be = htonl(err > 0 ? static_cast<uint32_t>(err)
                                  : static_cast<uint32_t>(ECONNREFUSED));
  write_full_blocking(client_fd, &status, sizeof(status));
  write_full_blocking(client_fd, &err_be, sizeof(err_be));
}

void splice_loop(int a, int b) {
  // Half-duplex shutdown: when one side closes for read we shutdown(write) on
  // the other.  Connection is fully torn down once both directions are done.
  bool a_to_b_open = true;
  bool b_to_a_open = true;
  uint8_t buf[16384];

  pollfd pfds[2];
  while (a_to_b_open || b_to_a_open) {
    int n = 0;
    if (a_to_b_open) {
      pfds[n].fd = a;
      pfds[n].events = POLLIN;
      ++n;
    }
    if (b_to_a_open) {
      pfds[n].fd = b;
      pfds[n].events = POLLIN;
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
};

void *proxy_worker(void *arg) {
  ProxyJob *job = static_cast<ProxyJob *>(arg);
  int client = job->client_fd;
  delete job;

  uint8_t handshake[kProxyHandshakeSize] = {};
  if (!read_full_blocking(client, handshake, sizeof(handshake))) {
    close(client);
    return nullptr;
  }
  if (handshake[0] != kProxyHandshakeVersion) {
    send_proxy_error(client, EPROTO);
    close(client);
    return nullptr;
  }

  uint8_t family = handshake[1];
  uint16_t port_be = 0;
  std::memcpy(&port_be, handshake + 2, sizeof(port_be));

  // Switch this thread into host netns to make the outbound connection.
  if (sys_setns(g_host_ns_fd, CLONE_NEWNET) != 0) {
    int err = errno;
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] setns(host) failed: %s\n",
                 std::strerror(err));
    send_proxy_error(client, err);
    close(client);
    return nullptr;
  }

  int upstream = -1;
  int connect_err = 0;
  if (family == kProxyFamilyV4) {
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = port_be;
    std::memcpy(&dst.sin_addr, handshake + 4, sizeof(dst.sin_addr));
    upstream = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (upstream < 0) {
      connect_err = errno;
    } else if (connect(upstream, reinterpret_cast<sockaddr *>(&dst),
                       sizeof(dst)) != 0) {
      connect_err = errno;
      close(upstream);
      upstream = -1;
    }
  } else if (family == kProxyFamilyV6) {
    sockaddr_in6 dst {};
    dst.sin6_family = AF_INET6;
    dst.sin6_port = port_be;
    std::memcpy(&dst.sin6_addr, handshake + 4, sizeof(dst.sin6_addr));
    upstream = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (upstream < 0) {
      connect_err = errno;
    } else if (connect(upstream, reinterpret_cast<sockaddr *>(&dst),
                       sizeof(dst)) != 0) {
      connect_err = errno;
      close(upstream);
      upstream = -1;
    }
  } else {
    connect_err = EAFNOSUPPORT;
  }

  // Switch back to isolated netns so subsequent accept()s are correct.  Any
  // failure here is non-fatal; we just log.
  if (sys_setns(g_isolated_ns_fd, CLONE_NEWNET) != 0) {
    std::fprintf(stderr,
                 "scene-netnsctl: [proxy] setns(isolated) restore failed: %s\n",
                 std::strerror(errno));
  }

  if (upstream < 0) {
    send_proxy_error(client, connect_err);
    close(client);
    return nullptr;
  }

  uint8_t ok = kProxyStatusOk;
  if (!write_full_blocking(client, &ok, sizeof(ok))) {
    close(upstream);
    close(client);
    return nullptr;
  }

  splice_loop(client, upstream);
  close(upstream);
  close(client);
  return nullptr;
}

uint16_t start_proxy_listener(int *out_listener_fd) {
  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    die("[proxy] socket");
  }
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // kernel-assigned ephemeral port
  if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    die("[proxy] bind");
  }
  socklen_t len = sizeof(addr);
  if (getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    die("[proxy] getsockname");
  }
  if (listen(listener, 64) != 0) {
    die("[proxy] listen");
  }
  *out_listener_fd = listener;
  return ntohs(addr.sin_port);
}

void *proxy_acceptor(void *arg) {
  int listener = *static_cast<int *>(arg);
  delete static_cast<int *>(arg);

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

    auto *job = new ProxyJob{client};
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

  // Start the in-netns transparent TCP proxy.
  int proxy_listener = -1;
  uint16_t proxy_port = start_proxy_listener(&proxy_listener);
  write_proxy_port_file(proxy_port);
  std::fprintf(stderr, "scene-netnsctl: [proxy] listening on 127.0.0.1:%u\n",
               static_cast<unsigned>(proxy_port));

  pthread_t accept_tid;
  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);
  int *listener_arg = new int(proxy_listener);
  if (pthread_create(&accept_tid, &pattr, proxy_acceptor, listener_arg) != 0) {
    delete listener_arg;
    die("[proxy] pthread_create acceptor");
  }
  pthread_attr_destroy(&pattr);

  std::signal(SIGTERM, [](int) {
    if (g_sock_path[0] != '\0') {
      unlink(g_sock_path);
    }
    unlink(kEndpointPath);
    unlink(kPidPath);
    unlink(kProxyPortPath);
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

  fp = std::fopen(kProxyPortPath, "r");
  if (fp) {
    char port[16] {};
    if (std::fgets(port, sizeof(port), fp)) {
      // strip trailing newline
      size_t n = std::strlen(port);
      while (n > 0 && (port[n - 1] == '\n' || port[n - 1] == '\r')) {
        port[--n] = '\0';
      }
      std::printf("proxy_port=%s\n", port);
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
