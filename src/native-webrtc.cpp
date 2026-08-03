// #include <jni.h>
// #include <android/log.h>
// #include <cstdint>
// #include <utility>

// #include "api/audio/audio_processing.h"              // C++20 기반 WebRTC 헤더
// #include "api/audio/builtin_audio_processing_builder.h"
// #include "api/environment/environment_factory.h"

// #define LOG_TAG "MyJniLib"
// #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
// #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// // webrtc 네임스페이스 및 AudioProcessing C++20 구문 사용 테스트
// using webrtc::AudioProcessing;
// using webrtc::BuiltinAudioProcessingBuilder;
// using webrtc::CreateEnvironment;
// using webrtc::scoped_refptr;
// using webrtc::StreamConfig;

// namespace {

// // jlong 핸들 하나에 AudioProcessing 인스턴스를 실어 나르기 위한 래퍼.
// struct ApmHandle {
//   scoped_refptr<AudioProcessing> apm;
// };

// ApmHandle* ToHandle(jlong handle) {
//   return reinterpret_cast<ApmHandle*>(handle);
// }

// }  // namespace

// extern "C" JNIEXPORT jstring JNICALL
// Java_com_example_myapp_MainActivity_stringFromJNI(
//     JNIEnv* env,
//     jobject /* this */) {

//     // C++20 std::span 등 규격이 포함된 AudioProcessing FrameSize 함수 확인
//     int frame_size_48k = AudioProcessing::GetFrameSize(48000);
//     LOGI("WebRTC Audio Processing Frame Size (48kHz): %d", frame_size_48k);

//     return env->NewStringUTF("Hello from Android C++20 JNI (.so)!");
// }

// // 에코 캔슬러(AEC) + 하이패스 필터 + 노이즈 억제를 활성화한 AudioProcessing 인스턴스를
// // 생성하고, 그 핸들을 jlong으로 반환한다. 실패 시 0을 반환한다.
// extern "C" JNIEXPORT jlong JNICALL
// Java_com_example_myapp_NativeAudioProcessor_nativeCreate(
//     JNIEnv* env,
//     jobject /* this */) {

//   AudioProcessing::Config config;
//   config.echo_canceller.enabled = true;
//   config.high_pass_filter.enabled = true;
//   config.noise_suppression.enabled = true;
//   config.noise_suppression.level =
//       AudioProcessing::Config::NoiseSuppression::kModerate;

//   scoped_refptr<AudioProcessing> apm =
//       BuiltinAudioProcessingBuilder(config).Build(CreateEnvironment());
//   if (!apm) {
//     LOGE("Failed to build AudioProcessing instance");
//     return 0;
//   }

//   auto* apm_handle = new ApmHandle{std::move(apm)};
//   return reinterpret_cast<jlong>(apm_handle);
// }

// // nativeCreate()로 만든 인스턴스를 해제한다.
// extern "C" JNIEXPORT void JNICALL
// Java_com_example_myapp_NativeAudioProcessor_nativeDestroy(
//     JNIEnv* env,
//     jobject /* this */,
//     jlong handle) {

//   delete ToHandle(handle);
// }

// // 원단(render/far-end) 프레임을 반향 제거기에 등록한다. far_end는 sample_rate_hz/100
// // 샘플 x num_channels 채널 분량(약 10ms)의 인터리브드 PCM16 데이터여야 하며,
// // 같은 프레임에 대해 nativeProcessStream()보다 먼저 호출되어야 한다.
// extern "C" JNIEXPORT jint JNICALL
// Java_com_example_myapp_NativeAudioProcessor_nativeProcessReverseStream(
//     JNIEnv* env,
//     jobject /* this */,
//     jlong handle,
//     jshortArray far_end,
//     jint sample_rate_hz,
//     jint num_channels) {

//   ApmHandle* apm_handle = ToHandle(handle);
//   if (apm_handle == nullptr) {
//     return AudioProcessing::kNullPointerError;
//   }

//   jshort* data = env->GetShortArrayElements(far_end, nullptr);
//   const StreamConfig stream_config(sample_rate_hz, num_channels);
//   int result = apm_handle->apm->ProcessReverseStream(
//       reinterpret_cast<int16_t*>(data), stream_config, stream_config,
//       reinterpret_cast<int16_t*>(data));
//   env->ReleaseShortArrayElements(far_end, data, 0);
//   return result;
// }

// // 근단(capture/near-end) 프레임에 에코 제거/노이즈 억제 등을 적용한다. stream_delay_ms는
// // far-end 프레임이 스피커로 나간 시점과 near-end 프레임이 마이크로 들어온 시점 사이의
// // 지연이며, ProcessStream() 이전에 반영된다. near_end 배열은 in-place로 갱신된다.
// extern "C" JNIEXPORT jint JNICALL
// Java_com_example_myapp_NativeAudioProcessor_nativeProcessStream(
//     JNIEnv* env,
//     jobject /* this */,
//     jlong handle,
//     jshortArray near_end,
//     jint sample_rate_hz,
//     jint num_channels,
//     jint stream_delay_ms) {

//   ApmHandle* apm_handle = ToHandle(handle);
//   if (apm_handle == nullptr) {
//     return AudioProcessing::kNullPointerError;
//   }

//   apm_handle->apm->set_stream_delay_ms(stream_delay_ms);

//   jshort* data = env->GetShortArrayElements(near_end, nullptr);
//   const StreamConfig stream_config(sample_rate_hz, num_channels);
//   int result = apm_handle->apm->ProcessStream(
//       reinterpret_cast<int16_t*>(data), stream_config, stream_config,
//       reinterpret_cast<int16_t*>(data));
//   env->ReleaseShortArrayElements(near_end, data, 0);
//   return result;
// }
