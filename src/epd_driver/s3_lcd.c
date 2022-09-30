#include "s3_lcd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "soc/rmt_periph.h"
#include "soc/lcd_periph.h"
#include "driver/rmt_types.h"
#include "driver/rmt_types_legacy.h"
#include "hal/rmt_types.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
 #include "hal/gpio_hal.h"
#include "esp_err.h"
#include "esp_log.h"
//#include "driver/rmt.h"
#include "soc/rmt_struct.h"
#include "hal/rmt_ll.h"
#include "hal/lcd_ll.h"
#include "rom/cache.h"
#include "lut.h"
#include "esp_private/periph_ctrl.h"

#define TAG "epdiy_s3"

inline int min(int x, int y) { return x < y ? x : y; }
inline int max(int x, int y) { return x > y ? x : y; }

#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define RMT_CLOCK_HZ                   (10 * 1000 * 1000)
#define EXAMPLE_PIN_NUM_BK_LIGHT       -1
#define EXAMPLE_PIN_NUM_HSYNC          46
#define EXAMPLE_PIN_NUM_VSYNC          3
#define EXAMPLE_PIN_NUM_CKV            1
#define EXAMPLE_PIN_NUM_DE             0
#define EXAMPLE_PIN_NUM_PCLK           9
#define EXAMPLE_PIN_NUM_DATA6          14 // B0
#define EXAMPLE_PIN_NUM_DATA7          13 // B1
#define EXAMPLE_PIN_NUM_DATA4          12 // B2
#define EXAMPLE_PIN_NUM_DATA5          11 // B3
#define EXAMPLE_PIN_NUM_DATA2          10 // B4
#define EXAMPLE_PIN_NUM_DATA3          39 // G0
#define EXAMPLE_PIN_NUM_DATA0          38 // G1
#define EXAMPLE_PIN_NUM_DATA1          45 // G2
#define EXAMPLE_PIN_NUM_DATA8          48 // G3
#define EXAMPLE_PIN_NUM_DATA9          47 // G4
#define EXAMPLE_PIN_NUM_DATA10         21 // G5
#define EXAMPLE_PIN_NUM_DATA11         1  // R0
#define EXAMPLE_PIN_NUM_DATA12         2  // R1
#define EXAMPLE_PIN_NUM_DATA13         42 // R2
#define EXAMPLE_PIN_NUM_DATA14         41 // R3
#define EXAMPLE_PIN_NUM_DATA15         40 // R4
#define EXAMPLE_PIN_NUM_DISP_EN        8

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              300
#define EXAMPLE_LCD_V_RES              (((EPD_HEIGHT  + 3) / 4) * 4)

#define RMT_CKV_CHAN                   RMT_CHANNEL_1

// the RMT channel configuration object
static esp_lcd_panel_handle_t panel_handle = NULL;
static volatile long int framecounter = 0;
static const uint8_t EPD_NOOP = 0x00;

/** set appropriate callback functions for data output in next VSYNC */
static bool enable_data_out = false;
static uint8_t static_output_data = EPD_NOOP;
/** unset appropriate callback functions for data output in next VSYNC */
static bool disable_data_out = false;
static QueueHandle_t line_queue;
static void (*frame_prepare_cb)(void) = NULL;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
// The extern line is declared in esp-idf/components/driver/deprecated/rmt_legacy.c. It has access to RMTMEM through the rmt_private.h header
// which we can't access outside the sdk. Declare our own extern here to properly use the RMTMEM smybol defined in components/soc/[target]/ld/[target].peripherals.ld
// Also typedef the new rmt_mem_t struct to the old rmt_block_mem_t struct. Same data fields, different names
typedef rmt_mem_t rmt_block_mem_t ;
extern rmt_block_mem_t RMTMEM;
#endif

static bool epd_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data);
static bool on_bounce_buffer(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes, void *user_ctx);
static bool on_singlecolor_bounce_buffer(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes, void *user_ctx);

static void esp_lcd_set_fetch_callbacks() {
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = epd_on_vsync_event,
        .on_bounce_empty = on_bounce_buffer,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, &line_queue));
}

