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
#include <sys/sysmacros.h>
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

using scene_netns::kEndpointPath;
using scene_netns::kProxyFamilyV4;
using scene_netns::kProxyFamilyV6;
using scene_netns::kProxyHandshakeSize;
using scene_netns::kProxyHandshakeVersion;
using scene_netns::kProxyPortPath;
using scene_netns::kProxyStatusErr;
using scene_netns::kProxyStatusOk;
using scene_netns::kRunDir;
using scene_netns::kUnixPathMax;

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
// Proxy port discovery + outbound redirect.
//
// The pinner publishes the in-netns transparent TCP proxy port to a small
// world-readable file inside /dev/.15f1c4b9.  Any process that has entered
// the isolated netns (i.e. our hooked target app) can read the port and
// connect to 127.0.0.1:<port>, then send a handshake describing the original
// destination.
// ---------------------------------------------------------------------------

uint16_t g_proxy_port = 0;            // populated once; 0 means unavailable
using connect_fn_t = int (*)(int, const struct sockaddr *, socklen_t);
connect_fn_t g_real_connect = nullptr;

bool read_proxy_port_once() {
  if (g_proxy_port != 0) return true;
  int fd = open(kProxyPortPath, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    loge("[hook] open proxy port file failed: %s", strerror(errno));
    return false;
  }
  char buf[16] = {};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    loge("[hook] proxy port file empty");
    return false;
  }
  buf[n] = '\0';
  unsigned long port = strtoul(buf, nullptr, 10);
  if (port == 0 || port > 65535) {
    loge("[hook] proxy port file invalid: %s", buf);
    return false;
  }
  g_proxy_port = static_cast<uint16_t>(port);
  return true;
}

bool is_loopback_v4(uint32_t ip_be) {
  return (ntohl(ip_be) & 0xff000000U) == 0x7f000000U;
}

bool is_loopback_v6(const struct in6_addr *a6) {
  if (IN6_IS_ADDR_LOOPBACK(a6)) return true;
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

bool build_handshake(const struct sockaddr *sa, socklen_t len,
                     uint8_t out[kProxyHandshakeSize]) {
  memset(out, 0, kProxyHandshakeSize);
  out[0] = kProxyHandshakeVersion;
  if (sa->sa_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
    const auto *in4 = reinterpret_cast<const sockaddr_in *>(sa);
    out[1] = kProxyFamilyV4;
    memcpy(out + 2, &in4->sin_port, sizeof(in4->sin_port));
    memcpy(out + 4, &in4->sin_addr, sizeof(in4->sin_addr));
    return true;
  }
  if (sa->sa_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
    const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
    out[1] = kProxyFamilyV6;
    memcpy(out + 2, &in6->sin6_port, sizeof(in6->sin6_port));
    memcpy(out + 4, &in6->sin6_addr, sizeof(in6->sin6_addr));
    return true;
  }
  return false;
}

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

extern "C" int my_connect(int fd, const struct sockaddr *sa, socklen_t len) {
  if (!sa || !g_real_connect) {
    if (g_real_connect) return g_real_connect(fd, sa, len);
    errno = EFAULT;
    return -1;
  }

  if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
    return g_real_connect(fd, sa, len);
  }
  if (target_is_loopback(sa, len)) {
    return g_real_connect(fd, sa, len);
  }
  if (g_proxy_port == 0 && !read_proxy_port_once()) {
    // No proxy means no outbound: reproduce ECONNREFUSED so callers see a
    // deterministic failure rather than hanging.
    errno = ENETUNREACH;
    return -1;
  }

  uint8_t handshake[kProxyHandshakeSize];
  if (!build_handshake(sa, len, handshake)) {
    errno = EAFNOSUPPORT;
    return -1;
  }

  // Honour caller-imposed O_NONBLOCK by temporarily clearing it; the proxy
  // handshake assumes a blocking-style flow.  We restore the flag before
  // returning so the app's downstream usage is unaffected.
  int orig_flags = fcntl(fd, F_GETFL);
  bool was_nonblock = (orig_flags >= 0) && (orig_flags & O_NONBLOCK);
  if (was_nonblock) {
    fcntl(fd, F_SETFL, orig_flags & ~O_NONBLOCK);
  }

  // Connect to the in-netns proxy listener using the original caller fd.
  sockaddr_in proxy_addr {};
  proxy_addr.sin_family = AF_INET;
  proxy_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  proxy_addr.sin_port = htons(g_proxy_port);
  int rc = g_real_connect(fd, reinterpret_cast<sockaddr *>(&proxy_addr),
                          sizeof(proxy_addr));
  if (rc != 0) {
    int saved = errno;
    if (was_nonblock) fcntl(fd, F_SETFL, orig_flags);
    errno = saved;
    return -1;
  }

  if (!write_full(fd, handshake, sizeof(handshake))) {
    int saved = errno;
    if (was_nonblock) fcntl(fd, F_SETFL, orig_flags);
    errno = saved ? saved : EIO;
    return -1;
  }

  uint8_t status = 0;
  if (!read_full(fd, &status, sizeof(status))) {
    if (was_nonblock) fcntl(fd, F_SETFL, orig_flags);
    errno = EIO;
    return -1;
  }
  if (status != kProxyStatusOk) {
    uint32_t err_be = 0;
    if (read_full(fd, &err_be, sizeof(err_be))) {
      errno = static_cast<int>(ntohl(err_be));
      if (errno <= 0) errno = ECONNREFUSED;
    } else {
      errno = ECONNREFUSED;
    }
    if (was_nonblock) fcntl(fd, F_SETFL, orig_flags);
    return -1;
  }

  if (was_nonblock) fcntl(fd, F_SETFL, orig_flags);
  return 0;
}

