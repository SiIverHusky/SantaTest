#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

    // AEC Configuration methods
    void PrintAfeStatus();
    void SetAecMode(int aec_mode);
    void SetAecSuppressionLevel(int level);
    int GetCurrentAecMode();
    void SetVadSensitivity(int sensitivity);
    void PrintAvailableAecModes();
    
    // Configuration query methods
    bool IsAecEnabled();
    bool IsVadEnabled();
    void GetAfeStatus();

private:
    EventGroupHandle_t event_group_ = nullptr;
    esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> output_buffer_;


    int current_aec_mode_ = 1;  
    int current_suppression_level_ = 2;  
    int current_vad_sensitivity_ = 0;  
    bool aec_enabled_ = false;
    bool vad_enabled_ = false;

    void AudioProcessorTask();
    void UpdateAfeConfiguration();
    bool ReinitializeAfe();
};

#endif