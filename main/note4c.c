// SSD2683 four-color sequence adapted from the NOTE4C reference firmware.
// Board source revision and license are in README.md and licenses/.
#include "note4c.h"
#include "screen.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { POWER_HOLD=17, PANEL_POWER=6, BUSY=8, RESET=9, DC=10, CS=11, CLK=12, MOSI=13 };
static spi_device_handle_t spi;
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_cal;
static void pause_ms(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)+1); }

static void battery_init(void) {
    adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1};
    adc_oneshot_chan_cfg_t channel={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_12};
    adc_cali_curve_fitting_config_t calibration={.unit_id=ADC_UNIT_1,.chan=ADC_CHANNEL_3,
        .atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_12};
    if (adc_oneshot_new_unit(&unit,&battery_adc)!=ESP_OK) return;
    if (adc_oneshot_config_channel(battery_adc,ADC_CHANNEL_3,&channel)!=ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&calibration,&battery_cal)!=ESP_OK) {
        adc_oneshot_del_unit(battery_adc);
        battery_adc=NULL;
    }
}
int note4c_battery_percent(void) {
    if (!battery_adc || !battery_cal) return -1;
    int sum=0;
    for (int i=0;i<10;++i) {
        int raw,mv;
        if (adc_oneshot_read(battery_adc,ADC_CHANNEL_3,&raw)!=ESP_OK ||
            adc_cali_raw_to_voltage(battery_cal,raw,&mv)!=ESP_OK) return -1;
        sum+=2*mv; // GPIO4 reads half of the cell voltage.
    }
    int mv=sum/10;
    if (mv<2500 || mv>4500) return -1;
    // Transfer curve from the NOTE4C reference firmware.
    int percent=(mv*(9016-mv)-19189000)/10000;
    return percent<0 ? 0 : (percent>100 ? 100 : percent);
}
void note4c_init(void) {
    gpio_hold_dis(POWER_HOLD);
    gpio_set_level(POWER_HOLD,1);
    gpio_config_t latch = {.pin_bit_mask=1ULL<<POWER_HOLD,.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&latch));
    gpio_hold_en(POWER_HOLD);
    gpio_hold_dis(PANEL_POWER);
    gpio_config_t outputs = {.pin_bit_mask=(1ULL<<PANEL_POWER)|(1ULL<<RESET)|(1ULL<<DC),
                             .mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_set_level(PANEL_POWER,0);
    gpio_config_t keys = {.pin_bit_mask=(1ULL<<BTN_UP)|(1ULL<<BTN_DOWN)|(1ULL<<BTN_FRONT),
                          .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&keys));
    gpio_config_t busy = {.pin_bit_mask=1ULL<<BUSY,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&busy));
    spi_bus_config_t bus = {.mosi_io_num=MOSI,.miso_io_num=-1,.sclk_io_num=CLK,
                            .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=1024};
    spi_device_interface_config_t dev = {.clock_speed_hz=8000000,.mode=0,.spics_io_num=CS,.queue_size=1};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST,&dev,&spi));
    battery_init();
}
bool note4c_pressed(enum button button) { return !gpio_get_level((gpio_num_t)button); }

static bool send_bytes(bool data, const uint8_t *bytes, size_t len) {
    gpio_set_level(DC,data);
    spi_transaction_t tx = {.length=len*8,.tx_buffer=bytes};
    return spi_device_polling_transmit(spi,&tx)==ESP_OK;
}
static bool command(uint8_t c) { return send_bytes(false,&c,1); }
static bool argument(uint8_t b) { return send_bytes(true,&b,1); }
static bool ready(void) {
    TickType_t start=xTaskGetTickCount();
    pause_ms(10);
    while (!gpio_get_level(BUSY)) {
        if (xTaskGetTickCount()-start >= pdMS_TO_TICKS(120000)) return false;
        pause_ms(20);
    }
    return true;
}
bool note4c_show(const uint8_t *frame) {
    gpio_set_level(PANEL_POWER,1); pause_ms(10);
    gpio_set_level(RESET,1); pause_ms(10);
    gpio_set_level(RESET,0); pause_ms(20);
    gpio_set_level(RESET,1); pause_ms(10);
    bool ok = ready() && command(0xE9) && argument(0x01) && command(0x10) && ready();
    for (int y=0;ok && y<SCREEN_H;++y)
        ok = send_bytes(true,frame+y*(SCREEN_W/4),SCREEN_W/4);
    ok = ok && command(0x04) && ready() && command(0x12) && argument(0) && ready()
            && command(0x02) && argument(0) && ready();
    if (ok) {
        pause_ms(20);
        ok = command(0x07) && argument(0xA5);
    }
    gpio_set_level(PANEL_POWER,0);
    if (!ok) ESP_LOGE("NOTE4C","Display transaction or BUSY timeout; panel rail switched off");
    return ok;
}
