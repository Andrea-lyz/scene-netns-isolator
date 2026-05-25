// SPDX-License-Identifier: GPL-3.0
#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zygisk.hpp>

#include "../common/proxy_protocol.h"
#include "../common/runtime_paths.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

using scene_netns::kCmdFetchNetns;
using scene_netns::kCmdHostConnect;
using scene_netns::kConnectFlagNonblock;
using scene_netns::kEndpointPath;
using scene_netns::kRunDir;
using scene_netns::kStatusErr;
using scene_netns::kStatusOk;
using scene_netns::kUnixPathMax;
using scene_netns::ProxyCommand;
using scene_netns::ProxyStatus;

constexpr const char *kTag = "SceneNetns";
constexpr const char *kPackage = "com.omarea.vtools";
constexpr const char *kModuleBin = "/data/adb/modules/scene-netns-isolator/bin";
constexpr const char *kFallbackPath = "/system/bin:/system/xbin:/vendor/bin";

void logi(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
  va_end(ap);
}

void loge(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(ANDROID_LOG_ERROR, kTag, fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// Tiny IO helpers for AF_UNIX SOCK_STREAM.  These speak in plain byte buffers;
// SCM_RIGHTS fd passing has its own helpers below.
// ---------------------------------------------------------------------------

bool write_full(int fd, const void *buf, size_t len) {
  const char *p = static_cast<const char *>(buf);
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

bool read_full(int fd, void *buf, size_t len) {
  char *p = static_cast<char *>(buf);
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

bool write_be32(int fd, uint32_t v) {
  uint32_t be = htonl(v);
  return write_full(fd, &be, sizeof(be));
}

bool read_be32(int fd, uint32_t *out) {
  uint32_t be = 0;
  if (!read_full(fd, &be, sizeof(be))) return false;
  *out = ntohl(be);
  return true;
}

// Send `data_byte` plus an optional fd over SCM_RIGHTS.  If fd < 0, no fd is
// transmitted (useful for error responses).
bool send_msg_with_fd(int sock, uint8_t status, int fd, const void *trailer,
                      size_t trailer_len) {
  struct iovec iov[2];
  iov[0].iov_base = &status;
  iov[0].iov_len = sizeof(status);
  size_t iovcnt = 1;
  if (trailer && trailer_len > 0) {
    iov[1].iov_base = const_cast<void *>(trailer);
    iov[1].iov_len = trailer_len;
    iovcnt = 2;
  }

  struct msghdr msg = {};
  msg.msg_iov = iov;
  msg.msg_iovlen = iovcnt;

  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  if (fd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  }

  ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
  return n >= 0;
}

// Receive one status byte plus optionally an fd via SCM_RIGHTS.  On error
// status, no fd is expected.  trailer (if non-null) gets exactly trailer_len
// bytes after the status byte.  *out_fd is set to -1 if no fd was received.
bool recv_msg_with_fd(int sock, uint8_t *out_status, int *out_fd,
                      void *trailer, size_t trailer_len) {
  *out_fd = -1;
  uint8_t status = 0;
  struct iovec iov[2];
  iov[0].iov_base = &status;
  iov[0].iov_len = sizeof(status);
  size_t iovcnt = 1;
  if (trailer && trailer_len > 0) {
    iov[1].iov_base = trailer;
    iov[1].iov_len = trailer_len;
    iovcnt = 2;
  }

  struct msghdr msg = {};
  msg.msg_iov = iov;
  msg.msg_iovlen = iovcnt;
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0) return false;
  if (n < 1) return false;
  *out_status = status;

  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr;
       c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
        c->cmsg_len == CMSG_LEN(sizeof(int))) {
      memcpy(out_fd, CMSG_DATA(c), sizeof(*out_fd));
      break;
    }
  }

  if (trailer && trailer_len > 0) {
    if (static_cast<size_t>(n) < 1 + trailer_len) {
      // Trailer truncated; bail.  Consumer must treat as error.
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// JNI helpers / process matching
// ---------------------------------------------------------------------------

bool copy_jstring(JNIEnv *env, jstring s, char *out, size_t out_size) {
  if (out_size == 0) return false;
  out[0] = '\0';
  if (!env || !s) return false;
  const char *chars = env->GetStringUTFChars(s, nullptr);
  if (!chars) return false;
  snprintf(out, out_size, "%s", chars);
  env->ReleaseStringUTFChars(s, chars);
  return true;
}

bool starts_with(const char *value, const char *prefix) {
  return strncmp(value, prefix, strlen(prefix)) == 0;
}

bool is_scene_process(JNIEnv *env, jstring nice_name_j, jstring app_data_dir_j,
                      char *nice_name, size_t nice_name_size,
                      char *app_data_dir, size_t app_data_dir_size) {
  copy_jstring(env, nice_name_j, nice_name, nice_name_size);
  copy_jstring(env, app_data_dir_j, app_data_dir, app_data_dir_size);
  if (strcmp(nice_name, kPackage) == 0 ||
      starts_with(nice_name, "com.omarea.vtools:")) {
    return true;
  }
  return strstr(app_data_dir, "/com.omarea.vtools") != nullptr;
}

int sys_setns(int fd, int nstype) {
  return static_cast<int>(syscall(__NR_setns, fd, nstype));
}

// ---------------------------------------------------------------------------
// Long-lived companion connection state, shared by all hooked threads.
// ---------------------------------------------------------------------------

struct ProxyState {
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  int sock = -1;            // long-lived AF_UNIX SOCK_STREAM to companion
  bool initialised = false; // true once setup attempt finished
};

ProxyState g_proxy;

// Forward decl of the original libc connect.
using connect_fn_t = int (*)(int, const struct sockaddr *, socklen_t);
connect_fn_t g_real_connect = nullptr;

bool is_loopback_v4(uint32_t ip_be) {
  // 127.0.0.0/8 in any byte order representation.  The kernel sees ip_be in
  // network byte order, so 0x7f is the high byte of the host-order address.
  return (ntohl(ip_be) & 0xff000000U) == 0x7f000000U;
}

bool is_loopback_v6(const struct in6_addr *a6) {
  // ::1
  if (IN6_IS_ADDR_LOOPBACK(a6)) return true;
  // v4-mapped loopback ::ffff:127.x.y.z
  if (IN6_IS_ADDR_V4MAPPED(a6)) {
    uint32_t ip_be;
    memcpy(&ip_be, &a6->s6_addr[12], sizeof(ip_be));
    return is_loopback_v4(ip_be);
  }
  return false;
}

bool target_is_loopback(const struct sockaddr *sa, socklen_t len) {
  if (!sa) return false;
  if (sa->sa_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
    const auto *in4 = reinterpret_cast<const sockaddr_in *>(sa);
    return is_loopback_v4(in4->sin_addr.s_addr);
  }
  if (sa->sa_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
    const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
    return is_loopback_v6(&in6->sin6_addr);
  }
  return false;
}

// Ask the companion for a freshly-connected socket living in host netns.
// On success returns the new fd (caller owns it).  On failure returns -1 and
// sets errno to whatever the companion reported.
int request_host_connected_fd(int domain, int type, int protocol,
                              const struct sockaddr *sa, socklen_t len,
                              uint32_t flags) {
  pthread_mutex_lock(&g_proxy.mu);
  int sock = g_proxy.sock;
  if (sock < 0) {
    pthread_mutex_unlock(&g_proxy.mu);
    errno = ENOTCONN;
    return -1;
  }

  uint8_t cmd = static_cast<uint8_t>(kCmdHostConnect);
  bool ok = write_full(sock, &cmd, sizeof(cmd));
  ok = ok && write_be32(sock, static_cast<uint32_t>(domain));
  ok = ok && write_be32(sock, static_cast<uint32_t>(type));
  ok = ok && write_be32(sock, static_cast<uint32_t>(protocol));
  ok = ok && write_be32(sock, static_cast<uint32_t>(len));
  ok = ok && write_full(sock, sa, len);
  ok = ok && write_be32(sock, flags);
  if (!ok) {
    loge("[hook] companion request write failed: %s", strerror(errno));
    pthread_mutex_unlock(&g_proxy.mu);
    errno = EIO;
    return -1;
  }

  uint8_t status = 0;
  int new_fd = -1;
  uint32_t errno_be = 0;
  // For both success and failure responses we tentatively read a 4-byte
  // trailer, then interpret based on status.  Companion always writes
  // exactly 5 bytes after the status byte either way, but for success the
  // trailing 4 bytes are reserved (zero) so we keep the framing fixed.
  char trailer[4] = {};
  ok = recv_msg_with_fd(sock, &status, &new_fd, trailer, sizeof(trailer));
  pthread_mutex_unlock(&g_proxy.mu);
  if (!ok) {
    loge("[hook] companion response read failed: %s", strerror(errno));
    if (new_fd >= 0) close(new_fd);
    errno = EIO;
    return -1;
  }

  if (status == kStatusOk && new_fd >= 0) {
    return new_fd;
  }

  if (new_fd >= 0) close(new_fd);
  memcpy(&errno_be, trailer, sizeof(errno_be));
  int err = static_cast<int>(ntohl(errno_be));
  errno = err > 0 ? err : ECONNREFUSED;
  return -1;
}

extern "C" int my_connect(int fd, const struct sockaddr *sa, socklen_t len) {
  if (!sa || !g_real_connect) {
    if (g_real_connect) return g_real_connect(fd, sa, len);
    errno = EFAULT;
    return -1;
  }

  // Loopback / non-IP: stay in isolated netns and use the real syscall.
  if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
    return g_real_connect(fd, sa, len);
  }
  if (target_is_loopback(sa, len)) {
    return g_real_connect(fd, sa, len);
  }

  // External target: fetch a connected fd from the host-netns companion and
  // dup it onto the caller-supplied fd.  This way the caller's fd number stays
  // valid for any selectors/epoll registrations they may have set up.
  int sock_type = 0;
  int protocol = 0;
  socklen_t opt_len = sizeof(sock_type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &opt_len) != 0) {
    sock_type = SOCK_STREAM;
  }
  // Bionic exposes SO_PROTOCOL on recent kernels; if it fails we let the
  // companion infer 0 (any).
#ifdef SO_PROTOCOL
  opt_len = sizeof(protocol);
  if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &opt_len) != 0) {
    protocol = 0;
  }
#endif

  int orig_flags = fcntl(fd, F_GETFL);
  if (orig_flags < 0) orig_flags = 0;
  uint32_t proxy_flags = 0;
  if (orig_flags & O_NONBLOCK) proxy_flags |= kConnectFlagNonblock;

  int host_fd = request_host_connected_fd(sa->sa_family, sock_type, protocol,
                                          sa, len, proxy_flags);
  if (host_fd < 0) {
    return -1;
  }

  // Replace caller fd with the host-connected one without changing fd number.
  if (dup2(host_fd, fd) < 0) {
    int saved = errno;
    close(host_fd);
    errno = saved;
    return -1;
  }
  close(host_fd);

  // Restore O_NONBLOCK if the caller had it set (dup2 preserves the open file
  // description's flags from host_fd, which we always opened in blocking
  // mode).  All other flags (O_CLOEXEC etc.) live on the fd flags side and
  // were already set on the original fd.
  if (orig_flags & O_NONBLOCK) {
    int cur = fcntl(fd, F_GETFL);
    if (cur >= 0 && !(cur & O_NONBLOCK)) {
      fcntl(fd, F_SETFL, cur | O_NONBLOCK);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// PLT hook installation: hook libc's connect() across every loaded ELF.
// ---------------------------------------------------------------------------

struct MapEntry {
  dev_t dev;
  ino_t inode;
  std::string path;
};

std::vector<MapEntry> collect_executable_mappings() {
  std::vector<MapEntry> result;
  std::set<std::pair<dev_t, ino_t>> seen;

  FILE *fp = fopen("/proc/self/maps", "re");
  if (!fp) return result;

  char *line = nullptr;
  size_t cap = 0;
  while (getline(&line, &cap, fp) > 0) {
    // Format: addr-addr perms offset dev inode path
    char perms[8] = {};
    unsigned long offset = 0;
    unsigned int dev_major = 0, dev_minor = 0;
    unsigned long inode_val = 0;
    int path_off = 0;
    int matched = sscanf(line, "%*lx-%*lx %7s %lx %x:%x %lu %n",
                         perms, &offset, &dev_major, &dev_minor, &inode_val,
                         &path_off);
    if (matched < 5) continue;
    if (perms[2] != 'x') continue;        // need executable
    if (inode_val == 0) continue;         // anonymous mapping
    const char *path = line + path_off;
    while (*path == ' ' || *path == '\t') ++path;
    if (*path == '\0' || *path == '\n') continue;
    if (*path == '[') continue;           // [vdso] etc.

    // Strip trailing newline
    std::string p = path;
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();

    dev_t dev = makedev(dev_major, dev_minor);
    ino_t inode = static_cast<ino_t>(inode_val);
    if (!seen.insert({dev, inode}).second) continue;
    result.push_back({dev, inode, std::move(p)});
  }
  free(line);
  fclose(fp);
  return result;
}

// Hook libc connect() in every loaded ELF that has a PLT entry for it.
// pltHookRegister tolerates missing symbols (they just become no-ops on
// commit), so we register against every executable mapping unconditionally.
bool install_connect_hooks(Api *api) {
  if (!api) return false;

  // Stash the real implementation up-front via dlsym so that g_real_connect is
  // populated even before any hooked library calls connect.  pltHookRegister
  // will also hand us back the original via oldFunc, but only after commit.
  void *libc = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
  if (libc) {
    g_real_connect = reinterpret_cast<connect_fn_t>(dlsym(libc, "connect"));
    dlclose(libc);
  }
  if (!g_real_connect) {
    g_real_connect = reinterpret_cast<connect_fn_t>(
        dlsym(RTLD_DEFAULT, "connect"));
  }
  if (!g_real_connect) {
    loge("[hook] failed to resolve real connect()");
    return false;
  }

  auto mappings = collect_executable_mappings();
  size_t registered = 0;
  for (const auto &m : mappings) {
    // Skip the dynamic linker / vdso-ish things; hooking them is pointless
    // and may crash.  Anything in /system/bin that's a binary (not .so) is
    // also unlikely to ever call connect().
    if (m.path.find(".so") == std::string::npos) continue;

    void *prev = nullptr;
    api->pltHookRegister(m.dev, m.inode, "connect",
                         reinterpret_cast<void *>(&my_connect), &prev);
    ++registered;
  }
  if (registered == 0) {
    loge("[hook] no eligible mappings for connect hook");
    return false;
  }

  if (!api->pltHookCommit()) {
    loge("[hook] pltHookCommit failed");
    return false;
  }
  logi("[hook] connect() PLT hook installed across %zu mappings",
       registered);
  return true;
}

// ---------------------------------------------------------------------------
// Pinner endpoint discovery (used by the companion for FETCH_NETNS).
// ---------------------------------------------------------------------------

bool path_is_in_run_dir(const char *path) {
  const size_t dir_len = strlen(kRunDir);
  if (strncmp(path, kRunDir, dir_len) != 0 || path[dir_len] != '/') {
    return false;
  }
  const char *leaf = path + dir_len + 1;
  return leaf[0] != '\0' && strchr(leaf, '/') == nullptr &&
         strlen(path) < kUnixPathMax;
}

bool read_endpoint_path(char *out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';

  int fd = open(kEndpointPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    loge("[companion] open pinner endpoint failed: %s", strerror(errno));
    return false;
  }

  struct stat st = {};
  if (fstat(fd, &st) != 0) {
    loge("[companion] stat pinner endpoint failed: %s", strerror(errno));
    close(fd);
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    loge("[companion] pinner endpoint is not private");
    close(fd);
    return false;
  }

  ssize_t n = read(fd, out, out_size - 1);
  if (n < 0) {
    loge("[companion] read pinner endpoint failed: %s", strerror(errno));
    close(fd);
    return false;
  }
  close(fd);

  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                   out[n - 1] == ' ' || out[n - 1] == '\t')) {
    out[--n] = '\0';
  }
  if (n == 0 || !path_is_in_run_dir(out)) {
    loge("[companion] pinner endpoint path is invalid");
    return false;
  }
  return true;
}

bool private_socket_exists(const char *path) {
  struct stat st = {};
  if (lstat(path, &st) != 0) {
    loge("[companion] stat pinner socket failed: %s", strerror(errno));
    return false;
  }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    loge("[companion] pinner socket is not private");
    return false;
  }
  return true;
}

