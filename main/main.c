
#include "esp_bt.h"
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include "esp_avrc_api.h"
#include <esp_a2dp_api.h>
#include <driver/i2s_std.h>
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/ringbuf.h"

#define AUDIO_RINGBUF_SIZE (1024 * 64) // Allocate 64KB for the buffer
static RingbufHandle_t s_audio_ringbuf = NULL;
static TaskHandle_t s_i2s_write_task_handle = NULL;

i2s_chan_handle_t tx_handle;
// dac_continuous_handle_t dac_handle = NULL;

#define I2S_BCLK_PIN GPIO_NUM_26 // Connect to PCM5102A BCK
#define I2S_WS_PIN GPIO_NUM_25   // Connect to PCM5102A LRCK
#define I2S_DO_PIN GPIO_NUM_22   // Connect to PCM5102A DIN

static esp_bd_addr_t device_bd = {0};
static bool has_bd = false;
static uint8_t s_volume = 0x4C;
static uint8_t max_volume = 0x7f;

static void init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    // 2. Setup Standard Mode Config
    i2s_std_config_t std_cfg = {
        // Set sample rate (e.g., 44100Hz for CD quality)
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),

        // Match PCM5102A communication style: Philips format, 16-bit or 32-bit Stereo
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),

        // .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),

        // Map the physical pins
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // PCM5102A generates MCLK internally! Leave unused.
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DO_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT | I2S_STD_SLOT_RIGHT; // Enable both left and right channels
    std_cfg.slot_cfg.slot_bit_width = I2S_DATA_BIT_WIDTH_32BIT; // Set slot bit width to 32 bits

    i2s_channel_init_std_mode(tx_handle, &std_cfg);
    i2s_channel_enable(tx_handle);
}

static void bt_app_a2d_data_cb(
    const uint8_t *data,
    uint32_t len)
{
    // size_t written = 0;

    // esp_err_t err = i2s_channel_write(
    //     tx_handle,
    //     data,
    //     len,
    //     &written,
    //     portMAX_DELAY);
    if (s_audio_ringbuf == NULL)
    {
        return;
    }

    BaseType_t ret = xRingbufferSend(s_audio_ringbuf, (void *)data, len, 0);

    if (ret != pdTRUE)
    {
        ESP_LOGW("BT_AUDIO", "Ring buffer full, dropping %u bytes", (unsigned)len);
    }

    // if (err != ESP_OK || written != len)
    // {
    //     ESP_LOGW(
    //         "BT_AUDIO",
    //         "I2S write incomplete: %u/%u, err=%s",
    //         (unsigned)written,
    //         (unsigned)len,
    //         esp_err_to_name(err));
    // }
}


#define AUDIO_BUFFER_MAX_SAMPLES 2048  // 16-bit stereo samples

static void i2s_write_task(void *pvParameters)
{
    size_t item_size = 0;
    size_t written = 0;

    // 16-bit stereo buffer
    int16_t *local_audio_buf16 = (int16_t *)heap_caps_malloc(
        AUDIO_BUFFER_MAX_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );

    if (local_audio_buf16 == NULL) {
        ESP_LOGE("BT_AUDIO", "Failed to allocate 16-bit audio buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int16_t *data16 = (int16_t *)xRingbufferReceive(
            s_audio_ringbuf,
            &item_size,
            portMAX_DELAY
        );

        if (data16 == NULL) {
            continue;
        }

        size_t total_samples = item_size / sizeof(int16_t);

        // Keep stereo alignment
        if (total_samples % 2 != 0) {
            total_samples--;
        }

        if (total_samples > AUDIO_BUFFER_MAX_SAMPLES) {
            total_samples = AUDIO_BUFFER_MAX_SAMPLES;
        }

         for (size_t i = 0; i < total_samples; i++) {
            int32_t sample = (int32_t)data16[i];
            // Scale and handle volume (0..127)
            local_audio_buf16[i] = (int16_t)((sample * s_volume) / 127);
        }

        vRingbufferReturnItem(s_audio_ringbuf, (void *)data16);

        size_t bytes_to_write = total_samples * sizeof(int16_t);

        esp_err_t err = i2s_channel_write(
            tx_handle,
            local_audio_buf16,
            bytes_to_write,
            &written,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            ESP_LOGW("BT_AUDIO", "I2S write error: %s", esp_err_to_name(err));
        } else if (written != bytes_to_write) {
            ESP_LOGW(
                "BT_AUDIO",
                "I2S partial write: %u/%u bytes",
                (unsigned)written,
                (unsigned)bytes_to_write
            );
        }
    }
}

