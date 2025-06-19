cmake_minimum_required(VERSION 3.10)
project(rk3566_receiver)

# 设置C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置WebRTC路径（需要根据实际环境修改）
set(WEBRTC_SRC_DIR "/path/to/webrtc/src")
set(WEBRTC_BUILD_DIR "${WEBRTC_SRC_DIR}/out/Default")

# 设置交叉编译工具链（需要根据实际环境修改）
set(CMAKE_SYSROOT "/path/to/rk3566/sysroot")
set(CMAKE_C_COMPILER "/path/to/rk3566/toolchain/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "/path/to/rk3566/toolchain/bin/aarch64-linux-gnu-g++")

# 添加WebRTC头文件路径
include_directories(
    ${WEBRTC_SRC_DIR}
    ${WEBRTC_SRC_DIR}/third_party/abseil-cpp
    ${WEBRTC_SRC_DIR}/third_party/jsoncpp/source/include
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/rockit
)

# 添加WebRTC库路径
link_directories(
    ${WEBRTC_BUILD_DIR}/obj
    ${CMAKE_SYSROOT}/usr/lib
)

# 查找依赖库
find_package(LibWebSockets REQUIRED)
find_package(jsoncpp REQUIRED)

# 添加源文件
add_executable(rk3566_receiver
    main.cc
    webrtc/webrtc_client.cc
    webrtc/peer_connection_observer_impl.cc
    webrtc/encoded_video_frame_handler_rockit.cc
    webrtc/audio_receiver_rockit.cc
    signaling/signaling_client_ws.cc
)

# 链接库
target_link_libraries(rk3566_receiver
    ${WEBRTC_BUILD_DIR}/obj/libwebrtc.a
    ${LIBWEBSOCKETS_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    rockit
    dl
    pthread
    rt
)

# 安装目标
install(TARGETS rk3566_receiver DESTINATION bin)
