#include "wifi_board.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "audio/codecs/santa_audio_codec.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emoji_display.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <string.h>
#include <cstdlib>

#define TAG "HeySanta"

float m1_coefficient = 1.0;
float m2_coefficient = 1.0;

// Global variables for web server
static httpd_handle_t control_server = NULL;
static bool web_server_active = false;

// Global stop flag for scenes
static bool scene_stop_requested = false;

class HeySantaCodec : public SantaAudioCodec {
public:
    HeySantaCodec(i2c_master_bus_handle_t i2c_bus, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din, uint8_t es7210_addr, bool input_reference)
        : SantaAudioCodec(i2c_bus, input_sample_rate, output_sample_rate,
                             mclk, bclk, ws, dout, din, es7210_addr, input_reference) {}

    virtual void EnableOutput(bool enable) override {
        SantaAudioCodec::EnableOutput(enable);
    }
};

class HeySantaBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button wake_button_;
    anim::EmojiWidget* display_ = nullptr;
    Esp32Camera* camera_;
    
    // Motor control functions
    void Initialize_Motor(void) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_MODE,
            .duty_resolution  = LEDC_DUTY_RES,
            .timer_num        = LEDC_TIMER,
            .freq_hz          = LEDC_FREQUENCY,
            .clk_cfg          = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

        const uint8_t motor_ledc_channel[LEDC_CHANNEL_COUNT] = {LEDC_M1_CHANNEL_A, LEDC_M1_CHANNEL_B, LEDC_M2_CHANNEL_A, LEDC_M2_CHANNEL_B};
        const int32_t ledc_channel_pins[LEDC_CHANNEL_COUNT] = {LEDC_M1_CHANNEL_A_IO, LEDC_M1_CHANNEL_B_IO, LEDC_M2_CHANNEL_A_IO, LEDC_M2_CHANNEL_B_IO};
        
        for (int i = 0; i < LEDC_CHANNEL_COUNT; i++) {
            ledc_channel_config_t ledc_channel = {
                .gpio_num       = ledc_channel_pins[i],
                .speed_mode     = LEDC_MODE,
                .channel        = (ledc_channel_t)motor_ledc_channel[i],
                .intr_type      = LEDC_INTR_DISABLE,
                .timer_sel      = LEDC_TIMER,
                .duty           = 0,
                .hpoint         = 0
            };
            ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        }
    }

    static void set_motor_A_speed(int speed) {
        if (speed >= 0) {
            uint32_t m1a_duty = (uint32_t)((speed * m1_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, m1a_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
        } else {
            uint32_t m1b_duty = (uint32_t)((-speed * m1_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, m1b_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
        }
    }

    static void set_motor_B_speed(int speed) {
        if (speed >= 0) {
            uint32_t m2a_duty = (uint32_t)((speed * m2_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_A, m2a_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_B, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_B));
        } else {
            uint32_t m2b_duty = (uint32_t)((-speed * m2_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_A, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_B, m2b_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_B));
        }
    }

    // Function to stop all motors immediately
    static void stop_all_motors() {
        ESP_LOGI(TAG, "Stopping all motors");
        set_motor_A_speed(0);
        set_motor_B_speed(0);
    }

    uint32_t unbiasedRandom3() {
        uint32_t r;
        const uint32_t upper_bound = 0xFFFFFFFF - (0xFFFFFFFF % 3);
        do {
            r = esp_random();
        } while (r >= upper_bound);
        return r % 3;
    }

    uint32_t unbiasedRandomRange(int min, int max) {
        uint32_t range = max - min + 1;
        uint32_t upper_bound = 0xFFFFFFFF - (0xFFFFFFFF % range);
        uint32_t r;
        do {
            r = esp_random();
        } while (r >= upper_bound);
        return min + (r % range);
    }

    // Modified shake body method with stop check
    void ShakeBody() {
        ESP_LOGI(TAG, "Body shake start - 9 seconds");
        set_motor_A_speed(100);
        
        // Check for stop every 100ms during the 9 second shake
        for (int i = 0; i < 90; i++) {
            if (scene_stop_requested) {
                ESP_LOGI(TAG, "Scene stop requested during body shake");
                stop_all_motors();
                scene_stop_requested = false; // Reset flag
                return;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        set_motor_A_speed(0);  // Stop motor
    }

    // Modified special shake for scene 33b7 with stop check
    void ShakeHipsSpecial() {
        ESP_LOGI(TAG, "Hip shake - low speed 5s + high speed 5s");
        
        // Low speed 5 seconds
        set_motor_B_speed(50);
        for (int i = 0; i < 50; i++) {
            if (scene_stop_requested) {
                ESP_LOGI(TAG, "Scene stop requested during hip shake (low speed)");
                stop_all_motors();
                scene_stop_requested = false;
                return;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        // High speed 5 seconds  
        set_motor_B_speed(100);
        for (int i = 0; i < 50; i++) {
            if (scene_stop_requested) {
                ESP_LOGI(TAG, "Scene stop requested during hip shake (high speed)");
                stop_all_motors();
                scene_stop_requested = false;
                return;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        set_motor_B_speed(0);  // Stop motor
    }

    // Scene methods
    void ExecuteScene7c2() {
        scene_stop_requested = false; // Reset stop flag
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("heart");
        ShakeBody();
    }

    void ExecuteScene9c5() {
        scene_stop_requested = false; // Reset stop flag
        auto display = Board::GetInstance().GetDisplay();
        // Loop between normal and happy during shake
        display->SetEmotion("neutral");
        for (int i = 0; i < 3; i++) {
            if (scene_stop_requested) {
                ESP_LOGI(TAG, "Scene stop requested during 9c5");
                stop_all_motors();
                scene_stop_requested = false;
                return;
            }
            
            set_motor_A_speed(100);
            for (int j = 0; j < 15; j++) {
                if (scene_stop_requested) {
                    stop_all_motors();
                    scene_stop_requested = false;
                    return;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            
            display->SetEmotion("happy");
            for (int j = 0; j < 15; j++) {
                if (scene_stop_requested) {
                    stop_all_motors();
                    scene_stop_requested = false;
                    return;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            
            display->SetEmotion("neutral");
            for (int j = 0; j < 15; j++) {
                if (scene_stop_requested) {
                    stop_all_motors();
                    scene_stop_requested = false;
                    return;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }
        set_motor_A_speed(0);
    }

    void ExecuteScene10g1() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("bell");
        ShakeBody();
    }

    void ExecuteScene11g2() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        ShakeBody();
    }

    void ExecuteScene12g3_1() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy2");
        ShakeBody();
    }

    void ExecuteScene12g3_2() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        ShakeBody();
    }

    void ExecuteScene14h1() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("neutral");
        ShakeBody();
    }

    void ExecuteScene21e3() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("star");
        ShakeBody();
    }

    void ExecuteScene23e2() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("cookie");
        ShakeBody();
    }

    void ExecuteScene25d1() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("neutral");
        // No body shake
    }

    void ExecuteScene26d2() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("star");
        ShakeBody();
    }

    void ExecuteScene27d3() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        // No body shake
    }

    void ExecuteScene28b8() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("star");
        ShakeBody();
    }

    void ExecuteScene30b6() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("snowman");
        ShakeBody();
    }

    void ExecuteScene33b7() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        ShakeHipsSpecial();
    }

    void ExecuteScene35b5() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        // No shake
    }

    void ExecuteScene36f5() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("neutral");
        
        // 2 second delay with stop check
        for (int i = 0; i < 20; i++) {
            if (scene_stop_requested) {
                ESP_LOGI(TAG, "Scene stop requested during 36f5 delay");
                scene_stop_requested = false;
                return;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        ShakeBody();
    }

    void ExecuteScene37f4() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("elf");
        ShakeBody();
    }

    void ExecuteScene38f1() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("sleep");
        // No shake
    }

    void ExecuteScene40() {
        scene_stop_requested = false;
        auto display = Board::GetInstance().GetDisplay();
        display->SetEmotion("happy");
        ShakeBody();
    }

    // Web server handlers
    static esp_err_t control_page_handler(httpd_req_t *req) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        
        const char* html = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<title>🎅 Santa Scene Control</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background: linear-gradient(135deg, #2E7D32 0%, #C62828 50%, #2E7D32 100%); color: white; min-height: 100vh; }"
        ".container { max-width: 1200px; margin: 0 auto; background: rgba(255,255,255,0.15); padding: 40px; border-radius: 25px; backdrop-filter: blur(15px); box-shadow: 0 10px 40px rgba(0,0,0,0.3); }"
        "h1 { color: #fff; margin-bottom: 30px; font-size: 32px; }"
        "h2 { color: #fff; margin: 30px 0 20px 0; font-size: 24px; }"
        "button { padding: 12px 20px; margin: 6px; font-size: 14px; border: none; border-radius: 8px; cursor: pointer; min-width: 120px; transition: all 0.3s ease; font-weight: bold; }"
        ".scene-btn { background: linear-gradient(45deg, #FF9800, #F57C00); color: white; box-shadow: 0 4px 15px rgba(255, 152, 0, 0.4); }"
        ".emotion-btn { background: linear-gradient(45deg, #E91E63, #C2185B); color: white; box-shadow: 0 4px 15px rgba(233, 30, 99, 0.4); }"
        ".emergency-btn { background: linear-gradient(45deg, #FF5722, #D84315); color: white; box-shadow: 0 6px 20px rgba(255, 87, 34, 0.5); font-size: 18px; min-width: 200px; }"
        ".stop-btn { background: linear-gradient(45deg, #F44336, #D32F2F); color: white; box-shadow: 0 6px 20px rgba(244, 67, 54, 0.4); }"
        "button:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(0,0,0,0.4); }"
        ".control-section { margin: 30px 0; padding: 25px; background: rgba(255,255,255,0.1); border-radius: 20px; }"
        ".scene-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 8px; margin: 20px 0; }"
        ".emotion-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 8px; margin: 20px 0; }"
        ".emergency-section { margin: 20px 0; padding: 20px; background: rgba(255,87,34,0.2); border-radius: 15px; border: 2px solid rgba(255,87,34,0.5); }"
        ".status { margin: 25px 0; padding: 20px; background: rgba(255,255,255,0.1); border-radius: 15px; font-weight: bold; font-size: 18px; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>🎅 Santa Scene Control 🎬</h1>"
        
        "<div class='emergency-section'>"
        "<h2>🚨 Emergency Controls</h2>"
        "<button class='emergency-btn' onclick='stopScene()'>⏹️ STOP SCENE</button>"
        "</div>"
        
        "<div class='control-section'>"
        "<h2>🎬 Scene Controls</h2>"
        "<div class='scene-grid'>"
        "<button class='scene-btn' onclick='executeScene(\"7c2\")'>7c2</button>"
        "<button class='scene-btn' onclick='executeScene(\"9c5\")'>9c5</button>"
        "<button class='scene-btn' onclick='executeScene(\"10g1\")'>10g1</button>"
        "<button class='scene-btn' onclick='executeScene(\"11g2\")'>11g2</button>"
        "<button class='scene-btn' onclick='executeScene(\"12g3-1\")'>12g3.1</button>"
        "<button class='scene-btn' onclick='executeScene(\"12g3-2\")'>12g3.2</button>"
        "<button class='scene-btn' onclick='executeScene(\"14h1\")'>14h1</button>"
        "<button class='scene-btn' onclick='executeScene(\"21e3\")'>21e3</button>"
        "<button class='scene-btn' onclick='executeScene(\"23e2\")'>23e2</button>"
        "<button class='scene-btn' onclick='executeScene(\"25d1\")'>25d1</button>"
        "<button class='scene-btn' onclick='executeScene(\"26d2\")'>26d2</button>"
        "<button class='scene-btn' onclick='executeScene(\"27d3\")'>27d3</button>"
        "<button class='scene-btn' onclick='executeScene(\"28b8\")'>28b8</button>"
        "<button class='scene-btn' onclick='executeScene(\"30b6\")'>30b6</button>"
        "<button class='scene-btn' onclick='executeScene(\"33b7\")'>33b7</button>"
        "<button class='scene-btn' onclick='executeScene(\"35b5\")'>35b5</button>"
        "<button class='scene-btn' onclick='executeScene(\"36f5\")'>36f5</button>"
        "<button class='scene-btn' onclick='executeScene(\"37f4\")'>37f4</button>"
        "<button class='scene-btn' onclick='executeScene(\"38f1\")'>38f1</button>"
        "<button class='scene-btn' onclick='executeScene(\"40\")'>40</button>"
        "</div>"
        "</div>"
        
        "<div class='control-section'>"
        "<h2>😊 Quick Emotion Controls</h2>"
        "<div class='emotion-grid'>"
        "<button class='emotion-btn' onclick='setEmotion(\"bell\")'>🔔 Bell</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"blinking\")'>😊 Blinking</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"cookie\")'>🍪 Cookie</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"heart\")'>❤️ Heart</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"sleep\")'>😴 Sleep</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"snowman\")'>⛄ Snowman</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"star\")'>⭐ Star</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"elf\")'>🧝 Elf</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"cross\")'>❌ Cross</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"cross2\")'>❌ Cross2</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"happy\")'>😄 Happy</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"happy2\")'>😁 Happy2</button>"
        "<button class='emotion-btn' onclick='setEmotion(\"neutral\")'>😐 Neutral</button>"
        "</div>"
        "</div>"
        
        "<div class='control-section'>"
        "<button class='stop-btn' onclick='stopServer()'>🛑 CLOSE CONTROL PANEL</button>"
        "</div>"
        
        "<div id='status' class='status'>🎬 Santa Scene Control Ready!</div>"
        "</div>"
        
        "<script>"
        "function executeScene(sceneId) {"
        "  console.log('Executing scene:', sceneId);"
        "  document.getElementById('status').innerText = 'Executing scene ' + sceneId + '...';"
        "  "
        "  fetch('/scene?id=' + sceneId)"
        "    .then(response => response.text())"
        "    .then(data => {"
        "      console.log('Response:', data);"
        "      document.getElementById('status').innerText = data;"
        "    })"
        "    .catch(error => {"
        "      console.error('Error:', error);"
        "      document.getElementById('status').innerText = 'Error: ' + error;"
        "    });"
        "}"
        
        "function setEmotion(emotionType) {"
        "  console.log('Setting emotion:', emotionType);"
        "  document.getElementById('status').innerText = 'Setting emotion to ' + emotionType + '...';"
        "  "
        "  fetch('/emotion?type=' + emotionType)"
        "    .then(response => response.text())"
        "    .then(data => {"
        "      console.log('Response:', data);"
        "      document.getElementById('status').innerText = data;"
        "    })"
        "    .catch(error => {"
        "      console.error('Error:', error);"
        "      document.getElementById('status').innerText = 'Error: ' + error;"
        "    });"
        "}"
        
        "function stopScene() {"
        "  console.log('Stopping scene');"
        "  document.getElementById('status').innerText = 'STOPPING SCENE...';"
        "  "
        "  fetch('/scene-stop')"
        "    .then(response => response.text())"
        "    .then(data => {"
        "      console.log('Response:', data);"
        "      document.getElementById('status').innerText = data;"
        "    })"
        "    .catch(error => {"
        "      console.error('Error:', error);"
        "      document.getElementById('status').innerText = 'Error: ' + error;"
        "    });"
        "}"
        
        "function stopServer() {"
        "  document.getElementById('status').innerText = 'Stopping control panel...';"
        "  fetch('/stop')"
        "    .then(response => response.text())"
        "    .then(data => {"
        "      document.getElementById('status').innerText = data;"
        "    });"
        "}"
        "</script>"
        "</body>"
        "</html>";
        
        httpd_resp_send(req, html, strlen(html));
        return ESP_OK;
    }

    // Single scene handler for ALL scenes using query parameters
    static esp_err_t scene_handler(httpd_req_t *req) {
        auto& board = static_cast<HeySantaBoard&>(Board::GetInstance());
        
        char query[100];
        char scene_id[50];
        
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            if (httpd_query_key_value(query, "id", scene_id, sizeof(scene_id)) == ESP_OK) {
                ESP_LOGI(TAG, "Executing scene: %s", scene_id);
                
                if (strcmp(scene_id, "7c2") == 0) {
                    board.ExecuteScene7c2();
                    httpd_resp_send(req, "🎬 Scene 7c2 executed!", -1);
                }
                else if (strcmp(scene_id, "9c5") == 0) {
                    board.ExecuteScene9c5();
                    httpd_resp_send(req, "🎬 Scene 9c5 executed!", -1);
                }
                else if (strcmp(scene_id, "10g1") == 0) {
                    board.ExecuteScene10g1();
                    httpd_resp_send(req, "🎬 Scene 10g1 executed!", -1);
                }
                else if (strcmp(scene_id, "11g2") == 0) {
                    board.ExecuteScene11g2();
                    httpd_resp_send(req, "🎬 Scene 11g2 executed!", -1);
                }
                else if (strcmp(scene_id, "12g3-1") == 0) {
                    board.ExecuteScene12g3_1();
                    httpd_resp_send(req, "🎬 Scene 12g3.1 executed!", -1);
                }
                else if (strcmp(scene_id, "12g3-2") == 0) {
                    board.ExecuteScene12g3_2();
                    httpd_resp_send(req, "🎬 Scene 12g3.2 executed!", -1);
                }
                else if (strcmp(scene_id, "14h1") == 0) {
                    board.ExecuteScene14h1();
                    httpd_resp_send(req, "🎬 Scene 14h1 executed!", -1);
                }
                else if (strcmp(scene_id, "21e3") == 0) {
                    board.ExecuteScene21e3();
                    httpd_resp_send(req, "🎬 Scene 21e3 executed!", -1);
                }
                else if (strcmp(scene_id, "23e2") == 0) {
                    board.ExecuteScene23e2();
                    httpd_resp_send(req, "🎬 Scene 23e2 executed!", -1);
                }
                else if (strcmp(scene_id, "25d1") == 0) {
                    board.ExecuteScene25d1();
                    httpd_resp_send(req, "🎬 Scene 25d1 executed!", -1);
                }
                else if (strcmp(scene_id, "26d2") == 0) {
                    board.ExecuteScene26d2();
                    httpd_resp_send(req, "🎬 Scene 26d2 executed!", -1);
                }
                else if (strcmp(scene_id, "27d3") == 0) {
                    board.ExecuteScene27d3();
                    httpd_resp_send(req, "🎬 Scene 27d3 executed!", -1);
                }
                else if (strcmp(scene_id, "28b8") == 0) {
                    board.ExecuteScene28b8();
                    httpd_resp_send(req, "🎬 Scene 28b8 executed!", -1);
                }
                else if (strcmp(scene_id, "30b6") == 0) {
                    board.ExecuteScene30b6();
                    httpd_resp_send(req, "🎬 Scene 30b6 executed!", -1);
                }
                else if (strcmp(scene_id, "33b7") == 0) {
                    board.ExecuteScene33b7();
                    httpd_resp_send(req, "🎬 Scene 33b7 executed!", -1);
                }
                else if (strcmp(scene_id, "35b5") == 0) {
                    board.ExecuteScene35b5();
                    httpd_resp_send(req, "🎬 Scene 35b5 executed!", -1);
                }
                else if (strcmp(scene_id, "36f5") == 0) {
                    board.ExecuteScene36f5();
                    httpd_resp_send(req, "🎬 Scene 36f5 executed!", -1);
                }
                else if (strcmp(scene_id, "37f4") == 0) {
                    board.ExecuteScene37f4();
                    httpd_resp_send(req, "🎬 Scene 37f4 executed!", -1);
                }
                else if (strcmp(scene_id, "38f1") == 0) {
                    board.ExecuteScene38f1();
                    httpd_resp_send(req, "🎬 Scene 38f1 executed!", -1);
                }
                else if (strcmp(scene_id, "40") == 0) {
                    board.ExecuteScene40();
                    httpd_resp_send(req, "🎬 Scene 40 executed!", -1);
                }
                else {
                    httpd_resp_send(req, "❌ Unknown scene ID", -1);
                }
                return ESP_OK;
            }
        }
        
        httpd_resp_send(req, "❌ Missing scene ID parameter", -1);
        return ESP_OK;
    }

    // Scene stop handler
    static esp_err_t scene_stop_handler(httpd_req_t *req) {
        ESP_LOGI(TAG, "Scene stop requested!");
        scene_stop_requested = true;
        stop_all_motors(); // Immediately stop motors
        httpd_resp_send(req, "⏹️ Scene stopped!", -1);
        return ESP_OK;
    }

    static esp_err_t emotion_handler(httpd_req_t *req) {
        char query[100];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char emotion[50];
            if (httpd_query_key_value(query, "type", emotion, sizeof(emotion)) == ESP_OK) {
                auto display = Board::GetInstance().GetDisplay();
                display->SetEmotion(emotion);
                
                char response[100];
                snprintf(response, sizeof(response), "😊 Santa emotion changed to: %s", emotion);
                httpd_resp_send(req, response, -1);
                return ESP_OK;
            }
        }
        httpd_resp_send(req, "❌ Invalid emotion parameter", -1);
        return ESP_OK;
    }

    static esp_err_t stop_handler(httpd_req_t *req) {
        httpd_resp_send(req, "🔴 Control panel closing...", -1);
        
        // Schedule server stop
        auto& board = static_cast<HeySantaBoard&>(Board::GetInstance());
        board.stop_control_webserver();
        
        return ESP_OK;
    }