static void esp_lcd_set_singlecolor_callbacks(const uint8_t* color) {
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = epd_on_vsync_event,
        .on_bounce_empty = on_singlecolor_bounce_buffer,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, (void*)color));
}

void s3_set_frame_prepare_cb(void (*cb)(void)) {
    frame_prepare_cb = cb;
}

void s3_delete_frame_prepare_cb() {
    frame_prepare_cb = NULL;
}

static bool IRAM_ATTR epd_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data) {

    //rmt_ll_tx_stop(&RMT, RMT_CKV_CHAN);
    //gpio_set_level(15, 0);

    rmt_ll_tx_reset_loop_count(&RMT, RMT_CKV_CHAN);
    rmt_ll_tx_reset_pointer(&RMT, RMT_CKV_CHAN);

    if (enable_data_out) {
        esp_lcd_set_fetch_callbacks();
        enable_data_out = false;
    }

    if (disable_data_out) {
        esp_lcd_set_singlecolor_callbacks(&static_output_data);
        disable_data_out = false;
    }

    rmt_ll_tx_start(&RMT, RMT_CKV_CHAN);
    //gpio_set_level(15, 1);

    return pdFALSE;
}


static void IRAM_ATTR rmt_interrupt_handler(void *args) {
    uint32_t intr_status = rmt_ll_tx_get_interrupt_status(&RMT, RMT_CKV_CHAN);
    rmt_ll_clear_interrupt_status(&RMT, intr_status);

    framecounter++;

    if (frame_prepare_cb != NULL) {
        (*frame_prepare_cb)();
    }
}

static bool IRAM_ATTR on_bounce_buffer(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes, void *user_ctx) {
    uint8_t* bp = (uint8_t*)bounce_buf;
    BaseType_t task_awoken = pdFALSE;

    assert(len_bytes % (EPD_WIDTH / 4) == 0);

    for (int i=0; i < len_bytes / (EPD_WIDTH / 4); i++) {
        if (!xQueueReceiveFromISR(line_queue, bp, &task_awoken)) {
            assert(false);
        }
        bp += EPD_WIDTH / 4;
    }

    return task_awoken;
}

void s3_lcd_enable_data_out() {
    enable_data_out = true;
}
void s3_lcd_disable_data_out(uint8_t color) {
    static_output_data = color;
    disable_data_out = true;
}

static bool IRAM_ATTR on_singlecolor_bounce_buffer(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes, void *user_ctx) {
    uint8_t color = *((uint8_t*)user_ctx);
    memset(bounce_buf, color, len_bytes);
    return pdFALSE;
}

void setup_transfer() {
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
}

void stop_transfer() {
    ESP_ERROR_CHECK(esp_lcd_panel_del(panel_handle));
}


void s3_set_display_queue(QueueHandle_t ext_line_queue) {
  line_queue = ext_line_queue;
}