void fill_sockaddr(sockaddr_un *addr, const char *path) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
}

int recv_single_fd(int sock) {
  uint8_t status = 0;
  int fd = -1;
  char trailer[4] = {};
  if (!recv_msg_with_fd(sock, &status, &fd, trailer, 0)) return -1;
  if (status != kStatusOk || fd < 0) {
    if (fd >= 0) close(fd);
    return -1;
  }
  return fd;
}

int request_pinner_netns_fd() {
  char sock_path[kUnixPathMax] = {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path)) ||
      !private_socket_exists(sock_path)) {
    return -1;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    loge("[companion] socket failed: %s", strerror(errno));
    return -1;
  }

  sockaddr_un addr = {};
  fill_sockaddr(&addr, sock_path);
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    loge("[companion] connect pinner failed: %s", strerror(errno));
    close(sock);
    return -1;
  }

  // The legacy pinner protocol just writes a single-byte 'N' plus SCM_RIGHTS
  // fd as soon as the client connects.  No request bytes from us.
  int fd = recv_single_fd(sock);
  close(sock);
  return fd;
}

// ---------------------------------------------------------------------------
// Companion: command loop.
// ---------------------------------------------------------------------------

void handle_fetch_netns(int client) {
  int ns_fd = request_pinner_netns_fd();
  if (ns_fd < 0) {
    char trailer[4] = {};
    uint32_t err_be = htonl(ECONNREFUSED);
    memcpy(trailer, &err_be, sizeof(err_be));
    send_msg_with_fd(client, kStatusErr, -1, trailer, sizeof(trailer));
    return;
  }
  send_msg_with_fd(client, kStatusOk, ns_fd, nullptr, 0);
  close(ns_fd);
}

