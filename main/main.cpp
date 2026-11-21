#include <string>
#include <sys/lock.h>

/* InfluxDB thermometer */

static const char *tag = "InfluxDB Thermometer";

#include "sdkconfig.h"

// The following defines are set with idf.py menuconfig:
//#define CONFIG_TARGET_SOC_ESP32C3_SUPERMINI
//#define CONFIG_TARGET_SOC_WAVESHARE_ESP32_C3_ZERO
//#define CONFIG_TARGET_SOC_ESP32C3_72X40OLED
//#define CONFIG_TARGET_SOC_NAME

#ifdef CONFIG_TARGET_SOC_ESP32C3_72X40OLED
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
//#include "esp_lvgl_port.h"
#include "lvgl.h"
#define SDA_PIN 5
#define SCL_PIN 6
#define LCD_I2C_ADDR       0x3c
#define LCD_PIXEL_CLOCK_HZ    (50 * 1000)
#define LCD_I2C_TRANSACTION_TIMEOUT_MS 1000
#define LCD_H_RES           128
#define LCD_V_RES           64
#define LCD_OFFSET_X        28
#define LCD_OFFSET_Y        24
#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8

#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_STACK_SIZE   (4 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_PALETTE_SIZE      8
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

lv_obj_t * temperaturLabel;

#endif

#ifdef CONFIG_TARGET_SOC_ESP32C3_SUPERMINI
#define SDA_PIN 3
#define SCL_PIN 4
#endif

#ifdef CONFIG_TARGET_SOC_WAVESHARE_ESP32_C3_ZERO
#define SDA_PIN 0
#define SCL_PIN 1
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

//#include "driver/rtc_io.h"

#include "onboard_led.hpp"
#include "wifi_manager.hpp"
#include "time_sync.hpp"
#include "i2c_master.hpp"
#include "deep_sleep.hpp"
RTC_DATA_ATTR int bootCount = 0;
#define DEVICE "ESP32"
#include "InfluxDbClient.h"
#include "http_config_server.hpp"

OnBoardLed *onBoardLed;

static void timeTask(void *pc){
    TimeSync* timeSync = &TimeSync::getInstance();

    // Synchronize time
    timeSync->obtainTime();

    while (1)
    {
        timeSync->printCalendar();
        vTaskDelay(pdMS_TO_TICKS(timeSync->getSyncIntervalMs())); // Print calendar every 5 minutes
    }
}

#ifdef CONFIG_TARGET_SOC_ESP32C3_72X40OLED
// To use LV_COLOR_FORMAT_I1, we need an extra buffer to hold the converted data
static uint8_t oled_buffer[LCD_H_RES * LCD_V_RES / 8];
// LVGL library is not thread-safe, this example will call LVGL APIs from different tasks, so use a mutex to protect it
static _lock_t lvgl_api_lock;

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) lv_display_get_user_data((lv_display_t *)disp);

    // This is necessary because LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as a palette. Skip the palette here
    // More information about the monochrome, please refer to https://docs.lvgl.io/master/main-modules/display/color_format.html#monochrome-displays
    px_map += LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            /* The order of bits is MSB first
                        MSB           LSB
               bits      7 6 5 4 3 2 1 0
               pixels    0 1 2 3 4 5 6 7
                        Left         Right
            */
            bool chroma_color = (px_map[(hor_res >> 3) * y  + (x >> 3)] & 1 << (7 - x % 8));

            /* Write to the buffer as required for the display.
            * It writes only 1-bit for monochrome displays mapped vertically.*/
            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + (x);
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));
            } else {
                (*buf) |= (1 << (y % 8));
            }
        }
    }
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(tag, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

constexpr uint32_t CANVAS_W = 72;
constexpr uint32_t CANVAS_H = 40;

void draw_white_rect_on_black_canvas(void)
{
    /* 1. Define and initialize the static draw buffer */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    LV_DRAW_BUF_DEFINE_STATIC(draw_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
#pragma GCC diagnostic pop
    LV_DRAW_BUF_INIT_STATIC(draw_buf);

    /* 2. Create the canvas and attach the buffer */
    lv_obj_t * canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas, &draw_buf);
    lv_obj_center(canvas);

    /* 3. Fill the canvas background with black */
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    /* 4. Initialize a drawing layer on the canvas */
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /* 5. Configure the rectangle draw descriptor */
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);

    /* Make the rectangle fill transparent (or set to lv_color_black()) */
    rect_dsc.bg_opa = LV_OPA_TRANSP;

    /* Set the white border properties */
    rect_dsc.border_color = lv_color_white();
    rect_dsc.border_width = 2;
    rect_dsc.border_opa = LV_OPA_COVER;
    rect_dsc.radius = 4; /* Set > 0 for rounded corners */

    /* 6. Define rectangle coordinates relative to the canvas */
    lv_area_t coords = {
        .x1 = 2,
        .y1 = 2,
        .x2 = 69,
        .y2 = 37
    };

    /* 7. Draw the rectangle and flush the layer */
    lv_draw_rect(&layer, &rect_dsc, &coords);
    lv_canvas_finish_layer(canvas, &layer);
}

