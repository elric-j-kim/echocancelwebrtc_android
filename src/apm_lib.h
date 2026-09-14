#pragma once

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

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"
#include "modules/audio_processing/aec3/aec3_fft.h"
#include "modules/audio_processing/aec3/dominant_nearend_detector.h"
#include "modules/audio_processing/aec3/fft_data.h"

extern "C" {
	JNIEXPORT jlong JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeCreate(
			JNIEnv* env, jclass, jint sample_rate_hz, jint channels, jboolean isNSON, jboolean isAGCOn);

	JNIEXPORT jint JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeProcessRender(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count);

	// 성공 시 처리된 pcm16(제자리 처리된 동일 버퍼)을 그대로 반환하고, 실패 시
	// RuntimeException/IllegalArgumentException을 던지고 null을 반환한다.
	JNIEXPORT jobject JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeProcessCapture(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count,
			jint stream_delay_ms);

	JNIEXPORT void JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeDestroy(
			JNIEnv*, jclass, jlong native_handle);

	JNIEXPORT jdouble JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeCalculateRmsDb(
			JNIEnv* env, jclass, jobject pcm16, jint byte_count);

	// dominant nearend 상태 조회용 detector의 threshold 설정 (실제 AEC3 config에는 미반영).
	JNIEXPORT void JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeSetNearEndDetectorConfig(
			JNIEnv* env, jclass, jlong native_handle, jfloat enrThreshold, jfloat snrThreshold);

	// 가장 최근 캡처 프레임 처리 시점 기준 dominant nearend 상태 여부.
	JNIEXPORT jboolean JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeIsNearEndState(
			JNIEnv* env, jclass, jlong native_handle);
}
