// #include <jni.h>

// #include <cstdint>
// #include <mutex>

// #include "api/audio/audio_processing.h"
// #include "api/audio/builtin_audio_processing_builder.h"
// #include "api/environment/environment_factory.h"
// #include "api/scoped_refptr.h"

// namespace {

// struct AecHandle {
//   AecHandle(int sample_rate_hz, int channels)
//       : stream_config(sample_rate_hz, channels),
//         frame_bytes(stream_config.num_samples() * sizeof(int16_t)) {}

//   webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
//   const webrtc::StreamConfig stream_config;
//   const size_t frame_bytes;
//   std::mutex mutex;
// };

// template <typename T>
// jlong ToJlong(T* p) {
//   return reinterpret_cast<jlong>(p);
// }

// template <typename T>
// T* FromJlong(jlong p) {
//   return reinterpret_cast<T*>(p);
// }

// void ThrowIllegalArgument(JNIEnv* env, const char* message) {
//   jclass clazz = env->FindClass("java/lang/IllegalArgumentException");
//   if (clazz != nullptr) {
//     env->ThrowNew(clazz, message);
//   }
// }

// bool IsNativePcm16Rate(int rate_hz) {
//   for (int native_rate : webrtc::AudioProcessing::kNativeSampleRatesHz) {
//     if (native_rate == rate_hz) return true;
//   }
//   return false;
// }

// int16_t* GetPcm(JNIEnv* env, jobject direct_buffer, jint byte_count) {
//   if (direct_buffer == nullptr || byte_count <= 0 ||
//       (byte_count % sizeof(int16_t)) != 0) {
//     ThrowIllegalArgument(env, "PCM16 byte count must be positive and even");
//     return nullptr;
//   }

//   void* address = env->GetDirectBufferAddress(direct_buffer);
//   jlong capacity = env->GetDirectBufferCapacity(direct_buffer);
//   if (address == nullptr || capacity < byte_count) {
//     ThrowIllegalArgument(env, "PCM buffer must be a sufficiently large DirectByteBuffer");
//     return nullptr;
//   }

//   return static_cast<int16_t*>(address);
// }

// bool HasWholeFrames(const AecHandle& handle, jint byte_count) {
//   return byte_count > 0 &&
//          static_cast<size_t>(byte_count) % handle.frame_bytes == 0;
// }

// }  // namespace

// extern "C" JNIEXPORT jlong JNICALL
// Java_com_example_audio_LocalEchoCanceller_nativeCreate(
//     JNIEnv* env, jclass, jint sample_rate_hz, jint channels) {
//   // WebRTC PCM16 인터페이스는 8/16/32/48 kHz만 지원.
//   if (!IsNativePcm16Rate(sample_rate_hz) ||
//       (channels != 1 && channels != 2)) {
//     ThrowIllegalArgument(env, "AEC requires 8/16/32/48 kHz PCM16, mono or stereo");
//     return 0;
//   }

//   webrtc::AudioProcessing::Config config;
//   config.echo_canceller.enabled = true;
//   config.high_pass_filter.enabled = true;
//   config.noise_suppression.enabled = true;
//   config.noise_suppression.level =
//       webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;

//   // Android 마이크 gain을 이 JNI가 제어하지 않으므로 AGC는 비활성화.
//   config.gain_controller1.enabled = false;
//   config.gain_controller2.enabled = false;

//   auto apm = webrtc::BuiltinAudioProcessingBuilder(config)
//                  .Build(webrtc::CreateEnvironment());
//   if (apm == nullptr) {
//     ThrowIllegalArgument(env, "Failed to create WebRTC AudioProcessing");
//     return 0;
//   }

//   auto* handle = new AecHandle(sample_rate_hz, channels);
//   handle->apm = std::move(apm);
//   return ToJlong(handle);
// }

// // Android 미디어 채널 PCM을 AudioTrack.write() 직전에 호출.
// // pcm16은 10 ms의 정수배여야 한다.
// extern "C" JNIEXPORT jint JNICALL
// Java_com_example_audio_LocalEchoCanceller_nativeProcessRender(
//     JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count) {
//   auto* handle = FromJlong<AecHandle>(native_handle);
//   if (handle == nullptr) {
//     ThrowIllegalArgument(env, "invalid AEC handle");
//     return webrtc::AudioProcessing::kNullPointerError;
//   }

//   int16_t* samples = GetPcm(env, pcm16, byte_count);
//   if (samples == nullptr) return webrtc::AudioProcessing::kNullPointerError;
//   if (!HasWholeFrames(*handle, byte_count)) {
//     ThrowIllegalArgument(env, "render PCM must contain complete 10 ms frames");
//     return webrtc::AudioProcessing::kBadDataLengthError;
//   }

//   std::lock_guard<std::mutex> lock(handle->mutex);
//   for (size_t offset = 0; offset < static_cast<size_t>(byte_count);
//        offset += handle->frame_bytes) {
//     auto* frame = reinterpret_cast<int16_t*>(
//         reinterpret_cast<uint8_t*>(samples) + offset);

//     const int result = handle->apm->ProcessReverseStream(
//         frame, handle->stream_config, handle->stream_config, frame);
//     if (result != webrtc::AudioProcessing::kNoError) return result;
//   }

//   return webrtc::AudioProcessing::kNoError;
// }

// // AudioRecord PCM을 제자리에서 AEC/NS 처리한다.
// extern "C" JNIEXPORT jint JNICALL
// Java_com_example_audio_LocalEchoCanceller_nativeProcessCapture(
//     JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count,
//     jint stream_delay_ms) {
//   auto* handle = FromJlong<AecHandle>(native_handle);
//   if (handle == nullptr) {
//     ThrowIllegalArgument(env, "invalid AEC handle");
//     return webrtc::AudioProcessing::kNullPointerError;
//   }

//   int16_t* samples = GetPcm(env, pcm16, byte_count);
//   if (samples == nullptr) return webrtc::AudioProcessing::kNullPointerError;
//   if (!HasWholeFrames(*handle, byte_count)) {
//     ThrowIllegalArgument(env, "capture PCM must contain complete 10 ms frames");
//     return webrtc::AudioProcessing::kBadDataLengthError;
//   }

//   std::lock_guard<std::mutex> lock(handle->mutex);

//   int result = handle->apm->set_stream_delay_ms(stream_delay_ms);
//   if (result != webrtc::AudioProcessing::kNoError) return result;

//   for (size_t offset = 0; offset < static_cast<size_t>(byte_count);
//        offset += handle->frame_bytes) {
//     auto* frame = reinterpret_cast<int16_t*>(
//         reinterpret_cast<uint8_t*>(samples) + offset);

//     result = handle->apm->ProcessStream(
//         frame, handle->stream_config, handle->stream_config, frame);
//     if (result != webrtc::AudioProcessing::kNoError) return result;
//   }

//   return webrtc::AudioProcessing::kNoError;
// }

// extern "C" JNIEXPORT void JNICALL
// Java_com_example_audio_LocalEchoCanceller_nativeDestroy(
//     JNIEnv*, jclass, jlong native_handle) {
//   delete FromJlong<AecHandle>(native_handle);
// }