#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "assets/lang_config.h"

#include <cstring>
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Application"

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting", 
    "idle",
    "invalid_state"
};

Application::Application() {
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();
    display->SetEmotion("star");
    display->SetStatus("Ready");

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    /* Start the clock timer */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "Santa Control System Ready!");
    
    // Print heap stats
    SystemInfo::PrintHeapStats();
}

void Application::OnClockTimer() {
    clock_ticks_++;
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    if (clock_ticks_ % 10 == 0) {
        SystemInfo::PrintHeapStats();
    }
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    switch (state) {
        case kDeviceStateIdle:
            display->SetStatus("Ready");
            display->SetEmotion("star");
            break;
        default:
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

// Required function implementations for linking
void Application::PlaySound(const std::string_view& sound) {
    ESP_LOGI(TAG, "Playing sound: %.*s", (int)sound.length(), sound.data());
    audio_service_.PlaySound(sound);
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::MainEventLoop() {
    ESP_LOGI(TAG, "Starting main event loop");
    
    // Simple main event loop for this basic version
    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        // Basic maintenance tasks
        if (clock_ticks_ % 60 == 0) {  // Every minute
            SystemInfo::PrintHeapStats();
        }
        
        // Keep the system responsive
        taskYIELD();
    }
}

// Motor control functions - these will be called by web interface
void Application::TriggerDance() {
    ESP_LOGI(TAG, "Dance triggered");
    // Implementation can be added here if needed
}

void Application::TriggerHeadShake() {
    ESP_LOGI(TAG, "Head shake triggered");
    // Implementation can be added here if needed
}

void Application::TriggerHipShake() {
    ESP_LOGI(TAG, "Hip shake triggered");
    // Implementation can be added here if needed
}

void Application::ChangeEmotion(const std::string& emotion) {
    ESP_LOGI(TAG, "Emotion changed to: %s", emotion.c_str());
    auto display = Board::GetInstance().GetDisplay();
    display->SetEmotion(emotion.c_str());
}

// Additional helper functions
void Application::SetStatus(const char* status) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
}

void Application::SetEmotion(const char* emotion) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetEmotion(emotion);
}

void Application::Log(const char* message) {
    ESP_LOGI(TAG, "%s", message);
}