public:
    void dance() {
        for (int i = 1; i <= 3; i++) {
            uint32_t head_mode = unbiasedRandom3();
            uint32_t hip_mode = unbiasedRandom3();
            
            // Head movement
            int head_speeds[3] = {87, 93, 100};
            set_motor_A_speed(head_speeds[head_mode]);
            vTaskDelay(unbiasedRandomRange(1500, 5000) / portTICK_PERIOD_MS);
            set_motor_A_speed(0);
            vTaskDelay(unbiasedRandomRange(150, 1000) / portTICK_PERIOD_MS);
            
            // Hip movement
            for (int j = 0; j < 3; j++) {
                int hip_speeds[3] = {90, 95, 100};
                set_motor_B_speed(hip_speeds[hip_mode]);
                vTaskDelay(150 / portTICK_PERIOD_MS);
                set_motor_B_speed(-hip_speeds[hip_mode]);
                vTaskDelay(150 / portTICK_PERIOD_MS);
            }
            set_motor_B_speed(0);
        }
    }

    void HeadShakeOnly() {
        ESP_LOGI(TAG, "Head shake!");
        for (int i = 0; i < 10; i++) {
            set_motor_A_speed(100);
            vTaskDelay(80 / portTICK_PERIOD_MS);
            set_motor_A_speed(-100);
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
        set_motor_A_speed(0);
    }

    void HipShakeOnly() {
        ESP_LOGI(TAG, "Hip shake!");
        for (int i = 0; i < 12; i++) {
            set_motor_B_speed(100);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            set_motor_B_speed(0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            set_motor_B_speed(-100);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            set_motor_B_speed(0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
    }

    void start_control_webserver(void) {
        if (control_server != NULL) return;

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.stack_size = 8192;
        config.max_uri_handlers = 8;  // Only need 5 handlers now!
        
        if (httpd_start(&control_server, &config) == ESP_OK) {
            httpd_uri_t handlers[] = {
                {"/", HTTP_GET, control_page_handler, NULL},
                {"/scene", HTTP_GET, scene_handler, NULL},        // Single handler for ALL scenes
                {"/scene-stop", HTTP_GET, scene_stop_handler, NULL},
                {"/emotion", HTTP_GET, emotion_handler, NULL},
                {"/stop", HTTP_GET, stop_handler, NULL}
            };
            
            for (auto& handler : handlers) {
                if (httpd_register_uri_handler(control_server, &handler) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to register handler for %s", handler.uri);
                }
            }
            
            web_server_active = true;
            ESP_LOGI(TAG, "🎬 Santa scene control web server started!");
        }
    }

    void stop_control_webserver(void) {
        if (control_server != NULL) {
            httpd_stop(control_server);
            control_server = NULL;
            web_server_active = false;
            ESP_LOGI(TAG, "Santa control web server stopped");
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button pressed!");
            
            if (!WifiStation::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "WiFi not connected, cannot start web server");
                return;
            }
            
            ESP_LOGI(TAG, "WiFi is connected, starting web server...");
            start_control_webserver();
            
            // Get IP and show notification
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(netif, &ip_info);
            
            ESP_LOGI(TAG, "🎅 Santa Control Panel: http://" IPSTR ":8080", IP2STR(&ip_info.ip));
        });

        wake_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Wake button pressed!");
            
            // Check if web server is already running
            if (web_server_active) {
                ESP_LOGI(TAG, "Web server is running, stopping it...");
                stop_control_webserver();
                HeadShakeOnly(); // Indicate server stopped
            } else {
                // Start web server
                if (!WifiStation::GetInstance().IsConnected()) {
                    ESP_LOGI(TAG, "WiFi not connected, cannot start web server");
                    HeadShakeOnly(); // Indicate error
                    return;
                }
                
                ESP_LOGI(TAG, "Starting web server...");
                start_control_webserver();
                
                if (web_server_active) {
                    // Get IP and show notification
                    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    esp_netif_ip_info_t ip_info;
                    esp_netif_get_ip_info(netif, &ip_info);
                    
                    ESP_LOGI(TAG, "🎅 Santa Control Panel: http://" IPSTR ":8080", IP2STR(&ip_info.ip));
                    
                    // Success dance
                    for (int i = 0; i < 3; i++) {
                        set_motor_A_speed(80);
                        vTaskDelay(200 / portTICK_PERIOD_MS);
                        set_motor_A_speed(-80);
                        vTaskDelay(200 / portTICK_PERIOD_MS);
                    }
                    set_motor_A_speed(0);
                } else {
                    HeadShakeOnly(); // Indicate error
                }
            }
        });
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        
        display_ = new anim::EmojiWidget(panel, panel_io);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_5;
        config.ledc_timer = LEDC_TIMER_1;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

public:
    HeySantaBoard() : boot_button_(BOOT_BUTTON_GPIO), wake_button_(WAKE_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeCamera();
        Initialize_Motor();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static HeySantaCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(HeySantaBoard);