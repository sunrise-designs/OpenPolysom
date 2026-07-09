#include "battery_gauge.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "hal/adc_types.h"

// GPIO4 is ADC1_CHANNEL_4 on ESP32-C6
#define BATT_ADC_CHANNEL ADC_CHANNEL_4

static const char *TAG = "battery_gauge";
static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_adc_cali;
static bool s_cali_enabled = false;

void battery_gauge_init(adc_oneshot_unit_handle_t adc_unit)
{
    s_adc_unit = adc_unit;
    if (!s_adc_unit) {
        ESP_LOGE(TAG, "Invalid ADC unit handle");
        return;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_unit, BATT_ADC_CHANNEL, &config));

    // Initialize calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali);
    if (ret == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "Calibration initialized successfully");
    } else {
        ESP_LOGW(TAG, "ADC calibration failed, using uncalibrated raw values");
    }
}

int battery_gauge_get_voltage_mv(void)
{
    if (!s_adc_unit) return 0;

    int raw;
    esp_err_t ret = adc_oneshot_read(s_adc_unit, BATT_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ADC, error %d", ret);
        return 0;
    }

    int voltage_mv = 0;
    if (s_cali_enabled) {
        adc_cali_raw_to_voltage(s_adc_cali, raw, &voltage_mv);
    } else {
        // Fallback: 12-bit ADC, 12dB attenuation -> ~3300mV max
        voltage_mv = (raw * 3300) / 4095;
    }

    // Voltage is divided by exactly half, so actual battery voltage is 2 * voltage_mv
    return voltage_mv * 2;
}

uint8_t battery_gauge_get_percentage(void)
{
    int voltage_mv = battery_gauge_get_voltage_mv();
    if (voltage_mv <= 0) return 0;

    // 4.2V -> 4200mV (100%), 3.4V -> 3400mV (0%)
    if (voltage_mv >= 4200) return 100;
    if (voltage_mv <= 3400) return 0;

    return (uint8_t)(((voltage_mv - 3400) * 100) / (4200 - 3400));
}
