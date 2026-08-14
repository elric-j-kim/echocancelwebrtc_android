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
#include <mutex>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"

extern "C" {
	JNIEXPORT jlong JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeCreate(
			JNIEnv* env, jclass, jint sample_rate_hz, jint channels);

	JNIEXPORT jint JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeProcessRender(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count);

	JNIEXPORT jint JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeProcessCapture(
			JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count,
			jint stream_delay_ms);

	JNIEXPORT void JNICALL
		Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeDestroy(
			JNIEnv*, jclass, jlong native_handle);
}