void handle_host_connect(int client) {
  uint32_t domain = 0, type = 0, protocol = 0, addr_len = 0, flags = 0;
  if (!read_be32(client, &domain) || !read_be32(client, &type) ||
      !read_be32(client, &protocol) || !read_be32(client, &addr_len)) {
    return;
  }
  if (addr_len == 0 || addr_len > sizeof(sockaddr_storage)) {
    // Drain remaining bytes if any then bail.
    return;
  }
  std::vector<char> addr_buf(addr_len);
  if (!read_full(client, addr_buf.data(), addr_len)) return;
  if (!read_be32(client, &flags)) return;

  // Always create a blocking socket on our side.  The caller is free to set
  // O_NONBLOCK on the returned fd via fcntl in their own process.
  int new_type = static_cast<int>(type);
  // Strip non-blocking/cloexec request bits from the type so that the
  // connect below is deterministic.  Bionic's socket() copes with these,
  // but we want full control.
  new_type &= ~SOCK_NONBLOCK;
  new_type |= SOCK_CLOEXEC;

  int s = socket(static_cast<int>(domain), new_type,
                 static_cast<int>(protocol));
  if (s < 0) {
    int err = errno;
    char trailer[4] = {};
    uint32_t err_be = htonl(err > 0 ? err : EAFNOSUPPORT);
    memcpy(trailer, &err_be, sizeof(err_be));
    send_msg_with_fd(client, kStatusErr, -1, trailer, sizeof(trailer));
    return;
  }

  if (connect(s, reinterpret_cast<sockaddr *>(addr_buf.data()),
              static_cast<socklen_t>(addr_len)) != 0) {
    int err = errno;
    close(s);
    char trailer[4] = {};
    uint32_t err_be = htonl(err > 0 ? err : ECONNREFUSED);
    memcpy(trailer, &err_be, sizeof(err_be));
    send_msg_with_fd(client, kStatusErr, -1, trailer, sizeof(trailer));
    return;
  }

  // Suppress the unused-flag warning; flags is currently informational only.
  (void)flags;

  char trailer[4] = {};  // reserved zeros for protocol symmetry
  send_msg_with_fd(client, kStatusOk, s, trailer, sizeof(trailer));
  close(s);
}