void epd_lcd_init() {

  gpio_config_t debug_io_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ull << 15,
  };

  gpio_config(&debug_io_conf);

  periph_module_reset(rmt_periph_signals.groups[0].module);
  periph_module_enable(rmt_periph_signals.groups[0].module);

  rmt_ll_enable_periph_clock(&RMT, true);
  rmt_ll_set_group_clock_src(&RMT, RMT_CKV_CHAN, (rmt_clock_source_t)RMT_BASECLK_DEFAULT, 1, 0, 0);
  rmt_ll_tx_set_channel_clock_div(&RMT, RMT_CKV_CHAN, 8);
  rmt_ll_tx_set_mem_blocks(&RMT, RMT_CKV_CHAN, 2);
  rmt_ll_enable_mem_access_nonfifo(&RMT, true);
  rmt_ll_tx_fix_idle_level(&RMT, RMT_CKV_CHAN, RMT_IDLE_LEVEL_HIGH, false);
  rmt_ll_tx_enable_carrier_modulation(&RMT, RMT_CKV_CHAN, false);

  rmt_ll_tx_enable_loop(&RMT, RMT_CKV_CHAN, true);
  rmt_ll_tx_enable_loop_count(&RMT, RMT_CKV_CHAN, true);
  rmt_ll_tx_enable_loop_autostop(&RMT, RMT_CKV_CHAN, true);
  rmt_ll_tx_set_loop_count(&RMT, RMT_CKV_CHAN, EXAMPLE_LCD_V_RES + 5);
  //rmt_ll_tx_enable_loop(&RMT, RMT_CKV_CHAN, true);

  volatile rmt_item32_t *rmt_mem_ptr =
      &(RMTMEM.chan[RMT_CKV_CHAN].data32[0]);
    rmt_mem_ptr->duration0 = 120;
    rmt_mem_ptr->level0 = 0;
    rmt_mem_ptr->duration1 = 60;
    rmt_mem_ptr->level1 = 1;
    rmt_mem_ptr++;
    rmt_mem_ptr->val = 0;

  // Divide 80MHz APB Clock by 8 -> .1us resolution delay

  gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[EXAMPLE_PIN_NUM_CKV], PIN_FUNC_GPIO);
  gpio_set_direction(EXAMPLE_PIN_NUM_CKV, GPIO_MODE_OUTPUT);
  esp_rom_gpio_connect_out_signal(EXAMPLE_PIN_NUM_CKV, rmt_periph_signals.groups[0].channels[RMT_CKV_CHAN].tx_sig, false, 0);
  rmt_ll_enable_interrupt(&RMT, RMT_LL_EVENT_TX_LOOP_END(RMT_CKV_CHAN), true);
  intr_handle_t rmt_intr_handle;
  ESP_ERROR_CHECK(esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_IRAM,
                 rmt_interrupt_handler, NULL, &rmt_intr_handle));

  //row_rmt_config.rmt_mode = RMT_MODE_TX;
  // currently hardcoded: use channel 0
  //row_rmt_config.channel = RMT_CHANNEL_1;

  //row_rmt_config.gpio_num = EXAMPLE_PIN_NUM_CKV;
  //row_rmt_config.mem_block_num = 2;

  //row_rmt_config.clk_div = 8;


  ESP_LOGI(TAG, "using resolution %dx%d", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);

  //row_rmt_config.tx_config.loop_en = true;
  //row_rmt_config.tx_config.carrier_en = false;
  //row_rmt_config.tx_config.carrier_level = RMT_CARRIER_LEVEL_LOW;
  //row_rmt_config.tx_config.idle_level = RMT_IDLE_LEVEL_HIGH;
  //row_rmt_config.tx_config.idle_output_en = true;
  //rmt_config(&row_rmt_config);

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 8,
        .bits_per_pixel = 8,
        .psram_trans_align = 64,
        .bounce_buffer_size_px = 2 * EXAMPLE_LCD_H_RES,
        .clk_src = LCD_CLK_SRC_PLL240M,
        .disp_gpio_num = EXAMPLE_PIN_NUM_DISP_EN,
        .pclk_gpio_num = EXAMPLE_PIN_NUM_PCLK,
        .vsync_gpio_num = EXAMPLE_PIN_NUM_VSYNC,
        .hsync_gpio_num = EXAMPLE_PIN_NUM_HSYNC,
        .de_gpio_num = EXAMPLE_PIN_NUM_DE,
        .data_gpio_nums = {
            EXAMPLE_PIN_NUM_DATA0,
            EXAMPLE_PIN_NUM_DATA1,
            EXAMPLE_PIN_NUM_DATA2,
            EXAMPLE_PIN_NUM_DATA3,
            EXAMPLE_PIN_NUM_DATA4,
            EXAMPLE_PIN_NUM_DATA5,
            EXAMPLE_PIN_NUM_DATA6,
            EXAMPLE_PIN_NUM_DATA7,
            EXAMPLE_PIN_NUM_DATA8,
            //EXAMPLE_PIN_NUM_DATA9,
            //EXAMPLE_PIN_NUM_DATA10,
            //EXAMPLE_PIN_NUM_DATA11,
            //EXAMPLE_PIN_NUM_DATA12,
            //EXAMPLE_PIN_NUM_DATA13,
            //EXAMPLE_PIN_NUM_DATA14,
            //EXAMPLE_PIN_NUM_DATA15,
        },
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            // The following parameters should refer to LCD spec
            .hsync_back_porch = 20,
            .hsync_front_porch = 30,
            .hsync_pulse_width = 10,
            .vsync_back_porch = 5,
            .vsync_front_porch = 100,
            .vsync_pulse_width = 1,
            // test
            .flags.pclk_active_neg = false,
            .flags.de_idle_high = true,
            .flags.hsync_idle_low = true,
        },
        .flags.no_fb = true, // allocate frame buffer in PSRAM
        .flags.refresh_on_demand = false,
