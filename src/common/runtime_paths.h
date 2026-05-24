#pragma once

#include <cstddef>
#include <sys/un.h>

namespace scene_netns {

constexpr const char *kRunDir = "/dev/.15f1c4b9";
constexpr const char *kEndpointPath = "/dev/.15f1c4b9/.endpoint";
constexpr const char *kPidPath = "/dev/.15f1c4b9/.pid";
constexpr std::size_t kUnixPathMax = sizeof(((sockaddr_un *)0)->sun_path);

}  // namespace scene_netns