void companion_handler(int client) {
  // Loop until peer closes the connection.  Each iteration consumes one
  // command byte and dispatches.  Errors bail the loop.
  for (;;) {
    uint8_t cmd = 0;
    if (!read_full(client, &cmd, sizeof(cmd))) break;

    switch (cmd) {
      case kCmdFetchNetns:
        handle_fetch_netns(client);
        break;
      case kCmdHostConnect:
        handle_host_connect(client);
        break;
      default:
        loge("[companion] unknown command: %u", cmd);
        // Don't try to parse trailing bytes; framing is now lost.
        close(client);
        return;
    }
  }
  close(client);
}

// ---------------------------------------------------------------------------
// Zygisk module: connects to companion, receives ns fd, setns, installs PLT
// hooks for connect, then keeps the proxy socket alive for the app's lifetime.
// ---------------------------------------------------------------------------

bool fetch_netns_and_keep_proxy(Api *api, int *out_ns_fd) {
  *out_ns_fd = -1;
  int sock = api->connectCompanion();
  if (sock < 0) {
    loge("[zygisk] connect companion failed");
    return false;
  }

  uint8_t cmd = static_cast<uint8_t>(kCmdFetchNetns);
  if (!write_full(sock, &cmd, sizeof(cmd))) {
    loge("[zygisk] send FETCH_NETNS failed: %s", strerror(errno));
    close(sock);
    return false;
  }

  uint8_t status = 0;
  int ns_fd = -1;
  if (!recv_msg_with_fd(sock, &status, &ns_fd, nullptr, 0)) {
    loge("[zygisk] recv FETCH_NETNS reply failed: %s", strerror(errno));
    close(sock);
    return false;
  }
  if (status != kStatusOk || ns_fd < 0) {
    loge("[zygisk] companion declined FETCH_NETNS (status=%u)", status);
    if (ns_fd >= 0) close(ns_fd);
    close(sock);
    return false;
  }

  *out_ns_fd = ns_fd;

  pthread_mutex_lock(&g_proxy.mu);
  g_proxy.sock = sock;
  g_proxy.initialised = true;
  pthread_mutex_unlock(&g_proxy.mu);

  // Tell zygote to leave our long-lived socket alone during specialization.
  if (!api->exemptFd(sock)) {
    loge("[zygisk] exemptFd(proxy_sock) failed; socket may be closed by zygote");
  }
  return true;
}