//#if CONFIG_EXAMPLE_DOUBLE_FB
//        .flags.double_fb = true,   // allocate double frame buffer
//#endif // CONFIG_EXAMPLE_DOUBLE_FB
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    uint16_t line_len = EXAMPLE_LCD_H_RES + panel_config.timings.hsync_front_porch + panel_config.timings.hsync_back_porch + panel_config.timings.hsync_pulse_width;

    ESP_LOGI(TAG, "line len: %u cycles", line_len);

    //rmt_encode_state_t rmt_state = 0;
    //copy_encoder->encode(copy_encoder, rmt_chan, &shape, sizeof(rmt_symbol_word_t) * 64, &rmt_state);

    ESP_LOGI(TAG, "Register event callbacks");
    esp_lcd_set_singlecolor_callbacks(&EPD_NOOP);

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    setup_transfer();

    //Cache_WriteBack_Addr((uint32_t)buf1, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);
    //esp_lcd_rgb_panel_refresh(panel_handle);
}


void output_singlecolor_frame(EpdRect area, short time, uint8_t color) {
    s3_lcd_disable_data_out(color);
    //setup_transfer();
    long int orig_counter = framecounter;

    //esp_lcd_rgb_panel_refresh(panel_handle);
    //while (!(LCD_CAM.lc_dma_int_raw.lcd_trans_done_int_raw)) {
    while (orig_counter == framecounter) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    s3_lcd_disable_data_out(EPD_NOOP);
    //stop_transfer();
}

