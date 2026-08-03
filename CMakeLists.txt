# cmake_minimum_required(VERSION 3.22.1)
# project("echocancelwebrtc_android" CXX)

# # 1. C++20 표준 강제 설정
# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF)

# # 2. WebRTC 매크로 설정
# add_compile_definitions(WEBRTC_POSIX WEBRTC_ANDROID WEBRTC_LINUX)

# # 3. 헤더 파일 경로 지정
# # audio_processing.h가 transitively 참조하는 헤더(api/scoped_refptr.h, api/task_queue/*,
# # absl/* 등)가 include/ 에는 일부만 존재하므로, WSL2 쪽 WebRTC 전체 소스 트리를 직접 참조한다.
# # include/ 폴더의 파일들은 이 트리에서 발췌한 것과 동일하여 더 이상 include 경로에 넣지 않는다.

# # 1. WSL2 내부의 WebRTC 최상위 소스 경로 (실제 본인의 WebRTC src 경로로 수정)
# set(WEBRTC_SRC_DIR "//wsl.localhost/Ubuntu/home/elricjkim/webrtc-android/src")

# # 2. 헤더 복사 없이 리눅스 로컬 경로 직접 참조
# include_directories(
#     ${WEBRTC_SRC_DIR}
#     ${WEBRTC_SRC_DIR}/third_party/abseil-cpp
# )

# set(NDK_ROOT "C:/Users/elric.j.kim/AppData/Local/Android/Sdk/ndk/28.2.13676358")

# include_directories(
#     ${NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include
#     ${NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include/android
# )


# # 4. .so 동적 라이브러리 타깃 정의
# add_library(
#     my_native_lib
#     SHARED
#     src/native-webrtc.cpp
# )

# # 5. Android 시스템 라이브러리 (Logcat) 연결
# find_library(log-lib log)

# # 6. 미리 빌드된 libaudio_processing.so 연결
# # 주의: 이 .so는 심볼을 export하지 않은 상태로 빌드되어 있어(RTC_EXPORT가
# # WEBRTC_ENABLE_SYMBOL_EXPORT 없이는 빈 매크로), 링크는 되지만 AudioProcessing의
# # 가상 함수 호출 등 실제 심볼이 필요한 지점에서 undefined reference가 발생할 수 있다.
# add_library(audio_processing SHARED IMPORTED)
# set_target_properties(
#     audio_processing PROPERTIES
#     IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/libs/libaudio_processing.so
# )

# target_link_libraries(
#     my_native_lib
#     ${log-lib}
#     audio_processing
# )


cmake_minimum_required(VERSION 3.22.1)

project("echocancelwebrtc_android" CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_VERBOSE_MAKEFILE true)

add_library(apm_lib SHARED
    src/apm_lib.cpp
    src/apm_lib.h
)

target_include_directories(apm_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(webrtc_apm STATIC IMPORTED)
set_target_properties(webrtc_apm PROPERTIES
    IMPORTED_LOCATION
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libwebrtc_apm.a"
)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)
find_library(z-lib z)

target_link_libraries(apm_lib PRIVATE
    webrtc_apm
    c++_shared
    ${log-lib}
    ${android-lib}
    ${dl-lib}
    ${z-lib}
)

target_link_options(apm_lib PRIVATE
    "-Wl,-z,max-page-size=16384"
)