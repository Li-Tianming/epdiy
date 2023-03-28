#include <stdint.h>
#include <string.h>

#include "render_lcd.h"
#include "include/epd_board.h"
#include "include/epd_driver.h"
#include "include/epd_internals.h"
#include "lut.h"
#include "render.h"
#include "s3_lcd.h"
#include "display_ops.h"

static bool IRAM_ATTR fill_line_noop(uint8_t *line) {
    memset(line, 0x00, EPD_LINE_BYTES);
    return false;
}

static bool IRAM_ATTR fill_line_white(uint8_t *line) {
    memset(line, CLEAR_BYTE, EPD_LINE_BYTES);
    return false;
}

static bool IRAM_ATTR fill_line_black(uint8_t *line) {
    memset(line, DARK_BYTE, EPD_LINE_BYTES);
    return false;
}

static bool IRAM_ATTR retrieve_line_isr(RenderContext_t* ctx, uint8_t *buf) {
    if (ctx->lines_consumed >= FRAME_LINES) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    assert(thread < NUM_FEED_THREADS);

    LineQueue_t *lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;
    assert(lq_read(lq, buf) == 0);
    if (ctx->lines_consumed >= EPD_HEIGHT) {
        memset(buf, 0x00, EPD_LINE_BYTES);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR prepare_lcd_frame(RenderContext_t *ctx) {
    int frame_time = DEFAULT_FRAME_TIME;
    if (ctx->phase_times != NULL) {
        frame_time = ctx->phase_times[ctx->current_frame];
    }

    if (ctx->mode & MODE_EPDIY_MONOCHROME) {
        frame_time = MONOCHROME_FRAME_TIME;
    }
    ctx->frame_time = frame_time;

    enum EpdDrawMode mode = ctx->mode;
    const EpdWaveformPhases *phases =
        ctx->waveform->mode_data[ctx->waveform_index]
            ->range_data[ctx->waveform_range];

    ctx->error |=
        calculate_lut(ctx->conversion_lut, ctx->conversion_lut_size, mode,
                      ctx->current_frame, phases);

    ctx->lines_prepared = 0;
    ctx->lines_consumed = 0;

    // on the classic ESP32, the prepare task starts the feeder task
    xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
    xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t *ctx) {
    epd_lcd_frame_done_cb(NULL);
    epd_lcd_line_source_cb(NULL);

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

void lcd_draw_prepared(RenderContext_t *ctx) {
    epd_set_mode(1);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb(handle_lcd_frame_done);
        prepare_lcd_frame(ctx);
        // transmission started in renderer threads
        xSemaphoreTake(ctx->frame_done, portMAX_DELAY);

        for (int i = 0; i < NUM_FEED_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        ctx.current_frame++;

        // make the watchdog happy.
        if (k % 10 == 0) {
            vTaskDelay(0);
        }
    }

    epd_lcd_line_source_cb(NULL);
    epd_lcd_frame_done_cb(NULL);

    epd_set_mode(1);
}

void epd_push_pixels_lcd(RenderContext_t *ctx, short time, int color) {
    ctx->current_frame = 0;
    epd_lcd_frame_done_cb(handle_lcd_frame_done);
    if (color == 0) {
        epd_lcd_line_source_cb(&fill_line_black);
    } else if (color == 1) {
        epd_lcd_line_source_cb(&fill_line_white);
    } else {
        epd_lcd_line_source_cb(&fill_line_noop);
    }
    epd_lcd_start_frame();
    xSemaphoreTake(ctx->frame_done, portMAX_DELAY);
}

void IRAM_ATTR lcd_feed_frame(RenderContext_t *ctx, int thread_id) {
    uint8_t line_buf[EPD_LINE_BYTES];
    uint8_t input_line[EPD_WIDTH];

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, EPD_WIDTH);

    LineQueue_t *lq = &render_context.line_queues[thread_id];

    int l = 0;
    while (l = atomic_fetch_add(&render_context.lines_prepared, 1),
           l < FRAME_LINES) {

        render_context.line_threads[l] = thread_id;

        // FIXME: handle too-small updates
        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l - min_y == 31) {
            epd_lcd_line_source_cb(&retrieve_line_isr);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y ||
            (render_context.drawn_lines != NULL &&
             !render_context.drawn_lines[l - area.y])) {
            uint8_t *buf = NULL;
            while (buf == NULL)
                buf = lq_current(lq);
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t *lp = (uint32_t *)input_line;
        const uint8_t *ptr = ptr_start + bytes_per_line * (l - min_y);

        Cache_Start_DCache_Preload((uint32_t)ptr, EPD_WIDTH, 0);

        assert(area.width == EPD_WIDTH && area.x == 0 &&
               !horizontally_cropped && !render_context.error);

        lp = (uint32_t *)ptr;

        uint8_t *buf = NULL;
        while (buf == NULL)
            buf = lq_current(lq);

        (*input_calc_func)(lp, buf, render_context.conversion_lut);

        lq_commit(lq);
    }
}
