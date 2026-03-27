#include "wifi_board.h"
#include "codecs/es8388_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "led/gpio_led.h"
#include "mcp_server.h"
#include "esp_video.h"
#include "settings.h"
#include "sdkconfig.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cassert>
#include <memory>

#define TAG "atk_dnesp32s3"
#define DEBUG_OTA_ENABLED_KEY "dbg_ota_en"

class XL9555 : public I2cDevice {
public:
    XL9555(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x06, 0x03);
        WriteReg(0x07, 0xF0);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint16_t data;
        int index = bit;

        if (bit < 8) {
            data = ReadReg(0x02);
        } else {
            data = ReadReg(0x03);
            index -= 8;
        }

        data = (data & ~(1 << index)) | (level << index);

        if (bit < 8) {
            WriteReg(0x02, data);
        } else {
            WriteReg(0x03, data);
        }
    }

    bool GetInputState(uint8_t bit) {
        uint8_t data;
        int index = bit;

        if (bit < 8) {
            data = ReadReg(0x00);
        } else {
            data = ReadReg(0x01);
            index -= 8;
        }

        return ((data >> index) & 0x01) != 0;
    }
};

struct Xl9555ButtonDriver {
    button_driver_t base;
    uint8_t bit;
};

class atk_dnesp32s3 : public WifiBoard {
private:
    static constexpr uint8_t XL9555_KEY0_BIT = 15; // io1_7
    static constexpr uint8_t XL9555_KEY1_BIT = 14; // io1_6
    static constexpr uint8_t XL9555_KEY2_BIT = 13; // io1_5
    static atk_dnesp32s3* instance_;

    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    std::unique_ptr<Button> key0_button_;
    std::unique_ptr<Button> key1_button_;
    std::unique_ptr<Button> key2_button_;
    button_driver_t* key0_button_driver_ = nullptr;
    button_driver_t* key1_button_driver_ = nullptr;
    button_driver_t* key2_button_driver_ = nullptr;
    LcdDisplay* display_;
    XL9555* xl9555_;
    EspVideo* camera_;
    bool led_on_ = false;

    void ClearNvsAndReboot() {
        auto* display = GetDisplay();
        if (display != nullptr) {
            display->ShowNotification("正在清除NVS...");
        }

        xTaskCreate([](void* arg) {
            (void)arg;
            ESP_LOGW(TAG, "BOOT long press detected, clearing NVS");

            esp_err_t ret = nvs_flash_erase();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
            } else {
                ret = nvs_flash_init();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to reinitialize NVS: %s", esp_err_to_name(ret));
                }
            }

            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "NVS cleared successfully, rebooting");
            }

            Application::GetInstance().Reboot();
            vTaskDelete(nullptr);
        }, "clear_nvs", 4096, this, 2, nullptr);
    }

    void ToggleDebugOtaAndReboot() {
#if CONFIG_USE_DEBUG_OTA_TOGGLE
        if (sizeof(CONFIG_DEBUG_OTA_URL) <= 1) {
            auto* display = GetDisplay();
            if (display != nullptr) {
                display->ShowNotification("未配置调试OTA地址");
            }
            ESP_LOGW(TAG, "Debug OTA toggle requested but CONFIG_DEBUG_OTA_URL is empty");
            return;
        }

        Settings settings("wifi", true);
        bool enabled = settings.GetBool(DEBUG_OTA_ENABLED_KEY, false);
        settings.SetBool(DEBUG_OTA_ENABLED_KEY, !enabled);

        auto* display = GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(!enabled ? "调试OTA已开启，正在重启" : "调试OTA已关闭，正在重启");
        }

        xTaskCreate([](void* arg) {
            (void)arg;
            vTaskDelay(pdMS_TO_TICKS(1200));
            Application::GetInstance().Reboot();
            vTaskDelete(nullptr);
        }, "debug_ota_reboot", 4096, this, 2, nullptr);
#else
        ESP_LOGI(TAG, "Debug OTA toggle is disabled by sdkconfig");
