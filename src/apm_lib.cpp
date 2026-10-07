#include "apm_lib.h"

// 전체 구조/처리 흐름 도식은 apm_lib.h 상단 주석 참고.
//
// [파일 구성]
//  1. 익명 namespace
//     - AecHandle      : jlong 핸들이 가리키는 인스턴스 (apm + nearend 미러 detector 상태)
//     - JNI 헬퍼       : 핸들 변환, 예외 throw, DirectByteBuffer 검증, RMS dBFS 계산
//  2. JNI 구현
//     - 생성/해제      : nativeCreate, nativeDestroy
//     - 오디오 처리    : nativeProcessRender (far-end), nativeProcessCapture (near-end)
//     - 상태/통계 조회 : nativeGetLikelihoodFiltered, nativeSetNearEndDetectorConfig,
//                        nativeIsNearEndState, nativeCalculateRmsDb
//  3. (주석 처리) 초기 C 스타일 apm_lib 클래스 구현 — 참고용

namespace {

    // nativeCreate에서 new로 생성되어 jlong 핸들로 Kotlin에 전달되고,
    // nativeDestroy에서 delete된다.
    struct AecHandle {
        AecHandle(int sample_rate_hz, int channels)
            : stream_config(sample_rate_hz, channels),
            frame_bytes(stream_config.num_samples() * sizeof(int16_t)),
            num_capture_channels(static_cast<size_t>(channels)) {
        }

        webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
        const webrtc::StreamConfig stream_config;
        const size_t frame_bytes;
        const size_t num_capture_channels;
        std::mutex mutex;  // apm(ProcessStream/ProcessReverseStream) 접근 보호

        // dominant nearend 상태 조회 전용 미러 인스턴스.
        // 실제 AEC3 config와는 분리되어 있으며, apm 처리 critical section을
        // 늘리지 않도록 별도 mutex로 보호한다.
        std::mutex nearend_mutex;
        std::unique_ptr<webrtc::DominantNearendDetector> nearend_detector;
        webrtc::Aec3Fft fft;
        bool nearend_state = false;
        

        // StreamConfig::num_samples()는 채널 수까지 곱해진 인터리브 샘플 개수이므로
        // (num_channels * num_frames), 지원 대역 중 가장 긴 프레임인
        // 48kHz/stereo/10ms 기준 480 * 2 = 960이 상한이다.
        static constexpr size_t kMaxNativeFrameSamples = 960;
        // ZeroPaddedFft()의 요구 입력 크기(kFftLengthBy2=64)로 프레임을 나누면
        // 나머지가 남을 수 있어(예: 16kHz 160샘플 = 64*2 + 32), 그 나머지를
        // 다음 nativeProcessCapture 호출로 이월해 10ms 프레임 전체가 누락 없이
        // 64샘플 블록에 포함되도록 한다.
        static constexpr size_t kMaxPendingSamples =
            kMaxNativeFrameSamples + webrtc::kFftLengthBy2 - 1;
        std::array<float, webrtc::kFftLengthBy2 - 1> nearend_pre_carry{};
        std::array<float, webrtc::kFftLengthBy2 - 1> nearend_post_carry{};
        size_t nearend_carry_count = 0;

        // enr/snr threshold 튜닝을 위한 임시 진단 로그 스로틀링(블록 단위).
        static constexpr int kNearendLogIntervalBlocks = 25;
        int nearend_log_counter = 0;

        // 블록(4ms) 단위 원시 스펙트럼은 NS/AEC의 순간적인 변동 때문에 너무 튀어서
        // (배경소음만 있어도 enr 문턱을 순간적으로 넘나듦) enr/snr 비교 전에 EMA로
        // 평활화한다. alpha ~= 2/(N+1), N=12블록(약 48ms) 기준.
        static constexpr float kNearendSpectrumSmoothingAlpha = 0.15f;
        std::array<float, webrtc::kFftLengthBy2Plus1> nearend_spectrum_smoothed{};
        std::array<float, webrtc::kFftLengthBy2Plus1> residual_echo_spectrum_smoothed{};
        bool nearend_smoothing_initialized = false;
    };

