# =============================================================================
# echocancelwebrtc_android — WebRTC APM(AEC3/NS/AGC2) JNI 래퍼 빌드
#
#   src/apm_lib.cpp ─┐
#   include/ (헤더) ─┼─► libapm_lib.so ◄── libs/${ANDROID_ABI}/libwebrtc_apm.a (정적 링크)
#                    └─ NDK: c++_shared, log, android, dl, z
#
# 빌드: cmake --preset arm64-v8a && cmake --build --preset arm64-v8a
#       (툴체인/ABI/API 레벨 설정은 CMakePresets.json 참고)
# =============================================================================

cmake_minimum_required(VERSION 3.22.1)

project("echocancelwebrtc_android" CXX)

# WebRTC 헤더가 C++20 기능(std::span 등)을 사용하므로 C++20 필수.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 컴파일/링크 명령 전체를 출력 (빌드 디버깅용).
set(CMAKE_VERBOSE_MAKEFILE true)

# --- JNI 공유 라이브러리 (Kotlin에서 System.loadLibrary("apm_lib")로 로드) ---
add_library(apm_lib SHARED
    src/apm_lib.cpp
    src/apm_lib.h
)

# WebRTC / abseil 헤더 경로 (api/..., modules/..., absl/...).
target_include_directories(apm_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --- 사전 빌드된 WebRTC APM 정적 라이브러리 (ABI별로 libs/<ABI>/ 에 배치) ---
add_library(webrtc_apm STATIC IMPORTED)
set_target_properties(webrtc_apm PROPERTIES
    IMPORTED_LOCATION
    #"${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libwebrtc_apm.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libwebrtc_apm.a"
)

# --- NDK 시스템 라이브러리 ---
find_library(log-lib log)          # __android_log_print (logcat)
find_library(android-lib android)
find_library(dl-lib dl)
find_library(z-lib z)

target_link_libraries(apm_lib PRIVATE
    webrtc_apm
    c++_shared                      # ANDROID_STL=c++_shared 와 일치해야 함
    ${log-lib}
    ${android-lib}
    ${dl-lib}
    ${z-lib}
)

# 16KB 페이지 크기 기기(Android 15+) 호환을 위한 ELF 세그먼트 정렬.
target_link_options(apm_lib PRIVATE
    "-Wl,-z,max-page-size=16384"
)