#endif
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize XL9555
        xl9555_ = new XL9555(i2c_bus_, 0x20);
    }

    // Initialize spi peripheral
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = LCD_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = LCD_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    static void CreateXl9555Button(uint8_t bit, button_driver_t*& driver, std::unique_ptr<Button>& button,
        std::function<void()> on_click) {
        button_config_t button_config = {
            .long_press_time = 0,
            .short_press_time = 0,
        };

        auto* xl9555_button_driver = static_cast<Xl9555ButtonDriver*>(calloc(1, sizeof(Xl9555ButtonDriver)));
        assert(xl9555_button_driver != nullptr);
        xl9555_button_driver->bit = bit;

        driver = &xl9555_button_driver->base;
        driver->enable_power_save = false;
        driver->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            auto* xl9555_button_driver = reinterpret_cast<Xl9555ButtonDriver*>(button_driver);
            return (instance_ != nullptr && !instance_->xl9555_->GetInputState(xl9555_button_driver->bit)) ? 1 : 0;
        };
        driver->del = [](button_driver_t* button_driver) -> esp_err_t {
            free(button_driver);
            return ESP_OK;
        };

        button_handle_t button_handle = nullptr;
        ESP_ERROR_CHECK(iot_button_create(&button_config, driver, &button_handle));
        button = std::make_unique<Button>(button_handle);
        button->OnClick(std::move(on_click));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this] {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this] {
            ClearNvsAndReboot();
        });

        CreateXl9555Button(XL9555_KEY0_BIT, key0_button_driver_, key0_button_, [this] {
            SetLedPower(!led_on_);
            ESP_LOGI(TAG, "key0 demo led: %s", led_on_ ? "on" : "off");
        });

        CreateXl9555Button(XL9555_KEY1_BIT, key1_button_driver_, key1_button_, [] {
            Application::GetInstance().ToggleRecordingState();
        });

#if CONFIG_USE_DEBUG_OTA_TOGGLE
        CreateXl9555Button(XL9555_KEY2_BIT, key2_button_driver_, key2_button_, [this] {
            ToggleDebugOtaAndReboot();
        });
#endif
    }

    GpioLed* GetBoardLed() {
        return dynamic_cast<GpioLed*>(GetLed());
    }

    bool SetLedPower(bool on) {
        auto* led = GetBoardLed();
        if (led == nullptr) {
            ESP_LOGW(TAG, "GpioLed unavailable");
            return false;
        }

        led->SetManualPower(on, 100);
        led_on_ = on;
        return true;
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.light.get_power",
            "Get the current power state of the light.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                cJSON* result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "power", led_on_);
                return result;
            });

        mcp_server.AddTool("self.light.set_power",
            "Set the power state of the light. Use true to turn it on and false to turn it off.",
            PropertyList({
                Property("power", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool power = properties["power"].value<bool>();
                if (!SetLedPower(power)) {
                    throw std::runtime_error("Failed to access board LED");
                }
                return true;
            });
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_LOGD(TAG, "Install panel IO");
        // 液晶屏控制IO初始化
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS_PIN;
        io_config.dc_gpio_num = LCD_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 7;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io);

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
        
        esp_lcd_panel_reset(panel);
        xl9555_->SetOutputState(8, 1);
        xl9555_->SetOutputState(2, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY); 
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 初始化摄像头：ov2640；
    // 根据正点原子官方示例参数
    void InitializeCamera() {
        xl9555_->SetOutputState(OV_PWDN_IO, 0); // PWDN=低 (上电)
        xl9555_->SetOutputState(OV_RESET_IO, 0); // 确保复位
        vTaskDelay(pdMS_TO_TICKS(50));           // 延长复位保持时间
        xl9555_->SetOutputState(OV_RESET_IO, 1); // 释放复位
        vTaskDelay(pdMS_TO_TICKS(50));           // 延长 50ms

        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAM_PIN_D0,
                [1] = CAM_PIN_D1,
                [2] = CAM_PIN_D2,
                [3] = CAM_PIN_D3,
                [4] = CAM_PIN_D4,
                [5] = CAM_PIN_D5,
                [6] = CAM_PIN_D6,
                [7] = CAM_PIN_D7,
            },
            .vsync_io = CAM_PIN_VSYNC,
            .de_io = CAM_PIN_HREF,
            .pclk_io = CAM_PIN_PCLK,
            .xclk_io = CAM_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = 1,
                .scl_pin = CAM_PIN_SIOC,
                .sda_pin = CAM_PIN_SIOD,
            },
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAM_PIN_RESET,   // 实际由 XL9555 控制
            .pwdn_pin = CAM_PIN_PWDN,     // 实际由 XL9555 控制
            .dvp_pin = dvp_pin_config,
            .xclk_freq = 20000000,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

public:
    atk_dnesp32s3() : boot_button_(BOOT_BUTTON_GPIO, false, 5000) {
        instance_ = this;
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeTools();
        InitializeCamera();
    }

    virtual Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8388AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_0, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8388_ADDR
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Camera* GetCamera() override {
        return camera_;
    }
};

atk_dnesp32s3* atk_dnesp32s3::instance_ = nullptr;

DECLARE_BOARD(atk_dnesp32s3);
