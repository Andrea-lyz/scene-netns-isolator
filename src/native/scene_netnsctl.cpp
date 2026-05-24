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
#include <sched.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../common/runtime_paths.h"

namespace {

using scene_netns::kEndpointPath;
using scene_netns::kPidPath;
using scene_netns::kRunDir;
using scene_netns::kUnixPathMax;

constexpr const char *kDefaultShell = "/system/bin/sh";
char g_sock_path[kUnixPathMax] = {};

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

int open_current_netns() {
  int fd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    die("[pinner] open /proc/self/ns/net");
  }
  return fd;
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

void pin_forever() {
  ensure_run_dir();
  cleanup_stale_endpoint();
  if (sys_unshare(CLONE_NEWNET) != 0) {
    die("[pinner] unshare(CLONE_NEWNET)");
  }
  bring_loopback_up();
  int ns_fd = open_current_netns();

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
      send_fd(client, ns_fd);
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