// ---------------------------------------------------------------------------
// PLT hook installation: hook libc's connect() across every loaded ELF that
// allows it.  We register and commit each ELF in isolation so that a single
// unhookable mapping does not roll back the whole batch.
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
    char perms[8] = {};
    unsigned long offset = 0;
    unsigned int dev_major = 0, dev_minor = 0;
    unsigned long inode_val = 0;
    int path_off = 0;
    int matched = sscanf(line, "%*lx-%*lx %7s %lx %x:%x %lu %n",
                         perms, &offset, &dev_major, &dev_minor, &inode_val,
                         &path_off);
    if (matched < 5) continue;
    if (perms[2] != 'x') continue;
    if (inode_val == 0) continue;
    const char *path = line + path_off;
    while (*path == ' ' || *path == '\t') ++path;
    if (*path == '\0' || *path == '\n') continue;
    if (*path == '[') continue;

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

bool path_should_be_hooked(const std::string &path) {
  // Limit ourselves to shared libraries so we don't try to rewrite the GOT of
  // executables (app_process etc.).
  if (path.find(".so") == std::string::npos) return false;
  // Skip our own library; we don't have a connect PLT entry (or if we did,
  // hooking yourself can produce surprising recursion).
  if (path.find("scene_netns_zygisk") != std::string::npos) return false;
  return true;
}

size_t install_connect_hooks(Api *api) {
  if (!api) return 0;

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
    return 0;
  }

  auto mappings = collect_executable_mappings();
  size_t committed = 0;
  size_t attempted = 0;
  for (const auto &m : mappings) {
    if (!path_should_be_hooked(m.path)) continue;
    ++attempted;
    void *prev = nullptr;
    api->pltHookRegister(m.dev, m.inode, "connect",
                         reinterpret_cast<void *>(&my_connect), &prev);
    // Commit per-ELF: a failure on one library no longer rolls back others.
    if (api->pltHookCommit()) {
      ++committed;
    } else {
      // pltHookCommit failure is silent in libc; log the path to aid debugging
      // but keep going.
      loge("[hook] commit failed for %s", m.path.c_str());
    }
  }
  logi("[hook] connect() PLT hook committed for %zu of %zu ELFs",
       committed, attempted);
  return committed;
}

// ---------------------------------------------------------------------------
// Pinner endpoint discovery (used by the companion for fetching the netns).
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

int recv_fd_msg(int sock) {
  char data = 0;
  iovec iov = {};
  iov.iov_base = &data;
  iov.iov_len = sizeof(data);
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) < 0) {
    return -1;
  }
  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    return -1;
  }
  int fd = -1;
  memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
  return fd;
}

void send_fd_msg(int sock, int fd) {
  char data = 'N';
  iovec iov = {};
  iov.iov_base = &data;
  iov.iov_len = sizeof(data);
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  msg.msg_controllen = CMSG_SPACE(sizeof(int));
  sendmsg(sock, &msg, MSG_NOSIGNAL);
}

int request_netns_fd_from_pinner() {
  char sock_path[kUnixPathMax] = {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path)) ||
      !private_socket_exists(sock_path)) {
    return -1;
  }
  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) return -1;
  sockaddr_un addr = {};
  fill_sockaddr(&addr, sock_path);
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    loge("[companion] connect pinner failed: %s", strerror(errno));
    close(sock);
    return -1;
  }
  int ns_fd = recv_fd_msg(sock);
  close(sock);
  return ns_fd;
}

// Companion is intentionally minimal: its sole job is to fetch the pinned
// netns fd (which only root can open) and hand it to the caller.  The
// long-lived proxy work happens entirely inside the pinner.
void companion_handler(int client) {
  int ns_fd = request_netns_fd_from_pinner();
  if (ns_fd < 0) {
    close(client);
    return;
  }
  send_fd_msg(client, ns_fd);
  close(ns_fd);
  close(client);
}

bool fetch_netns_fd(Api *api, int *out_ns_fd) {
  *out_ns_fd = -1;
  int sock = api->connectCompanion();
  if (sock < 0) {
    loge("[zygisk] connect companion failed");
    return false;
  }
  int ns_fd = recv_fd_msg(sock);
  close(sock);
  if (ns_fd < 0) {
    loge("[zygisk] companion did not return a netns fd");
    return false;
  }
  *out_ns_fd = ns_fd;
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
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }

    int ns_fd = -1;
    if (!fetch_netns_fd(api, &ns_fd)) {
      loge("[zygisk] failed to acquire netns: %s", nice_name);
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }

    if (sys_setns(ns_fd, CLONE_NEWNET) != 0) {
      loge("[zygisk] setns failed: %s", strerror(errno));
      close(ns_fd);
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
      return;
    }
    close(ns_fd);

    // Cache the proxy port now that we are inside the isolated netns.  This
    // is just a regular file read (no SELinux issue), but doing it once here
    // avoids per-connect file IO on the hot path.
    if (!read_proxy_port_once()) {
      loge("[zygisk] proxy port unknown; outbound traffic will fail");
      // Continue: install hooks anyway so my_connect at least returns a
      // deterministic ENETUNREACH instead of hanging.
    }

    size_t hooked = install_connect_hooks(api);
    prepend_path_for_su_wrapper();

    if (hooked == 0) {
      loge("[zygisk] no PLT hooks installed; outbound traffic will fail");
    }
    // Important: do NOT request DLCLOSE_MODULE_LIBRARY here.  Our my_connect
    // and read_proxy_port_once must remain mapped for the lifetime of the
    // process now that PLT entries point into us.

    logi("[zygisk] Scene process isolated: nice_name=%s app_data_dir=%s hooked=%zu",
         nice_name, app_data_dir, hooked);
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
