#pragma once

// =============================================================================
// apm_lib — WebRTC APM(AEC3 / NS / AGC2) Android JNI 래퍼 (libapm_lib.so)
// =============================================================================
//
// [프로젝트 구조]
//
//  echocancelwebrtc_android/
//  ├─ CMakeLists.txt       apm_lib(SHARED) 빌드 정의, libwebrtc_apm.a 정적 링크
//  ├─ CMakePresets.json    NDK 툴체인 프리셋 (arm64-v8a / armeabi-v7a / x86_64)
//  ├─ src/
//  │   ├─ apm_lib.h        JNI 함수 선언 + WebRTC 헤더 include (이 파일)
//  │   └─ apm_lib.cpp      AecHandle 정의 및 JNI 함수 구현
//  ├─ include/             WebRTC / abseil 헤더 (api/, modules/, rtc_base/, absl/ ...)
//  ├─ libs/<ABI>/          사전 빌드된 WebRTC APM 정적 라이브러리 (libwebrtc_apm.a)
//  └─ build/<ABI>/         CMake + Ninja 빌드 출력 → libapm_lib.so
//
// [모듈 의존 관계]
//
//  Kotlin  com.selvas.echocancelsample.models.LocalEchoCanceller
//     │  JNI (native* 함수)
//     ▼
//  libapm_lib.so  (src/apm_lib.cpp)
//     │  정적 링크
//     ▼
//  libwebrtc_apm.a  ─ AudioProcessing(AEC3, NS, AGC2), EchoDetector,
//                     DominantNearendDetector, Aec3Fft
//     │
//     ▼
//  NDK 시스템 라이브러리: libc++_shared, liblog, libandroid, libdl, libz
//
// [오디오 처리 흐름]  10 ms PCM16 프레임 단위, DirectByteBuffer 제자리(in-place) 처리
//
//  스피커 재생 PCM ─► nativeProcessRender ─► apm->ProcessReverseStream (far-end 참조 신호)
//                                                   │ AEC3 내부 render buffer에 적재
//                                                   ▼
//  마이크 녹음 PCM ─► nativeProcessCapture ─┬─► apm->ProcessStream ─────► 처리된 PCM 반환
//                    (stream_delay_ms)      │    (AEC3 → NS → AGC2)    │
//                                           │ pre(처리 전)             │ post(처리 후)
//                                           ▼                          ▼
//                       [선택] nearend 미러 detector (nativeSetNearEndDetectorConfig 이후)
//                         64샘플 블록 FFT: near = post, echo = pre - post
//                         → EMA 평활화 → DominantNearendDetector → nearend_state
//                         → nativeIsNearEndState 로 조회
//
//  apm->GetStatistics() ─► nativeGetLikelihoodFiltered (1 - residual_echo_likelihood)
//
// [핸들 수명 / 스레드 모델]
//
//  nativeCreate ─► jlong(AecHandle*) ─► native* 호출들 ─► nativeDestroy
//   - AecHandle::mutex          : apm 접근 보호 (render 스레드 ↔ capture 스레드)
//   - AecHandle::nearend_mutex  : nearend detector 상태 보호 (apm critical section과 분리)
//
// =============================================================================

//class apm_lib
//{
//public:
//	typedef void* APM_HANDLE;
//
//
//public:
//	const char * getPlatformABI();
//	apm_lib();
//	~apm_lib();
//	// APM 인스턴스 생성 (sample_rate: 16000, 32000, 48000 등, channels: 1 또는 2)
//	APM_HANDLE Apm_Create(int sample_rate_hz, int channels);
//
//	// 마이크(Near-end) 오디오 처리 (10ms float 샘플 버퍼)
//	int Apm_ProcessStream(APM_HANDLE handle, const float* in_frame, float* out_frame);
//
//	// 스피커 출력/참조(Far-end) 오디오 입력 (AEC 에코 제거용)
//	int Apm_ProcessReverseStream(APM_HANDLE handle, const float* far_frame);
//
//	// APM 인스턴스 파괴 및 메모리 해제
//	void Apm_Destroy(APM_HANDLE handle);
//
//};

#include <jni.h>
#include <errno.h>

#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#include <android/log.h>