static void bt_app_avrc_tg_cb(
    esp_avrc_tg_cb_event_t event,
    esp_avrc_tg_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(
            "BT_AVRCP",
            "AVRCP %s",
            param->conn_stat.connected ? "connected" : "disconnected");
        break;

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id ==
            ESP_AVRC_RN_VOLUME_CHANGE)
        {

            esp_avrc_rn_param_t rn_param = {
                .volume = s_volume};

            ESP_ERROR_CHECK(
                esp_avrc_tg_send_rn_rsp(
                    ESP_AVRC_RN_VOLUME_CHANGE,
                    ESP_AVRC_RN_RSP_INTERIM,
                    &rn_param));
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        uint8_t incoming_vol = param->set_abs_vol.volume & 0x7f;
        if (incoming_vol > max_volume)
        {
            s_volume = max_volume;
        }
        else
        {
            s_volume = incoming_vol;
        }

        ESP_LOGI(
            "BT_AVRCP",
            "Volume: %u%%",
            (unsigned)(s_volume * 100 / 0x7f));

        break;

    default:
        break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI("BT_A2DP", "Connected to device!");
            memcpy(device_bd, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            has_bd = true;

            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            ESP_LOGI("BT_A2DP", "Device Disconnected!");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            if (has_bd && param->conn_stat.disc_rsn != ESP_A2D_DISC_RSN_NORMAL)
            {
                ESP_LOGI("BT_A2DP", "Link lost unexpectedly. Attempting auto-reconnect...");

                esp_a2d_sink_connect(device_bd);
            }
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
    {
        esp_a2d_mcc_t *mcc = &param->audio_cfg.mcc;
        if (mcc->type == ESP_A2D_MCT_SBC)
        {
            uint32_t sample_rate = 44100;
            uint8_t freq = mcc->cie.sbc_info.samp_freq;

            if (freq & ESP_A2D_SBC_CIE_SF_48K)
                sample_rate = 48000;
            else if (freq & ESP_A2D_SBC_CIE_SF_44K)
                sample_rate = 44100;
            else if (freq & ESP_A2D_SBC_CIE_SF_32K)
                sample_rate = 32000;
            else if (freq & ESP_A2D_SBC_CIE_SF_16K)
                sample_rate = 16000;

            i2s_channel_disable(tx_handle);
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
            i2s_channel_enable(tx_handle);

            ESP_LOGI("BT_A2DP", "I2S reconfigured to sample rate: %lu Hz", (unsigned long)sample_rate);
        }
        break;
    }

    case ESP_A2D_PROF_STATE_EVT:
    {
        ESP_LOGI(
            "BT_A2DP",
            "A2DP profile state: init=%d",
            param->a2d_prof_stat.init_state);

        if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS)
        {
            ESP_LOGI("BT_A2DP", "A2DP profile ready");
        }
        break;
    }

    default:
        break;
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI("BT_GAP", "Authentication successful with: %s", param->auth_cmpl.device_name);
        }
        else
        {
            ESP_LOGE("BT_GAP", "Authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        // Auto-confirm pairing numeric requests from Windows
        ESP_LOGI("BT_GAP", "Auto-confirming pairing request...");
        ESP_LOGI("BT_GAP", "GAP User Confirmation Request: %lu", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI("BT_GAP", "GAP Key Notification Passkey: %lu", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI("BT_GAP", "GAP Key Request");
        break;

    default:
        break;
    }
}

static void init_bluetooth(void)
{

    esp_bt_controller_config_t bt_cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(
        esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name("ESP32_A2DP_SINK"));

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_app_gap_cb));
    esp_bt_cod_t cod;
    cod.major = ESP_BT_COD_MAJOR_DEV_AV; // Audio/Video major device class
    cod.minor = 0x08;                    // HiFi Audio minor class (or ESP_BT_COD_MINOR_AV_HEADSET)

    // Use ESP_BT_SET_COD_ALL or ESP_BT_SET_COD_MAJOR_MINOR which are the valid enums
    ESP_ERROR_CHECK(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL));


    // Change IO Capability to NONE for "Just Works" pairing
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE; // <-- Fix this
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(param_type, &io_cap, sizeof(io_cap)));

    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(bt_app_avrc_tg_cb));

    ESP_ERROR_CHECK(esp_avrc_tg_init());

    // Pass the features and notifications into the target configuration
    // (Note: Depending on your exact ESP-IDF version, you use esp_avrc_tg_init or feature macros)

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(
        ESP_AVRC_BIT_MASK_OP_SET,
        &evt_set,
        ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

    /*
     * Initialize A2DP Sink after AVRCP.
     */
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_app_a2d_cb));
    ESP_ERROR_CHECK(
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    ESP_ERROR_CHECK(
        esp_bt_gap_set_scan_mode(
            ESP_BT_CONNECTABLE,
            ESP_BT_GENERAL_DISCOVERABLE));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    init_i2s();
    // Create the No-Lock Ringbuffer optimal for byte stream pipelines
    s_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_audio_ringbuf == NULL)
    {
        ESP_LOGE("BT_AUDIO", "Failed to create ringbuffer");
        return;
    }

    // Spawn the task on Core 1 (or Core 0 depending on your architecture)
    xTaskCreatePinnedToCore(i2s_write_task, "i2s_write_task", 4096, NULL, configMAX_PRIORITIES - 1, &s_i2s_write_task_handle, 1);

    init_bluetooth();
}