    float full_power_pcm = 32768.0f * 32768.0f;

    template <typename T>
    jlong ToJlong(T* p) {
        return reinterpret_cast<jlong>(p);
    }

    template <typename T>
    T* FromJlong(jlong p) {
        return reinterpret_cast<T*>(p);
    }

    void ThrowIllegalArgument(JNIEnv* env, const char* message) {
        jclass clazz = env->FindClass("java/lang/IllegalArgumentException");
        if (clazz != nullptr) {
            env->ThrowNew(clazz, message);
        }
    }

    // webrtc::AudioProcessing 내부 처리 실패(에러 코드)를 RuntimeException으로 변환.
    void ThrowApmError(JNIEnv* env, const char* what, int code) {
        char message[128];
        std::snprintf(message, sizeof(message), "%s failed (code=%d)", what, code);
        jclass clazz = env->FindClass("java/lang/RuntimeException");
        if (clazz != nullptr) {
            env->ThrowNew(clazz, message);
        }
    }

    bool IsNativePcm16Rate(int rate_hz) {
        for (int native_rate : webrtc::AudioProcessing::kNativeSampleRatesHz) {
            if (native_rate == rate_hz) return true;
        }
        return false;
    }

    int16_t* GetPcm(JNIEnv* env, jobject direct_buffer, jint byte_count) {
        if (direct_buffer == nullptr || byte_count <= 0 ||
            (byte_count % sizeof(int16_t)) != 0) {
            ThrowIllegalArgument(env, "PCM16 byte count must be positive and even");
            return nullptr;
        }

        void* address = env->GetDirectBufferAddress(direct_buffer);
        jlong capacity = env->GetDirectBufferCapacity(direct_buffer);
        if (address == nullptr || capacity < byte_count) {
            ThrowIllegalArgument(env, "PCM buffer must be a sufficiently large DirectByteBuffer");
            return nullptr;
        }

        return static_cast<int16_t*>(address);
    }

    bool HasWholeFrames(const AecHandle& handle, jint byte_count) {
        return byte_count > 0 &&
            static_cast<size_t>(byte_count) % handle.frame_bytes == 0;
    }

    // 16비트 양자화 잡음 바닥에 해당하는 dBFS 하한값.
    constexpr double kMinDbfs = -96.0;