void prepend_path_for_su_wrapper() {
  const char *prefix = kModuleBin;
  const char *old_path = getenv("PATH");
  char path[4096];
  if (old_path && old_path[0] != '\0') {
    snprintf(path, sizeof(path), "%s:%s", prefix, old_path);
  } else {
    snprintf(path, sizeof(path), "%s:%s", prefix, kFallbackPath);
  }
  setenv("PATH", path, 1);
  setenv("SCENE_NETNS_ISOLATED", "1", 1);
  logi("[zygisk] PATH prefixed with %s", prefix);
}

class SceneNetnsModule : public zygisk::ModuleBase {
 public:
  void onLoad(Api *api, JNIEnv *env) override {
    this->api = api;
    this->env = env;
  }

  void preAppSpecialize(AppSpecializeArgs *args) override {
    char nice_name[256] = {};
    char app_data_dir[512] = {};

    if (!is_scene_process(env, args->nice_name, args->app_data_dir,
                          nice_name, sizeof(nice_name),
                          app_data_dir, sizeof(app_data_dir))) {
      // Non-Scene process: safe to be unloaded once specialize finishes.
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }

    int ns_fd = -1;
    if (!fetch_netns_and_keep_proxy(api, &ns_fd)) {
      loge("[zygisk] failed to acquire netns/proxy: %s", nice_name);
      // Without proxy we cannot safely enter the netns: that would break
      // outbound connectivity entirely.  Fall back to leaving the process
      // in host netns, which is the same as not having the module installed.
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }

    if (sys_setns(ns_fd, CLONE_NEWNET) != 0) {
      loge("[zygisk] setns failed: %s", strerror(errno));
      close(ns_fd);
      pthread_mutex_lock(&g_proxy.mu);
      if (g_proxy.sock >= 0) {
        close(g_proxy.sock);
        g_proxy.sock = -1;
      }
      pthread_mutex_unlock(&g_proxy.mu);
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }
    close(ns_fd);

    if (!install_connect_hooks(api)) {
      loge("[zygisk] PLT hook installation failed; outbound traffic will fail");
      // Note: we deliberately do NOT request DLCLOSE here, because a partial
      // install may have already swapped some PLT entries.  Unloading would
      // crash the app on the next external connect().
      prepend_path_for_su_wrapper();
      return;
    }

    prepend_path_for_su_wrapper();
    // Important: do NOT call setOption(DLCLOSE_MODULE_LIBRARY) once PLT hooks
    // are live.  Our my_connect would be torn out from under the app.

    logi("[zygisk] Scene process isolated with proxy: nice_name=%s app_data_dir=%s",
         nice_name, app_data_dir);
  }

  void postAppSpecialize(const AppSpecializeArgs *) override {}

  void preServerSpecialize(ServerSpecializeArgs *) override {
    api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
  }

 private:
  Api *api = nullptr;
  JNIEnv *env = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SceneNetnsModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
