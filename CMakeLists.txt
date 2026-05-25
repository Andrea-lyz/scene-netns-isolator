cmake_minimum_required(VERSION 3.22)
project(scene_netns_isolator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(scene-netnsctl src/native/scene_netnsctl.cpp)
target_compile_options(scene-netnsctl PRIVATE -Wall -Wextra -Werror)

add_executable(scene-su-wrapper src/native/su_wrapper.cpp)
target_compile_options(scene-su-wrapper PRIVATE -Wall -Wextra -Werror)

if(NOT DEFINED ZYGISK_API_INCLUDE)
  set(ZYGISK_API_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zygisk")
endif()

add_library(scene_netns_zygisk SHARED src/zygisk/scene_netns_zygisk.cpp)
target_include_directories(scene_netns_zygisk PRIVATE ${ZYGISK_API_INCLUDE})
target_compile_options(scene_netns_zygisk PRIVATE
  -Wall -Wextra -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit)
target_link_libraries(scene_netns_zygisk log)
target_link_options(scene_netns_zygisk PRIVATE -nostdlib++)
