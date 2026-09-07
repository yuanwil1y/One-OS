#include "board.h"

#include "driver/i2c_master.h"

#define BOARD_I2C_PORT       I2C_NUM_0
#define BOARD_I2C_CLOCK_HZ   200000
#define BOARD_I2C_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_cst816_dev;
static bool s_touch_initialized;

static esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL && s_cst816_dev != NULL) {
        return ESP_OK;
    }

    if (s_i2c_bus == NULL) {
        const i2c_master_bus_config_t bus_config = {
            .i2c_port = BOARD_I2C_PORT,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };

        esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
        if (err != ESP_OK) {
            s_i2c_bus = NULL;
            return err;
        }
    }

    if (s_cst816_dev == NULL) {
        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_CST816_ADDR,
            .scl_speed_hz = BOARD_I2C_CLOCK_HZ,
        };

        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus,
                                                   &device_config,
                                                   &s_cst816_dev);
        if (err != ESP_OK) {
            (void)i2c_del_master_bus(s_i2c_bus);
            s_i2c_bus = NULL;
            s_cst816_dev = NULL;
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t board_touch_init(void)
{
    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t command[2] = {0x00, 0x00};
    err = i2c_master_transmit(s_cst816_dev,
                              command,
                              sizeof(command),
                              BOARD_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        s_touch_initialized = true;
    }
    return err;
}

esp_err_t board_touch_read(board_touch_sample_t *out_sample)
{
    if (out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_touch_initialized) {
        esp_err_t err = board_touch_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    const uint8_t start_register = 0x00;
    uint8_t data[7] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_cst816_dev,
                                                 &start_register,
                                                 1,
                                                 data,
                                                 sizeof(data),
                                                 BOARD_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    out_sample->pressed = data[2] != 0;
    out_sample->x = 0;
    out_sample->y = 0;
    if (out_sample->pressed) {
        out_sample->x = (uint16_t)(((uint16_t)(data[3] & 0x0f) << 8) | data[4]);
        out_sample->y = (uint16_t)(((uint16_t)(data[5] & 0x0f) << 8) | data[6]);
    }
    return ESP_OK;
}