    double ComputeRmsDbfs(const int16_t* samples, size_t sample_count) {
        if (sample_count == 0) return kMinDbfs;

        double sum_squares = 0.0;
        for (size_t i = 0; i < sample_count; ++i) {
            const double sample = samples[i];
            sum_squares += sample * sample;
        }

        const double rms = std::sqrt(sum_squares / static_cast<double>(sample_count));
        if (rms <= 0.0) return kMinDbfs;

        const double dbfs = 20.0 * std::log10(rms / 32768.0);
        return std::max(dbfs, kMinDbfs);
    }

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
//Java_com_selvas_echocancelsample_models_LocalEchoCanceller_nativeCreate(
Java_selvas_webrtc_apm_EchoCancel_CreateAPMHandle(
    JNIEnv* env, jclass, jint sample_rate_hz, jint channels, jboolean isNSON, jboolean isAGCOn) {
    // WebRTC PCM16 인터페이스는 8/16/32/48 kHz만 지원.
    if (!IsNativePcm16Rate(sample_rate_hz) ||
        (channels != 1 && channels != 2)) {
        ThrowIllegalArgument(env, "AEC requires 8/16/32/48 kHz PCM16, mono or stereo");
        return 0;
    }

    webrtc::AudioProcessing::Config config;
    config.high_pass_filter.enabled = false; //echo_canceller 내부에 HPF 존재
    
    config.echo_canceller.enabled = true;  //AEC
    
    //config.noise_suppression.enabled = false;
    config.noise_suppression.enabled = isNSON;
    config.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;

    // Android 마이크 gain을 이 JNI가 제어하지 않으므로 AGC는 비활성화.
    config.gain_controller1.enabled = false;

    //config.gain_controller2.enabled = false;
    config.gain_controller2.enabled = isAGCOn;
    config.gain_controller2.adaptive_digital.enabled = isAGCOn;


    // residual_echo_likelihood 등 잔여 에코 통계는 BuiltinAudioProcessingBuilder가
    // 기본으로 생성하지 않으므로, EchoDetector를 명시적으로 주입해야 GetStatistics()가
    // nullopt가 아닌 값을 채운다.
    auto apm = webrtc::BuiltinAudioProcessingBuilder(config)
        .SetEchoDetector(webrtc::CreateEchoDetector())
        .Build(webrtc::CreateEnvironment());
    if (apm == nullptr) {
        ThrowIllegalArgument(env, "Failed to create WebRTC AudioProcessing");
        return 0;
    }

    auto* handle = new AecHandle(sample_rate_hz, channels);
    handle->apm = std::move(apm);
    return ToJlong(handle);
}

// 스피커 신호 전달. Android 스피커 PCM을 출력 직전에 호출할 필요는 없음.
// pcm16은 10 ms의 크기만큼.
extern "C" JNIEXPORT jint JNICALL
Java_selvas_webrtc_apm_EchoCancel_ProcessRender(
    JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count) {
    auto* handle = FromJlong<AecHandle>(native_handle);
    if (handle == nullptr) {
        ThrowIllegalArgument(env, "invalid AEC handle");
        return webrtc::AudioProcessing::kNullPointerError;
    }

    int16_t* samples = GetPcm(env, pcm16, byte_count);
    if (samples == nullptr) return webrtc::AudioProcessing::kNullPointerError;
    if (!HasWholeFrames(*handle, byte_count)) {
        ThrowIllegalArgument(env, "render PCM must contain complete 10 ms frames");
        return webrtc::AudioProcessing::kBadDataLengthError;
    }

    std::lock_guard<std::mutex> lock(handle->mutex);
    for (size_t offset = 0; offset < static_cast<size_t>(byte_count);
        offset += handle->frame_bytes) {
        auto* frame = reinterpret_cast<int16_t*>(
            reinterpret_cast<uint8_t*>(samples) + offset);

        const int result = handle->apm->ProcessReverseStream(
            frame, handle->stream_config, handle->stream_config, frame);
        if (result != webrtc::AudioProcessing::kNoError) return result;
    }

    return webrtc::AudioProcessing::kNoError;
}

// AudioRecord PCM을 제자리에서 AEC/NS 처리하고, 처리된 동일 버퍼를 반환한다.
extern "C" JNIEXPORT jobject JNICALL
Java_selvas_webrtc_apm_EchoCancel_ProcessCapture(
    JNIEnv* env, jclass, jlong native_handle, jobject pcm16, jint byte_count,
    jint stream_delay_ms) {
    auto* handle = FromJlong<AecHandle>(native_handle);
    if (handle == nullptr) {
        ThrowIllegalArgument(env, "invalid AEC handle");
        return nullptr;
    }

    int16_t* samples = GetPcm(env, pcm16, byte_count);
    if (samples == nullptr) return nullptr;
    if (!HasWholeFrames(*handle, byte_count)) {
        ThrowIllegalArgument(env, "capture PCM must contain complete 10 ms frames");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        const int result = handle->apm->set_stream_delay_ms(stream_delay_ms);  //AEC3 필터에 전달하는 스피커-마이크 간 추정지연 시간
        if (result != webrtc::AudioProcessing::kNoError) {
            ThrowApmError(env, "set_stream_delay_ms", result);
            return nullptr;
        }
    }


    const size_t frame_samples = handle->stream_config.num_samples();

    for (size_t offset = 0; offset < static_cast<size_t>(byte_count);
        offset += handle->frame_bytes) {
        auto* frame = reinterpret_cast<int16_t*>(
            reinterpret_cast<uint8_t*>(samples) + offset);

        bool track_nearend;
        {
            std::lock_guard<std::mutex> lock(handle->nearend_mutex);
            track_nearend = (handle->nearend_detector != nullptr);
        }

        // AEC/NS 처리 전(에코 포함) 프레임 전체 보존 — nearend 미러 detector용.
        std::array<float, AecHandle::kMaxNativeFrameSamples> pre_process{};
        if (track_nearend) {
            for (size_t i = 0; i < frame_samples; ++i)
                pre_process[i] = static_cast<float>(frame[i]);
        }

        int result;
        {
            std::lock_guard<std::mutex> lock(handle->mutex);
            result = handle->apm->ProcessStream(
                frame, handle->stream_config, handle->stream_config, frame);
        }  // apm mutex 해제 — render 스레드의 ProcessReverseStream 대기 시간을 최소화
        if (result != webrtc::AudioProcessing::kNoError) {
            ThrowApmError(env, "ProcessStream", result);
            return nullptr;
        }

        if (track_nearend) {
            std::lock_guard<std::mutex> nearend_lock(handle->nearend_mutex);
            if (handle->nearend_detector != nullptr) {
                // ZeroPaddedFft()는 정확히 kFftLengthBy2(64) 크기 입력만 받는다(크기를 자체
                // 검증하지 않고, 짧으면 미초기화 메모리가, 길면 스택 오버플로우가 발생한다).
                // 10ms 프레임(예: 16kHz=160샘플)은 64로 나누어떨어지지 않으므로(160=64*2+32),
                // 매 호출마다 남는 나머지를 이전 호출의 나머지(carry)와 이어붙여 64샘플 블록
                // 단위로 처리하고, 다시 64샘플 미만으로 남은 꼬리는 다음 호출로 이월한다.
                // 이렇게 하면 10ms 프레임의 모든 샘플이 (호출 경계와 무관하게) 정확히 한 번씩
                // 어떤 64샘플 블록에 포함되어 detector에 반영된다.
                const size_t carry_count = handle->nearend_carry_count;
                const size_t pending_count = carry_count + frame_samples;

                std::array<float, AecHandle::kMaxPendingSamples> pending_pre{};
                std::array<float, AecHandle::kMaxPendingSamples> pending_post{};
                std::copy(handle->nearend_pre_carry.begin(),
                          handle->nearend_pre_carry.begin() + carry_count,
                          pending_pre.begin());
                std::copy(handle->nearend_post_carry.begin(),
                          handle->nearend_post_carry.begin() + carry_count,
                          pending_post.begin());
                for (size_t i = 0; i < frame_samples; ++i) {
                    pending_pre[carry_count + i] = pre_process[i];
                    pending_post[carry_count + i] = static_cast<float>(frame[i]);
                }

                size_t block_offset = 0;
                for (; block_offset + webrtc::kFftLengthBy2 <= pending_count;
                    block_offset += webrtc::kFftLengthBy2) {
                    std::array<float, webrtc::kFftLengthBy2> nearend_block{};
                    std::array<float, webrtc::kFftLengthBy2> echo_block{};
                    for (size_t i = 0; i < webrtc::kFftLengthBy2; ++i) {
                        nearend_block[i] = pending_post[block_offset + i];
                        // 제거된 성분(처리 전 - 처리 후) = 잔여 에코 근사.
                        echo_block[i] =
                            pending_pre[block_offset + i] - pending_post[block_offset + i];
                    }

                    webrtc::FftData nearend_fft;  // 처리 후 신호 = near-end 추정
                    handle->fft.ZeroPaddedFft(nearend_block, webrtc::Aec3Fft::Window::kRectangular,
                                               &nearend_fft);
                    webrtc::FftData residual_echo_fft;
                    handle->fft.ZeroPaddedFft(echo_block, webrtc::Aec3Fft::Window::kRectangular,
                                               &residual_echo_fft);

                    std::array<float, webrtc::kFftLengthBy2Plus1> nearend_spectrum{};
                    std::array<float, webrtc::kFftLengthBy2Plus1> residual_echo_spectrum{};
                    std::array<float, webrtc::kFftLengthBy2Plus1> comfort_noise_spectrum{};  // 0 근사(플로어)
                    nearend_fft.Spectrum(webrtc::Aec3Optimization::kNone, nearend_spectrum);
                    residual_echo_fft.Spectrum(webrtc::Aec3Optimization::kNone,
                                                residual_echo_spectrum);

                    // 블록 단위 원시 스펙트럼은 NS/AEC의 순간 변동 때문에 너무 튀므로
                    // (배경소음만 있어도 enr 문턱을 순간적으로 넘나듦) EMA로 평활화한 뒤
                    // 비교에 사용한다. 첫 블록은 초기값을 그대로 채택해 0에서부터
                    // 서서히 올라오는 편향을 없앤다.
                    if (!handle->nearend_smoothing_initialized) {
                        handle->nearend_spectrum_smoothed = nearend_spectrum;
                        handle->residual_echo_spectrum_smoothed = residual_echo_spectrum;
                        handle->nearend_smoothing_initialized = true;
                    } else {
                        constexpr float kAlpha = AecHandle::kNearendSpectrumSmoothingAlpha;
                        for (size_t i = 0; i < webrtc::kFftLengthBy2Plus1; ++i) {
                            handle->nearend_spectrum_smoothed[i] +=
                                kAlpha * (nearend_spectrum[i] - handle->nearend_spectrum_smoothed[i]);
                            handle->residual_echo_spectrum_smoothed[i] +=
                                kAlpha * (residual_echo_spectrum[i] -
                                          handle->residual_echo_spectrum_smoothed[i]);
                        }
                    }

                    handle->nearend_detector->Update(
                        std::span<const std::array<float, webrtc::kFftLengthBy2Plus1>>(
                            &handle->nearend_spectrum_smoothed, 1),
                        std::span<const std::array<float, webrtc::kFftLengthBy2Plus1>>(
                            &handle->residual_echo_spectrum_smoothed, 1),
                        std::span<const std::array<float, webrtc::kFftLengthBy2Plus1>>(
                            &comfort_noise_spectrum, 1),
                        /*initial_state=*/false);

                    handle->nearend_state = handle->nearend_detector->IsNearendState();

                    // enr_threshold/snr_threshold 튜닝용 임시 진단 로그.
                    // comfort_noise_spectrum이 항상 0이라 snr 조건은 사실상 무력화되어 있으므로,
                    // 여기서 실제 nearend_power/echo_power 비율을 확인해 enr_threshold를 잡는다.
                    if (++handle->nearend_log_counter >= AecHandle::kNearendLogIntervalBlocks) {
                        handle->nearend_log_counter = 0;
                        float nearend_power = std::accumulate(
                            handle->nearend_spectrum_smoothed.begin(),
                            handle->nearend_spectrum_smoothed.end(), 0.f) / handle->nearend_spectrum_smoothed.size();
                        nearend_power = nearend_power / full_power_pcm;
                        float echo_power = std::accumulate(
                            handle->residual_echo_spectrum_smoothed.begin(),
                            handle->residual_echo_spectrum_smoothed.end(), 0.f) / handle->residual_echo_spectrum_smoothed.size();
                        echo_power = echo_power / full_power_pcm;
                        __android_log_print(
                            ANDROID_LOG_DEBUG, "apm_lib_nearend",
                            "nearend_power=%.4f echo_power=%.4f ratio=%.4f state=%d",
                            nearend_power, echo_power,
                            echo_power > 0.f ? nearend_power / echo_power : -1.f,
                            handle->nearend_state ? 1 : 0);
                    }
                }

                handle->nearend_carry_count = pending_count - block_offset;
                std::copy(pending_pre.begin() + block_offset, pending_pre.begin() + pending_count,
                           handle->nearend_pre_carry.begin());
                std::copy(pending_post.begin() + block_offset, pending_post.begin() + pending_count,
                           handle->nearend_post_carry.begin());
            }
        }
    }

    //double isAECDone = handle->apm->GetStatistics(true).residual_echo_likelihood.value();

    return pcm16;  // 제자리 처리된 동일 버퍼를 그대로 반환 (추가 할당 없음)
}

extern "C" JNIEXPORT jdouble JNICALL
Java_selvas_webrtc_apm_EchoCancel_GetLikelihoodFiltered(
    JNIEnv* env, jclass, jlong native_handle) {
        auto* handle = FromJlong<AecHandle>(native_handle);
        if (handle == nullptr) {
            ThrowIllegalArgument(env, "invalid AEC handle");
            return 0;
        }

        const webrtc::AudioProcessingStats stats = handle->apm->GetStatistics(true);

        std::optional<double> likelihood = stats.residual_echo_likelihood;

        std::optional<int32_t> delay_ms = stats.delay_ms;
        std::optional<double> erle = stats.echo_return_loss_enhancement;
        std::optional<double> erl = stats.echo_return_loss;
        std::optional<double> divergent = stats.divergent_filter_fraction;


        if(delay_ms != std::nullopt && erle != std::nullopt && divergent != std::nullopt){
            __android_log_print(
                                ANDROID_LOG_DEBUG, "apm_lib_nearend",
                                "delay_ms=%.1f erle=%.1f div=%.1f",
                                delay_ms.value(), erle.value(),
                                divergent.value());                                 
        }



        if(delay_ms != std::nullopt){
            __android_log_print(
                                ANDROID_LOG_DEBUG, "apm_lib_nearend",
                                "delay_ms=%d",
                                delay_ms.value());                                 
        }
        
        if(erle != std::nullopt){
            __android_log_print(
                                ANDROID_LOG_DEBUG, "apm_lib_nearend",
                                "erle=%.1f",
                                erle.value());                                 
        }

        if(erl != std::nullopt){
            __android_log_print(
                                ANDROID_LOG_DEBUG, "apm_lib_nearend",
                                "erl=%.1f",
                                erl.value());                                 
        }

        if(divergent != std::nullopt){
            __android_log_print(
                                ANDROID_LOG_DEBUG, "apm_lib_nearend",
                                "divergent=%.1f",
                                divergent.value());                                 
        }




        if(likelihood == std::nullopt){
            return 0;
        }
        else{
            double likelihoodEchoResident = likelihood.value();
            return 1.0 - likelihoodEchoResident;
        }
}
		
// dominant nearend 상태 조회용 미러 detector의 threshold 설정.
// 실제 AEC3 처리 config(nativeCreate)에는 반영되지 않는 별도 인스턴스이다.
extern "C" JNIEXPORT void JNICALL
Java_selvas_webrtc_apm_EchoCancel_SetNearEndDetectorConfig(
    JNIEnv* env, jclass, jlong native_handle, jfloat enrThreshold, jfloat snrThreshold) {
    auto* handle = FromJlong<AecHandle>(native_handle);
    if (handle == nullptr) {
        ThrowIllegalArgument(env, "invalid AEC handle");
        return;
    }

    webrtc::EchoCanceller3Config::Suppressor::DominantNearendDetection nearend_config;
    nearend_config.enr_threshold = enrThreshold;
    nearend_config.snr_threshold = snrThreshold;

    std::lock_guard<std::mutex> lock(handle->nearend_mutex);
    handle->nearend_detector = std::make_unique<webrtc::DominantNearendDetector>(
        nearend_config, handle->num_capture_channels);
    handle->nearend_state = false;
    handle->nearend_carry_count = 0;  // 이전 carry는 재구성된 detector와 무관하므로 폐기
    handle->nearend_smoothing_initialized = false;  // 평활화 상태도 새 detector 기준으로 재시작
}

// nativeSetNearEndDetectorConfig() 호출 전이면 항상 false를 반환한다.
extern "C" JNIEXPORT jboolean JNICALL
Java_selvas_webrtc_apm_EchoCancel_IsNearEndState(
    JNIEnv* env, jclass, jlong native_handle) {
    auto* handle = FromJlong<AecHandle>(native_handle);
    if (handle == nullptr) {
        ThrowIllegalArgument(env, "invalid AEC handle");
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(handle->nearend_mutex);
    return handle->nearend_state ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_selvas_webrtc_apm_EchoCancel_DestroyAPMHandle(
    JNIEnv*, jclass, jlong native_handle) {
    delete FromJlong<AecHandle>(native_handle);
}

// PCM16 프레임의 RMS 음성 에너지를 dBFS(-96.0 ~ 0.0)로 반환.
extern "C" JNIEXPORT jdouble JNICALL
Java_selvas_webrtc_apm_EchoCancel_CalculateRmsDb(
    JNIEnv* env, jclass, jobject pcm16, jint byte_count) {
    int16_t* samples = GetPcm(env, pcm16, byte_count);
    if (samples == nullptr) return kMinDbfs;

    const size_t sample_count = static_cast<size_t>(byte_count) / sizeof(int16_t);
    return ComputeRmsDbfs(samples, sample_count);
}


//#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "apm_lib", __VA_ARGS__))
//#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "apm_lib", __VA_ARGS__))
//
//extern "C" {
//	/* 이 Trivial 함수는 이 동적 네이티브 라이브러리가 컴파일되는 플랫폼 ABI를 반환합니다.*/
//	const char * apm_lib::getPlatformABI()
//	{
//	#if defined(__arm__)
//	#if defined(__ARM_ARCH_7A__)
//	#if defined(__ARM_NEON__)
//		#define ABI "armeabi-v7a/NEON"
//	#else
//		#define ABI "armeabi-v7a"
//	#endif
//	#else
//		#define ABI "armeabi"
//	#endif
//	#elif defined(__i386__)
//		#define ABI "x86"
//	#else
//		#define ABI "unknown"
//	#endif
//		LOGI("This dynamic shared library is compiled with ABI: %s", ABI);
//		return "This native library is compiled with ABI: %s" ABI ".";
//	}
//	
//	void apm_lib()
//	{
//	}
//
//	apm_lib::apm_lib()
//	{
//	}
//
//	apm_lib::~apm_lib()
//	{
//	}
//
//
//	apm_lib::APM_HANDLE Apm_Create(int sample_rate_hz, int channels) {
//		// 1. WebRTC AudioProcessing 객체 생성
//		webrtc::scoped_refptr<webrtc::AudioProcessing> apm =
//			webrtc::AudioProcessingBuilderInterface().Create();
//
//		if (!apm) return nullptr;
//
//		// 2. 기본 APM 기능 활성화 (노이즈 억제, 에코 캔슬러 등)
//		webrtc::AudioProcessing::Config config;
//		config.echo_canceller.enabled = true;
//		config.noise_suppression.enabled = true;
//		config.noise_suppression.level =
//			webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
//		config.gain_controller1.enabled = true;
//		apm->ApplyConfig(config);
//
//		// 3. 컨텍스트 객체 생성 후 C 핸들(void*)로 반환
//		ApmContext* ctx = new ApmContext(sample_rate_hz, channels);
//		ctx->apm = apm;
//
//		return static_cast<APM_HANDLE>(ctx);
//	}
//}