void example_lvgl_demo_ui(lv_display_t *disp)
{
    draw_white_rect_on_black_canvas();

    // Label auf dem aktiven Bildschirm erstellen
    temperaturLabel = lv_label_create(lv_screen_active());

    // Text setzen (kopiert den String in den dynamischen Speicher)
    lv_label_set_text(temperaturLabel, "");

    // Schriftfarbe auf Weiß setzen
    lv_obj_set_style_text_color(temperaturLabel, lv_color_white(), LV_PART_MAIN);

    // Schriftart auf Montserrat 20 setzen
    lv_obj_set_style_text_font(temperaturLabel, &lv_font_montserrat_28, LV_PART_MAIN);

    // Label mittig auf dem Bildschirm platzieren
    lv_obj_set_pos(temperaturLabel, 28+2, 16);
}
#endif

extern "C" void app_main(void)
{
    ESP_LOGI(tag, "Start");

    onBoardLed = new OnBoardLed(
	    #if defined(CONFIG_TARGET_SOC_WAVESHARE_ESP32_C3_ZERO)
		    std::string("Waveshare ESP32-C3-Zero onBoardLed"),
		    (gpio_num_t) 10,
		    std::string("RGB"),
		    std::string("RMT"),
		    LED_MODEL_WS2812,
        #elif defined(CONFIG_TARGET_SOC_ESP32C3_SUPERMINI)
		    std::string("ESP32-C3 w/ LCD display onBoardLed"),
		    (gpio_num_t) 8,
            (uint8_t) 0, // activeLevel
	    #elif defined(CONFIG_TARGET_SOC_ESP32C3_72X40OLED)
		    std::string("ESP32-C3 Supermini onBoardLed"),
		    (gpio_num_t) 8,
            (uint8_t) 0, // activeLevel
	    #endif
		500);
	onBoardLed->setLedPixelColor(0, 0, 0, 16);
    onBoardLed->setLedState(true);
    onBoardLed->show();

    /* Initialize DeepSleep class */
    ESP_LOGI(tag, "DeepSleep");
    DeepSleep deepSleep(
		std::string("DeepSleep"), // tag
		&bootCount // Address of int bootCount in RTC_DATA
    );

    ESP_LOGI(tag, "EnableTimerWakeup");
    ESP_ERROR_CHECK(deepSleep.EnableTimerWakeup(15, "min"));  // enable wake up after 15 minutes sleep time

    Wifi wifi( std::string("WifiManager"), // tag for ESP_LOGx
               std::string("ESP32"),       // ssid_prefix for configuration access point
               std::string("de-DE")        // language for configuration access point
             );

    if(!wifi.IsConnected()) {
        onBoardLed->setLedState(false);
        onBoardLed->show();

        ESP_LOGI(tag, "WiFi is not connected -> GoToDeepSleep");
        deepSleep.GoToDeepSleep(); // go to deep sleep
    }

    // get ConfigServer instance
    HttpConfigServer* configServer = &HttpConfigServer::getInstance();

    configServer->initialize( std::string("**** HttpServerConfig ****"), // tag for ESP_LOGx
                              wifi.GetIpAddress(), // IP address
                              std::string("config") // nvs namespace
                            );

    // add config parameter "influxdbUrl"
    configServer->addStringParameter( "01_Url",
                                     "InfluxDB server url. Don't use localhost, always server name or ip address. E.g. http://192.168.1.48:8086 (In InfluxDB 2 UI -> Load Data -> Client Libraries)"
                                   );
    // add config parameter "influxdbToken"
    configServer->addStringParameter( "02_Token",
                                     "InfluxDB 2 server or cloud API authentication token (Use: InfluxDB UI -> Load Data -> Tokens -> <select token>)"
                                   );
    // add config parameter "influxdbOrg"
    configServer->addStringParameter( "03_Org",
                                     "InfluxDB 2 organization id (Use: InfluxDB UI -> Settings -> Profile -> <name under tile> )"
                                   );
    // add config parameter "influxdbBucket"
    configServer->addStringParameter( "04_Bucket",
                                     "InfluxDB 2 bucket name (Use: InfluxDB UI -> Load Data -> Buckets)"
                                   );

    // add config parameter "thermometerCalibration"
    configServer->addStringParameter( "05_Calibration",
                                     "Calibration of thermometer (float value to shift temperature)"
                                   );

    // isConfigured()
    // connect to nvs_flash
    // if config parameters are already set in nvs_flash -> return true
    // else start http server to set config parameter values and return false
    while (!configServer->isConfigured()) {
        ESP_LOGI(tag, "HttpConfigServer is not yet configured, go to http://%s/config to enter configuration parameter values", wifi.GetIpAddress().c_str());
        vTaskDelay(pdMS_TO_TICKS(10000)); // delay 10 seconds
    }

    // get config parameter values
    ESP_LOGI(tag, "get config parameter values from HttpConfigServer");
    esp_err_t ret;
    std::string influxdbUrl = configServer->getStringParameterValue("01_Url", &ret);
    std::string influxdbToken = configServer->getStringParameterValue("02_Token", &ret);
    std::string influxdbOrg = configServer->getStringParameterValue("03_Org", &ret);
    std::string influxdbBucket = configServer->getStringParameterValue("04_Bucket", &ret);
    std::string thermometerCalibration = configServer->getStringParameterValue("05_Calibration", &ret);

    ESP_LOGI(tag, "influxdbUrl: %s", influxdbUrl.c_str());
    ESP_LOGI(tag, "influxdbToken: %s", influxdbToken.c_str());
    ESP_LOGI(tag, "influxdbOrg: %s", influxdbOrg.c_str());
    ESP_LOGI(tag, "influxdbBucket: %s", influxdbBucket.c_str());
    ESP_LOGI(tag, "thermometerCalibration: %s", thermometerCalibration.c_str());

    /* Initialize TimeSync class */
    ESP_LOGI(tag, "TimeSync");
    TimeSync* timeSync = &timeSync->getInstance();
    timeSync->initializeSntp();
    timeSync->setTimezone(std::string("CET"));
    timeSync->setSyncIntervalMs(300000);

    xTaskCreate(timeTask, "time_task", 4096, NULL, 5, NULL);

    while(!timeSync->isSynchronized()) {
        ESP_LOGI(tag, "time is not yet synchronized");
        vTaskDelay(pdMS_TO_TICKS(1000)); // delay 1 second
    }

    ESP_LOGI(tag, "SoC Board: %s", CONFIG_TARGET_SOC_NAME);
    ESP_LOGI(tag, "I2C: SDA = %d, SCL = %d", SDA_PIN, SCL_PIN);

    /* First configure the I2C Master Bus */
    ESP_LOGI(tag, "I2cMaster");
    I2cMaster i2c(
	    std::string("I2C Master Bus"), // tag
	    (i2c_port_num_t) 0, // I2C_MASTER_NUM, // i2cPort
	    (gpio_num_t) SCL_PIN, // sclPin
	    (gpio_num_t) SDA_PIN, // sdaPin
	    (uint32_t) 0 //transQueueDepth = 0: synchronous mode
    );

#ifdef CONFIG_TARGET_SOC_ESP32C3_72X40OLED
    // add the LCD Panel device to the I2C Master Bus
    ESP_LOGI(tag, "I2cDevice LCD");

    //I2cDevice lcdDevice(
    //    std::string("LCD Device"), // tag
    //    std::string("LCD"), // deviceName
    //    (i2c_addr_bit_len_t) I2C_ADDR_BIT_LEN_7, // devAddrLength
    //    (uint16_t) OLED_I2C_ADDR, // deviceAddress (without R/W bit)
    //    (uint32_t) 400000 // sclSpeedHz
    //    );
    //i2c_master_dev_handle_t lcdHandle = i2c.AddDevice(&lcdDevice);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = LCD_I2C_ADDR;    // Standard-I2C-Adresse für SSD1306
    io_config.scl_speed_hz = LCD_PIXEL_CLOCK_HZ;   // 400 kHz
    io_config.transaction_timeout_ms = LCD_I2C_TRANSACTION_TIMEOUT_MS;
    io_config.control_phase_bytes = 1;     // SSD1306 nutzt 1 Control-Byte
    io_config.dc_bit_offset = 6;           // D/C Bit liegt an Position 6
    io_config.lcd_cmd_bits = LCD_CMD_BITS;
    io_config.lcd_param_bits = LCD_CMD_BITS;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c.GetHandle(), &io_config, &io_handle));

    vTaskDelay(pdMS_TO_TICKS(1000)); // wait for 1 sec

    ESP_ERROR_CHECK(i2c_master_probe(i2c.GetHandle(), LCD_I2C_ADDR, 1000));

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xAE, NULL, 0));      // Display OFF
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xD5, (uint8_t[]){0xF0}, 1)); // Set Display Clock Divide Ratio / Oscillator Frequency (0x80 -> ~171 Hz)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA8, (uint8_t[]){0x27}, 1)); // Set Multiplex Ratio (40MUX -> 0x27 = 39)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xD3, (uint8_t[]){0x0c}, 1)); // Set Display Offset (14)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x40, NULL, 0));      // Set Display Start Line (0)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xAD, (uint8_t[]){0x30}, 1)); // Internal IREF Setting (wichtig für Helligkeit bei 0.42" Panels)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x8D, (uint8_t[]){0x10}, 1)); // Charge Pump Enable
    //ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x20, (uint8_t[]){0x00}, 1)); // Memory Addressing Mode (Horizontal)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA1, NULL, 0));      // Segment Re-map (Spalte 127 auf SEG0)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC8, NULL, 0));      // COM Output Scan Direction (remapped)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xDA, (uint8_t[]){0x12}, 1)); // COM Pins Hardware Configuration
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x81, (uint8_t[]){0xFF}, 1)); // Set Contrast Control
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xD9, (uint8_t[]){0x22}, 1)); // Set Pre-charge Period
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xDB, (uint8_t[]){0x20}, 1)); // Set VCOMH Deselect Level
    //ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA4, NULL, 0));      // Entire Display ON (RAM-Inhalt anzeigen)
    //ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA6, NULL, 0));      // Normal Display (nicht invertiert)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA7, NULL, 0));      // Invertiertes Display
    //ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xA5, NULL, 0));      // all white
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xAF, NULL, 0));      // Display ON

    // create SSD1306 Panel driver
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.bits_per_pixel = 1;
    panel_config.reset_gpio_num = (gpio_num_t) -1; // no Reset-Pin connected
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {};
    ssd1306_config.height = LCD_V_RES;
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Set Offset for 72x40 window
    //ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, LCD_OFFSET_X, LCD_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(tag, "Initialize LVGL");
    lv_init();
    // create a lvgl display
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    // associate the i2c panel handle to the display
    lv_display_set_user_data(display, panel_handle);
    // create draw buffer
    void *buf = NULL;
    ESP_LOGI(tag, "Allocate separate LVGL draw buffers");
    // LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as a palette.
    size_t draw_buffer_sz = LCD_H_RES * LCD_V_RES / 8 + LVGL_PALETTE_SIZE;
    buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    // LVGL9 suooprt new monochromatic format.
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    // initialize LVGL draw buffers
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    // set the callback which can copy the rendered image to an area of the display
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    ESP_LOGI(tag, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = example_notify_lvgl_flush_ready,
    };
    /* Register done callback */
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

    ESP_LOGI(tag, "Use esp_timer as LVGL tick timer");
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(tag, "Create LVGL task");
    xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(tag, "Display LVGL Scroll Text");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    _lock_acquire(&lvgl_api_lock);
    example_lvgl_demo_ui(display);
    _lock_release(&lvgl_api_lock);
