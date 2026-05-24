#include <android/log.h>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <zygisk.hpp>

#include "../common/runtime_paths.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

using scene_netns::kEndpointPath;
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

bool copy_jstring(JNIEnv *env, jstring s, char *out, size_t out_size) {
  if (out_size == 0) {
    return false;
  }
  out[0] = '\0';
  if (!env || !s) {
    return false;
  }

  const char *chars = env->GetStringUTFChars(s, nullptr);
  if (!chars) {
    return false;
  }
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

  if (strcmp(nice_name, kPackage) == 0 || starts_with(nice_name, "com.omarea.vtools:")) {
    return true;
  }
  return strstr(app_data_dir, "/com.omarea.vtools") != nullptr;
}

int sys_setns(int fd, int nstype) {
  return static_cast<int>(syscall(__NR_setns, fd, nstype));
}

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
  if (!out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  int fd = open(kEndpointPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    loge("[zygisk] open pinner endpoint failed: %s", strerror(errno));
    return false;
  }

  struct stat st {};
  if (fstat(fd, &st) != 0) {
    loge("[zygisk] stat pinner endpoint failed: %s", strerror(errno));
    close(fd);
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    loge("[zygisk] pinner endpoint is not private");
    close(fd);
    return false;
  }

  ssize_t n = read(fd, out, out_size - 1);
  if (n < 0) {
    loge("[zygisk] read pinner endpoint failed: %s", strerror(errno));
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
    loge("[zygisk] pinner endpoint path is invalid");
    return false;
  }
  return true;
}

bool private_socket_exists(const char *path) {
  struct stat st {};
  if (lstat(path, &st) != 0) {
    loge("[zygisk] stat pinner socket failed: %s", strerror(errno));
    return false;
  }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
    loge("[zygisk] pinner socket is not private");
    return false;
  }
  return true;
}

void fill_sockaddr(sockaddr_un *addr, const char *path) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
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
    loge("[zygisk] recv netns fd failed: %s", strerror(errno));
    return -1;
  }

  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    loge("[zygisk] pinner did not send a namespace fd");
    return -1;
  }

  int fd = -1;
  memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
  if (fd < 0) {
    loge("[zygisk] received invalid namespace fd");
  }
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
  memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  msg.msg_controllen = CMSG_SPACE(sizeof(int));

  if (sendmsg(sock, &msg, MSG_NOSIGNAL) < 0) {
    loge("[companion] send netns fd failed: %s", strerror(errno));
  }
}

int request_netns_fd() {
  char sock_path[kUnixPathMax] {};
  if (!read_endpoint_path(sock_path, sizeof(sock_path)) ||
      !private_socket_exists(sock_path)) {
    return -1;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    loge("[zygisk] socket failed: %s", strerror(errno));
    return -1;
  }

  sockaddr_un addr {};
  fill_sockaddr(&addr, sock_path);
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    loge("[zygisk] connect pinner socket failed: %s", strerror(errno));
    close(sock);
    return -1;
  }

  int fd = recv_fd(sock);
  close(sock);
  return fd;
}

int request_netns_fd_from_companion(Api *api) {
  if (!api) {
    loge("[zygisk] api is unavailable");
    return -1;
  }

  int sock = api->connectCompanion();
  if (sock < 0) {
    loge("[zygisk] connect companion failed");
    return -1;
  }

  int fd = recv_fd(sock);
  close(sock);
  return fd;
}

bool enter_scene_netns(Api *api) {
  int fd = request_netns_fd_from_companion(api);
  if (fd < 0) {
    return false;
  }
  if (sys_setns(fd, CLONE_NEWNET) != 0) {
    loge("[zygisk] setns failed: %s", strerror(errno));
    close(fd);
    return false;
  }
  close(fd);
  logi("[zygisk] entered Scene netns");
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

void companion_handler(int client) {
  int fd = request_netns_fd();
  if (fd >= 0) {
    send_fd(client, fd);
    close(fd);
  } else {
    loge("[companion] failed to get netns fd from pinner");
  }
  close(client);
}

class SceneNetnsModule : public zygisk::ModuleBase {
 public:
  void onLoad(Api *api, JNIEnv *env) override {
    this->api = api;
    this->env = env;
  }

  void preAppSpecialize(AppSpecializeArgs *args) override {
    char nice_name[256] {};
    char app_data_dir[512] {};

    if (!is_scene_process(env, args->nice_name, args->app_data_dir,
                          nice_name, sizeof(nice_name),
                          app_data_dir, sizeof(app_data_dir))) {
      request_module_unload();
      return;
    }

    if (!enter_scene_netns(api)) {
      request_module_unload();
      loge("[zygisk] Scene process matched but netns enter failed: nice_name=%s app_data_dir=%s",
           nice_name, app_data_dir);
      return;
    }

    prepend_path_for_su_wrapper();
    request_module_unload();

    logi("[zygisk] Scene process isolated: nice_name=%s app_data_dir=%s",
         nice_name, app_data_dir);
  }

  void postAppSpecialize(const AppSpecializeArgs *) override {}

  void preServerSpecialize(ServerSpecializeArgs *) override {
    request_module_unload();
  }

 private:
  void request_module_unload() {
    if (api) {
      api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
  }

  Api *api = nullptr;
  JNIEnv *env = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SceneNetnsModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
