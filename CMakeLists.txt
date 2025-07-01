# CMake最低版本要求和项目定义
cmake_minimum_required(VERSION 3.16)
project(rk3566_receiver CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. 定义所有依赖的根路径变量 (方便管理) ---
set(ROCKCHIP_SYSROOT /home/zw/rk356x/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot)
set(OFFICIAL_SDK_SRC_PATH /home/zw/rk356x) # 官方SDK源码的根目录
set(MY_CROSS_LIBS_PATH "/home/zw/rk3566_sdk/cross_libs") # 【修正一】使用变量替代'~'
set(WEBRTC_SRC_PATH "/home/zw/webrtc_rk3566/webrtc-checkout/src")
set(WEBRTC_LIB_PATH "${WEBRTC_SRC_PATH}/out/rk3566/obj")

# --- 2. 定义可执行文件 ---
add_executable(rk3566_receiver
    main.cc
    signaling/signaling_client_ws.cc
    webrtc/webrtc_client.cc
    webrtc/peer_connection_observer_impl.cc
    webrtc/audio_receiver_rockit.cc
    webrtc/encoded_video_frame_handler_rockit.cc
)

# --- 3. 为目标(target)精确配置头文件搜索路径 ---
# 【核心修正】我们现在把所有可能包含所需头文件的路径都明确地加进去
target_include_directories(rk3566_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    
    # Sysroot里的路径
    ${ROCKCHIP_SYSROOT}/usr/include
    #${ROCKCHIP_SYSROOT}/usr/include/rockchip

    # 官方SDK源码里的路径 (以防Sysroot不全)
    #${OFFICIAL_SDK_SRC_PATH}/external/mpp/inc
    ${OFFICIAL_SDK_SRC_PATH}/external/rockit/mpi/sdk/include
    ${OFFICIAL_SDK_SRC_PATH}/external/rockit/mpi/sdk/lib/lib64  # 【修正二】为 rk_defines.h 添加此路径

    # 第三方库的路径
    ${MY_CROSS_LIBS_PATH}/include

    # WebRTC的路径
    ${WEBRTC_SRC_PATH}
    ${WEBRTC_SRC_PATH}/third_party/abseil-cpp
)

# --- 4. 为目标(target)添加编译宏定义 ---
target_compile_definitions(rk3566_receiver PRIVATE
    WEBRTC_POSIX WEBRTC_LINUX WEBRTC_ARCH_AARCH64 WEBRTC_ARCH_ARM64 WEBRTC_HAVE_SCTP
)

# --- 5. 为目标(target)链接所有库 ---
target_link_libraries(rk3566_receiver PRIVATE
    # Rockchip 相关的库
    # ${ROCKCHIP_SYSROOT}/usr/lib/librockchip_mpp.so
    # 您的项目暂时没用到RGA，所以注释掉
    # ${ROCKCHIP_SYSROOT}/usr/lib/librga.so
    ${OFFICIAL_SDK_SRC_PATH}/external/rockit/mpi/sdk/lib/lib64/librockit.so 

    # 第三方库
    ${MY_CROSS_LIBS_PATH}/lib/libwebsockets.so
    ${MY_CROSS_LIBS_PATH}/lib/libjsoncpp.so
    ${MY_CROSS_LIBS_PATH}/lib/libssl.so
    ${MY_CROSS_LIBS_PATH}/lib/libcrypto.so

    # WebRTC 静态库
    ${WEBRTC_LIB_PATH}/libwebrtc.a

    # 系统库
    pthread dl rt
    atomic
    m
    stdc++
)