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

    // Web server handlers
    static esp_err_t control_page_handler(httpd_req_t *req) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        
        const char* html = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<title>🎅 Santa Control Panel</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background: linear-gradient(135deg, #2E7D32 0%, #C62828 50%, #2E7D32 100%); color: white; min-height: 100vh; }"
        ".container { max-width: 900px; margin: 0 auto; background: rgba(255,255,255,0.15); padding: 40px; border-radius: 25px; backdrop-filter: blur(15px); box-shadow: 0 10px 40px rgba(0,0,0,0.3); }"
        "h1 { color: #fff; margin-bottom: 30px; font-size: 32px; }"
        "h2 { color: #fff; margin: 30px 0 20px 0; font-size: 24px; }"
        "button { padding: 15px 25px; margin: 8px; font-size: 16px; border: none; border-radius: 12px; cursor: pointer; min-width: 140px; transition: all 0.3s ease; font-weight: bold; }"
        ".movement-btn { background: linear-gradient(45deg, #4CAF50, #45a049); color: white; box-shadow: 0 4px 15px rgba(76, 175, 80, 0.4); }"
        ".emotion-btn { background: linear-gradient(45deg, #E91E63, #C2185B); color: white; box-shadow: 0 4px 15px rgba(233, 30, 99, 0.4); }"
        ".stop-btn { background: linear-gradient(45deg, #F44336, #D32F2F); color: white; box-shadow: 0 6px 20px rgba(244, 67, 54, 0.4); }"
        "button:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(0,0,0,0.4); }"
        ".control-section { margin: 30px 0; padding: 25px; background: rgba(255,255,255,0.1); border-radius: 20px; }"
        ".button-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 12px; margin: 20px 0; }"
        ".emotion-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; margin: 20px 0; }"
        ".status { margin: 25px 0; padding: 20px; background: rgba(255,255,255,0.1); border-radius: 15px; font-weight: bold; font-size: 18px; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>🎅 Santa Control Panel 🎅</h1>"
        
        "<div class='control-section'>"
        "<h2>🕺 Movement Controls</h2>"
        "<div class='button-grid'>"
        "<button class='movement-btn' onclick='sendCommand(\"/dance\")'>💃 DANCE</button>"
        "<button class='movement-btn' onclick='sendCommand(\"/shake-head\")'>🤖 SHAKE HEAD</button>"
        "<button class='movement-btn' onclick='sendCommand(\"/shake-hip\")'>🍑 SHAKE HIP</button>"
        "</div>"
        "</div>"
        
        "<div class='control-section'>"
        "<h2>😊 Emotion Controls</h2>"
        "<div class='emotion-grid'>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=bell\")'>🔔 Bell</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=blinking\")'>😊 Blinking</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=cookie\")'>🍪 Cookie</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=deer\")'>🦌 Deer</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=heart\")'>❤️ Heart</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=sleep\")'>😴 Sleep</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=snowman\")'>⛄ Snowman</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=star\")'>⭐ Star</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=elf\")'>🧝 Elf</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=wrong\")'>❌ Wrong</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=happy\")'>😄 Happy</button>"
        "<button class='emotion-btn' onclick='sendCommand(\"/emotion?type=neutral\")'>😐 Neutral</button>"
        "</div>"
        "</div>"
        
        "<div class='control-section'>"
        "<button class='stop-btn' onclick='stopServer()'>🛑 CLOSE CONTROL PANEL</button>"
        "</div>"
        
        "<div id='status' class='status'>🎅 Santa Control Panel Ready!</div>"
        "</div>"
        
        "<script>"
        "function sendCommand(endpoint) {"
        "  console.log('Sending command:', endpoint);"
        "  document.getElementById('status').innerText = 'Sending command...';"
        "  "
        "  fetch(endpoint)"
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

    static esp_err_t dance_handler(httpd_req_t *req) {
        auto& board = static_cast<HeySantaBoard&>(Board::GetInstance());
        board.dance();
        httpd_resp_send(req, "🕺 Santa is dancing!", -1);
        return ESP_OK;
    }

    static esp_err_t shake_head_handler(httpd_req_t *req) {
        auto& board = static_cast<HeySantaBoard&>(Board::GetInstance());
        board.HeadShakeOnly();
        httpd_resp_send(req, "🤖 Santa is shaking his head!", -1);
        return ESP_OK;
    }

    static esp_err_t shake_hip_handler(httpd_req_t *req) {
        auto& board = static_cast<HeySantaBoard&>(Board::GetInstance());
        board.HipShakeOnly();
        httpd_resp_send(req, "🍑 Santa is shaking his hips!", -1);
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
        
        if (httpd_start(&control_server, &config) == ESP_OK) {
            // Register handlers
            httpd_uri_t handlers[] = {
                {"/", HTTP_GET, control_page_handler, NULL},
                {"/dance", HTTP_GET, dance_handler, NULL},
                {"/shake-head", HTTP_GET, shake_head_handler, NULL},
                {"/shake-hip", HTTP_GET, shake_hip_handler, NULL},
                {"/emotion", HTTP_GET, emotion_handler, NULL},
                {"/stop", HTTP_GET, stop_handler, NULL}
            };
            
            for (auto& handler : handlers) {
                httpd_register_uri_handler(control_server, &handler);
            }
            
            web_server_active = true;
            ESP_LOGI(TAG, "🎅 Santa control web server started!");
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

        // MODIFIED: Use wake button for web server control
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