// void IRAM_ATTR supply_display(OutputParams* params) {
//     uint8_t line[EPD_WIDTH];
//     memset(line, 255, EPD_WIDTH);
//     uint8_t out_line[EPD_WIDTH / 4];
//
//     ESP_LOGI(TAG, "display supplicant initialized");
//
//     while (true) {
//         xSemaphoreTake(params->start_smphr, portMAX_DELAY);
//         params->error |= calculate_lut(params);
//
//         EpdRect area = params->area;
//         const uint8_t *ptr = params->data_ptr;
//         const bool crop = (params->crop_to.width > 0 && params->crop_to.height > 0);
//         // number of pixels per byte of input data
//         int ppB = 0;
//         int bytes_per_line = 0;
//         int width_divider = 0;
//
//         if (params->mode & MODE_PACKING_1PPB_DIFFERENCE) {
//           ppB = 1;
//           bytes_per_line = area.width;
//           width_divider = 1;
//         } else if (params->mode & MODE_PACKING_2PPB) {
//           ppB = 2;
//           bytes_per_line = area.width / 2 + area.width % 2;
//           width_divider = 2;
//         } else if (params->mode & MODE_PACKING_8PPB) {
//           ppB = 8;
//           bytes_per_line = (area.width / 8 + (area.width % 8 > 0));
//           width_divider = 8;
//         } else {
//           params->error |= EPD_DRAW_INVALID_PACKING_MODE;
//         }
//
//         int crop_x = (crop ? params->crop_to.x : 0);
//         int crop_y = (crop ? params->crop_to.y : 0);
//         int crop_w = (crop ? params->crop_to.width : 0);
//         int crop_h = (crop ? params->crop_to.height : 0);
//
//         // Adjust for negative starting coordinates with optional crop
//         if (area.x - crop_x < 0) {
//           ptr += -(area.x - crop_x) / width_divider;
//         }
//
//         if (area.y - crop_y < 0) {
//           ptr += -(area.y - crop_y) * bytes_per_line;
//         }
//
//         // calculate start and end row with crop
//         int min_y = area.y + crop_y;
//         int max_y = min(min_y + (crop ? crop_h : area.height), area.height);
//
//
//         // interval of the output line that is needed
//         // FIXME: only lookup needed parts
//         int line_start_x = area.x + (crop ? params->crop_to.x : 0);
//         int line_end_x = line_start_x + (crop ? params->crop_to.width : area.width);
//         line_start_x = min(max(line_start_x, 0), EPD_WIDTH);
//         line_end_x = min(max(line_end_x, 0), EPD_WIDTH);
//
//
//         enum EpdDrawMode mode = params->mode;
//         void (*input_calc_func)(const uint32_t *, uint8_t *, const uint8_t *) =
//             NULL;
//         if (mode & MODE_PACKING_2PPB) {
//           if (params->conversion_lut_size == 1024) {
//             if (mode & PREVIOUSLY_WHITE) {
//               input_calc_func = &calc_epd_input_4bpp_1k_lut_white;
//             } else if (mode & PREVIOUSLY_BLACK) {
//               input_calc_func = &calc_epd_input_4bpp_1k_lut_black;
//             } else {
//               params->error |= EPD_DRAW_LOOKUP_NOT_IMPLEMENTED;
//             }
//           } else if (params->conversion_lut_size == (1 << 16)) {
//             input_calc_func = &calc_epd_input_4bpp_lut_64k;
//           } else {
//             params->error |= EPD_DRAW_LOOKUP_NOT_IMPLEMENTED;
//           }
//         } else if (mode & MODE_PACKING_1PPB_DIFFERENCE) {
//           input_calc_func = &calc_epd_input_1ppB;
//         } else if (mode & MODE_PACKING_8PPB) {
//           input_calc_func = &calc_epd_input_1bpp;
//         } else {
//           params->error |= EPD_DRAW_LOOKUP_NOT_IMPLEMENTED;
//         }
//
//         int non_zero = 0;
//
//         for (int i = 0; i < EXAMPLE_LCD_V_RES; i++) {
//
//           if (i < min_y || i >= max_y) {
//             memset(out_line, 0x00, EPD_WIDTH / 4);
//             xQueueSendToBack(*params->output_queue, out_line, portMAX_DELAY);
//             continue;
//           }
//
//           uint32_t *lp = (uint32_t *)line;
//           bool shifted = false;
//           if (area.width == EPD_WIDTH && area.x == 0 && !crop && !params->error) {
//             lp = (uint32_t *)ptr;
//             ptr += bytes_per_line;
//           } else if (!params->error) {
//             uint8_t *buf_start = (uint8_t *)line;
//             uint32_t line_bytes = bytes_per_line;
//
//             int min_x = area.x + crop_x;
//             if (min_x >= 0) {
//               buf_start += min_x / width_divider;
//             } else {
//               // reduce line_bytes to actually used bytes
//               // ptr was already adjusted above
//               line_bytes += min_x / width_divider;
//             }
//             line_bytes = min(line_bytes, EPD_WIDTH / width_divider -
//                                              (uint32_t)(buf_start - line));
//             gpio_set_level(15, 0);
//             memcpy(buf_start, ptr, line_bytes);
//             gpio_set_level(15, 1);
//             ptr += bytes_per_line;
//             lp = (uint32_t *)line;
//           }
//
//           (*input_calc_func)(lp, out_line, params->conversion_lut);
//           if (line_start_x > 0 || line_end_x < EPD_WIDTH) {
//             mask_line_buffer(out_line, line_start_x, line_end_x);
//           }
//
//           xQueueSendToBack(*params->output_queue, out_line, portMAX_DELAY);
//
//           if (shifted) {
//             memset(line, 255, EPD_WIDTH);
//           }
//
//         }
//
//         //s3_lcd_disable_data_out(EPD_NOOP);
//         //xSemaphoreGive(params->done_smphr);
//     }
// }
//