#include <cstdlib>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <numeric>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_detector_creator.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"
#include "modules/audio_processing/aec3/aec3_fft.h"
#include "modules/audio_processing/aec3/dominant_nearend_detector.h"
#include "modules/audio_processing/aec3/fft_data.h"

// JNI 진입점. Kotlin 클래스 com.selvas.echocancelsample.models.LocalEchoCanceller의
// native 메서드와 1:1로 대응하므로, 패키지/클래스명 변경 시 함수명도 함께 바꿔야 한다.
extern "C" {
	// APM 인스턴스 생성 후 AecHandle 포인터를 jlong 핸들로 반환한다.
	// sample_rate_hz: 8000/16000/32000/48000, channels: 1 또는 2.
	// isNSON: 노이즈 억제(NS, kVeryHigh) 사용 여부, isAGCOn: AGC2(adaptive digital) 사용 여부.
	// 실패 시 IllegalArgumentException을 던지고 0을 반환한다.
	// JNIEXPORT jlong JNICALL
	// 	Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeCreate(
	// 		JNIEnv* env, jclass, jint sample_rate_hz, jint channels, jboolean isNSON, jboolean isAGCOn);
	JNIEXPORT jlong JNICALL
		Java_selvas_webrtc_apm_EchoCancel_CreateAPMHandle(
			JNIEnv* env, jclass, jint sample_rate_hz, jint channels, jboolean isNSON, jboolean isAGCOn);

	// 스피커 재생(far-end) PCM16을 AEC3 참조 신호로 전달한다.
	// byte_count는 10ms 프레임 크기의 정수배여야 하며, webrtc 에러 코드(kNoError=0)를 반환한다.
	JNIEXPORT jint JNICALL
		//Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeProcessRender(
		Java_selvas_webrtc_apm_EchoCancel_ProcessRender(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count);

	// 마이크 녹음(near-end) PCM16을 AEC/NS/AGC 처리한다.
	// stream_delay_ms: 스피커 출력 → 마이크 입력 간 추정 지연(ms), AEC3 지연 추정의 초기값.
	// 성공 시 처리된 pcm16(제자리 처리된 동일 버퍼)을 그대로 반환하고, 실패 시
	// RuntimeException/IllegalArgumentException을 던지고 null을 반환한다.
	JNIEXPORT jobject JNICALL
		Java_selvas_webrtc_apm_EchoCancel_ProcessCapture(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count,
			jint stream_delay_ms);

	// nativeCreate로 생성한 핸들을 해제한다. 다른 native* 호출이 진행 중이 아닐 때 호출해야 한다.
	JNIEXPORT void JNICALL
		Java_selvas_webrtc_apm_EchoCancel_DestroyAPMHandle(
			JNIEnv*, jclass, jlong native_handle);

	// PCM16 버퍼의 RMS 레벨을 dBFS(-96.0 ~ 0.0)로 반환한다. 핸들과 무관한 유틸리티 함수.
	JNIEXPORT jdouble JNICALL
		Java_selvas_webrtc_apm_EchoCancel_CalculateRmsDb(
			JNIEnv* env, jclass, jobject pcm16, jint byte_count);

	// dominant nearend 상태 조회용 detector의 threshold 설정 (실제 AEC3 config에는 미반영).
	JNIEXPORT void JNICALL
		Java_selvas_webrtc_apm_EchoCancel_SetNearEndDetectorConfig(
			JNIEnv* env, jclass, jlong native_handle, jfloat enrThreshold, jfloat snrThreshold);

	// 가장 최근 캡처 프레임 처리 시점 기준 dominant nearend 상태 여부.
	JNIEXPORT jboolean JNICALL
		Java_selvas_webrtc_apm_EchoCancel_IsNearEndState(
			JNIEnv* env, jclass, jlong native_handle);

	// APM 통계의 (1 - residual_echo_likelihood)를 반환한다. 값이 없으면 0.
	// 디버깅용으로 delay_ms / ERLE / ERL / divergent_filter_fraction을 logcat에 출력한다.
	JNIEXPORT jdouble JNICALL
		Java_selvas_webrtc_apm_EchoCancel_GetLikelihoodFiltered(
			JNIEnv* env, jclass, jlong native_handle);
}