#endif

// ab hier prüfen!

//    // Initialize LVGL port
//    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
//    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
//
//    // Register Display in LVGL v9
//    lvgl_port_display_cfg_t disp_cfg = {};
//    disp_cfg.io_handle = io_handle;
//    disp_cfg.panel_handle = panel_handle;
//    disp_cfg.buffer_size = LCD_H_RES * LCD_V_RES;
//    disp_cfg.double_buffer = false;
//    disp_cfg.hres = LCD_H_RES;
//    disp_cfg.vres = LCD_V_RES;
//    disp_cfg.monochrome = true; // Aktiviert automatische 1-Bit-Konvertierung für SSD1306
//    disp_cfg.color_format = LV_COLOR_FORMAT_I1;
//    disp_cfg.rotation.swap_xy = false;
//    disp_cfg.rotation.mirror_x = false;
//    disp_cfg.rotation.mirror_y = false;
//    //disp_cfg.flags.buff_dma = false;
//    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
//

//    // create Test UI (Thread safe with Lock)
//    _lock_acquire(&lvgl_api_lock);
//    if (lvgl_api_lock) {
//        lv_theme_t *th = lv_theme_mono_init(display, true, LV_FONT_DEFAULT);
//        lv_display_set_theme(display, th);
//
//        lv_obj_t *label = lv_label_create(lv_screen_active());
//        lv_label_set_text(label, "LVGL 9\nI2C v2");
//        _lock_release(&lvgl_api_lock); //lvgl_port_unlock();
//    }

    // add the MCP9808 thermometer device to the I2C Master Bus
    ESP_LOGI(tag, "I2cDevice Thermometer Device");
    i2c_master_dev_handle_t thermometerHandle =
       i2c.AddDevice(new I2cDevice(
           std::string("thermometerDevice"), // tag
           std::string("Thermometer"), // deviceName
           (i2c_addr_bit_len_t) I2C_ADDR_BIT_LEN_7, // devAddrLength
           (uint16_t) 0x18, // deviceAddress
           (uint32_t) 50000 // sclSpeedHz
           )
       );

    ESP_ERROR_CHECK(i2c_master_probe(i2c.GetHandle(), 0x18, 100));

   float temperature = 0.0;
    uint8_t set_resolution_buffer[2] = {0x08, 0x03};
    uint8_t request_temperature_buffer = 0x05;
    uint8_t temperature_buffer[2];

    //mcp9808
    // Set resolution to 0.0625°C
    ESP_ERROR_CHECK(i2c_master_transmit(thermometerHandle, set_resolution_buffer, 2, -1));

    ESP_LOGI(tag, "Start InfluxDB client");
    // InfluxDB client instance
    InfluxDBClient client;
    client.setConnectionParams(influxdbUrl.c_str(), influxdbOrg.c_str(), influxdbBucket.c_str(), influxdbToken.c_str());

    while(!client.validateConnection()) {
        ESP_LOGI(tag, "InfluxDB connection failed: %s\n", client.getLastErrorMessage().c_str());
        ESP_LOGI(tag, "wait 1 sec. for InfluxDB client connection");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Data point
    Point sensor("temperature");

    // Add constant tags - only once
    sensor.addTag("ip_address", wifi.GetIpAddress().c_str());

    char devAddrBuffer[10];
    sprintf(devAddrBuffer, "0x%2x", i2c.GetDevice("Thermometer")->GetConfig().device_address);
    sensor.addTag("device_address", devAddrBuffer);

    // overwrite value of Parameter 05_Calibration for MCP9808 devices with well-known IP addresses
    std::string currentCalibration = thermometerCalibration;
    if(wifi.GetIpAddress().compare("192.168.178.150") == 0) { //Wohnzimmer ESP32C3 Supermini
        thermometerCalibration = "0.2";
    }
    if(wifi.GetIpAddress().compare("192.168.178.145") == 0) { //Schlafzimmer Waveshare ESP32C3 Zero
        thermometerCalibration = "0.4";
    }
    if(wifi.GetIpAddress().compare("192.168.178.116") == 0) { // LCD ESP32C3 72x40 OLED
        thermometerCalibration = "0.0";
    }
    if(wifi.GetIpAddress().compare("192.168.178.166") == 0) { // LCD ESP32C3 72x40 OLED
        thermometerCalibration = "-0.1";
    }
    if(currentCalibration.compare(thermometerCalibration) != 0) {
        configServer->setStringParameterValue("05_Calibration", thermometerCalibration);
    }

    // Check server connection
    if (client.validateConnection()) {
        ESP_LOGI(tag, "connected to InfluxDB: %s", client.getServerUrl().c_str());
    } else {
        ESP_LOGI(tag, "InfluxDB connection failed: %s", client.getLastErrorMessage().c_str());
    }

    // Read the temperature from MCP9808
    ESP_ERROR_CHECK(i2c_master_transmit_receive(thermometerHandle, &request_temperature_buffer, 1, temperature_buffer, 2, -1));
    // wait a moment and read temperature again, because the first measurement is always wrong
    vTaskDelay(1000 / portTICK_PERIOD_MS); // wait for 1 second
    // Read the temperature from MCP9808
    ESP_ERROR_CHECK(i2c_master_transmit_receive(thermometerHandle, &request_temperature_buffer, 1, temperature_buffer, 2, -1));

    temperature_buffer[0] = temperature_buffer[0] & 0x1F; //Clear flag bits
    if ((temperature_buffer[0] & 0x10) == 0x10){ //TA < 0°C
        temperature_buffer[0] = temperature_buffer[0] & 0x0F; //Clear SIGN
        temperature = 256. - (temperature_buffer[0] * 16. + temperature_buffer[1] / 16.);
    } else //TA > 0°C
    temperature = (temperature_buffer[0] * 16. + temperature_buffer[1] / 16.);
    //Temperature = Ambient Temperature (°C)

    // use calibration value to adjust the measured temperature
    float calibrationOffset = 0.0;
    float correctedTemperature = temperature;

    if(thermometerCalibration != "") {
        calibrationOffset = std::stof(thermometerCalibration.c_str());
        correctedTemperature = temperature + calibrationOffset;
    }

    ESP_LOGI(tag, "Temperature: measured: %f, corrected: %f", temperature, correctedTemperature);

    char buffer[1000];
    sprintf(buffer, "%8.1f", correctedTemperature);

    // Store measured value into point
    sensor.clearFields();
    // Report RSSI of currently connected network
    sensor.addField("temperature", correctedTemperature);
    // Print what are we exactly writing

#ifdef CONFIG_TARGET_SOC_ESP32C3_72X40OLED
    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text(temperaturLabel, &buffer[3]);
    _lock_release(&lvgl_api_lock);
#endif

    ESP_LOGI(tag, "writing: %s", client.pointToLineProtocol(sensor).c_str());
    // If no Wifi signal, try to reconnect it
    if (!wifi.IsConnected()) {
        ESP_LOGI(tag, "Wifi connection lost");
        wifi.RestartStation();
        // Check server connection
        if (client.validateConnection()) {
          ESP_LOGI(tag, "(Re-)Connected to InfluxDB: %s", client.getServerUrl().c_str());
        } else {
          ESP_LOGI(tag, "InfluxDB connection failed: %s", client.getLastErrorMessage().c_str());
        }
    }
    // Write point
    if (!client.writePoint(sensor)) {
        ESP_LOGI(tag, "InfluxDB write failed: %s", client.getLastErrorMessage().c_str());
    }

    onBoardLed->setLedState(false);
    onBoardLed->show();

    ESP_LOGI(tag, "GoToDeepSleep");
    deepSleep.GoToDeepSleep(); // go to deep sleep
}
  
