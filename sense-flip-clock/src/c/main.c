/**
 * Sense Flip Clock — Pebble Time 2 (Emery, 200x228)
 *
 * Inspired by the HTC Sense flip-clock widget: bare bold digits for
 * hour/minute (no card background), a colour weather icon in the middle,
 * and a bottom info strip with date/conditions + temperature/hi-lo.
 *
 * Time font is a placeholder system font — swap in a custom Segoe UI
 * font resource once provided (see PLACEHOLDER FONT note below).
 *
 * Battery-efficient: redraws on MINUTE_UNIT only. No continuous animation;
 * precipitation dots use a deterministic per-minute frame counter instead
 * of a timer, so the scene varies without extra redraws.
 */

#include <pebble.h>

// ============================================================================
// GLOBAL STATE
// ============================================================================

static Window *s_window;
static Layer *s_canvas_layer;

// PLACEHOLDER FONT: Roboto Bold subset stands in for Segoe UI until a
// custom TTF is supplied. Swap fonts_get_system_font(...) for
// fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOE_UI_64))
// once the font resource is added.
static GFont s_time_font;
static GFont s_temp_font;
static GFont s_date_font;
static GFont s_small_font;

static struct tm s_time;
static bool s_time_valid = false;
static int s_frame = 0;

static bool s_weather_valid = false;
static int s_weather_code = -1;
static int s_temp = 0;
static int s_temp_high = 0;
static int s_temp_low = 0;
static char s_condition[16] = "";

// ============================================================================
// WEATHER
// ============================================================================

static int condition_to_code(const char *c) {
    if (strcmp(c, "Clear") == 0) return 0;
    if (strcmp(c, "Cloudy") == 0) return 2;
    if (strcmp(c, "Fog") == 0) return 45;
    if (strcmp(c, "Drizzle") == 0) return 51;
    if (strcmp(c, "Fz. Drizzle") == 0) return 56;
    if (strcmp(c, "Rain") == 0) return 63;
    if (strcmp(c, "Fz. Rain") == 0) return 66;
    if (strcmp(c, "Snow") == 0) return 73;
    if (strcmp(c, "Snow Grains") == 0) return 77;
    if (strcmp(c, "Showers") == 0) return 81;
    if (strcmp(c, "Snow Shwrs") == 0) return 85;
    if (strcmp(c, "T-Storm") == 0) return 95;
    return 2;
}

static void request_weather(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
        app_message_outbox_send();
    }
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    Tuple *temp_tuple = dict_find(iterator, MESSAGE_KEY_TEMPERATURE);
    Tuple *high_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_HIGH);
    Tuple *low_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_LOW);
    Tuple *cond_tuple = dict_find(iterator, MESSAGE_KEY_CONDITIONS);

    if (temp_tuple && cond_tuple) {
        s_temp = (int)temp_tuple->value->int32;
        if (high_tuple) s_temp_high = (int)high_tuple->value->int32;
        if (low_tuple) s_temp_low = (int)low_tuple->value->int32;
        snprintf(s_condition, sizeof(s_condition), "%s", cond_tuple->value->cstring);
        s_weather_code = condition_to_code(s_condition);
        s_weather_valid = true;
        layer_mark_dirty(s_canvas_layer);
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Weather message dropped: %d", reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator,
                                    AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Weather request failed: %d", reason);
}

// ============================================================================
// WEATHER ICON DRAWING
// ============================================================================

static void draw_sun(GContext *ctx, GPoint center, int r, bool with_rays) {
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_circle(ctx, center, r + 4);
    graphics_context_set_fill_color(ctx, GColorOrange);
    graphics_fill_circle(ctx, center, r);

    if (with_rays) {
        graphics_context_set_stroke_color(ctx, GColorYellow);
        graphics_context_set_stroke_width(ctx, 3);
        for (int i = 0; i < 8; i++) {
            int32_t angle = (TRIG_MAX_ANGLE * i) / 8;
            int x1 = center.x + ((r + 6) * sin_lookup(angle)) / TRIG_MAX_RATIO;
            int y1 = center.y - ((r + 6) * cos_lookup(angle)) / TRIG_MAX_RATIO;
            int x2 = center.x + ((r + 15) * sin_lookup(angle)) / TRIG_MAX_RATIO;
            int y2 = center.y - ((r + 15) * cos_lookup(angle)) / TRIG_MAX_RATIO;
            graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
        }
    }
}

static void draw_cloud(GContext *ctx, GPoint center, int r, GColor color) {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, GPoint(center.x - r / 2, center.y), (r * 3) / 5);
    graphics_fill_circle(ctx, GPoint(center.x + r / 3, center.y - r / 4), (r * 2) / 3);
    graphics_fill_circle(ctx, GPoint(center.x + r, center.y + r / 6), r / 2);
    graphics_fill_rect(ctx,
        GRect(center.x - r, center.y, (r * 2) + r / 2, (r * 2) / 3),
        (r * 2) / 3 / 2, GCornersAll);
}

static void draw_weather_icon(GContext *ctx, GRect box, int code, int frame) {
    GPoint center = grect_center_point(&box);
    int r = box.size.w / 2 - 6;

    if (code < 0) {
        // No data yet — faint placeholder ring.
        graphics_context_set_stroke_color(ctx, GColorDarkGray);
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_circle(ctx, center, r);
        return;
    }

    bool clear = (code == 0);
    bool partly = (code >= 1 && code <= 3);
    bool fog = (code >= 45 && code <= 48);
    bool rainy = (code >= 51 && code <= 82 && !fog);
    bool snowy = (code >= 71 && code <= 86 &&
                  (code == 71 || code == 73 || code == 75 || code == 77 ||
                   code == 85 || code == 86));
    bool storm = (code >= 95);

    if (clear) {
        draw_sun(ctx, center, r, true);
    } else if (partly) {
        GPoint sun_c = GPoint(center.x - r / 3, center.y - r / 3);
        GPoint cloud_c = GPoint(center.x + r / 4, center.y + r / 4);
        draw_sun(ctx, sun_c, (r * 2) / 3, false);
        draw_cloud(ctx, cloud_c, (r * 2) / 3, GColorWhite);
    } else {
        GColor cloud_color = GColorWhite;
        if (fog) cloud_color = GColorLightGray;
        if (storm) cloud_color = GColorDarkGray;
        draw_cloud(ctx, center, (r * 3) / 4, cloud_color);
    }

    if (snowy) {
        graphics_context_set_fill_color(ctx, GColorWhite);
        for (int i = 0; i < 5; i++) {
            int sx = box.origin.x + ((i * 13) + (frame * 5)) % box.size.w;
            int sy = box.origin.y + box.size.h - 12 + ((i * 4) + (frame * 3)) % 8;
            graphics_fill_circle(ctx, GPoint(sx, sy), 2);
        }
    } else if (rainy && !storm) {
        graphics_context_set_stroke_color(ctx, GColorVividCerulean);
        graphics_context_set_stroke_width(ctx, 2);
        for (int i = 0; i < 4; i++) {
            int rx = box.origin.x + ((i * 15) + (frame * 7)) % box.size.w;
            int ry = box.origin.y + box.size.h - 14 + ((i * 5) + (frame * 3)) % 10;
            graphics_draw_line(ctx, GPoint(rx, ry), GPoint(rx - 3, ry + 8));
        }
    } else if (storm) {
        graphics_context_set_stroke_color(ctx, GColorYellow);
        graphics_context_set_stroke_width(ctx, 3);
        GPoint p1 = GPoint(center.x, box.origin.y + box.size.h - 24);
        GPoint p2 = GPoint(center.x - 6, box.origin.y + box.size.h - 12);
        GPoint p3 = GPoint(center.x + 2, box.origin.y + box.size.h - 12);
        GPoint p4 = GPoint(center.x - 4, box.origin.y + box.size.h - 2);
        graphics_draw_line(ctx, p1, p2);
        graphics_draw_line(ctx, p2, p3);
        graphics_draw_line(ctx, p3, p4);
    } else if (fog) {
        graphics_context_set_stroke_color(ctx, GColorLightGray);
        graphics_context_set_stroke_width(ctx, 2);
        for (int i = 0; i < 3; i++) {
            int fy = box.origin.y + box.size.h - 6 - (i * 6);
            graphics_draw_line(ctx, GPoint(box.origin.x + 6, fy),
                                GPoint(box.origin.x + box.size.w - 6, fy));
        }
    }
}

// ============================================================================
// MAIN CANVAS DRAWING
// ============================================================================

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (!s_time_valid) return;

    // --- Time: bare bold hour / minute blocks, flip-clock gap between ---
    static char hour_buf[4];
    static char min_buf[4];
    strftime(hour_buf, sizeof(hour_buf), clock_is_24h_style() ? "%H" : "%I", &s_time);
    strftime(min_buf, sizeof(min_buf), "%M", &s_time);

    GRect hour_rect = GRect(0, 12, bounds.size.w / 2 - 6, 74);
    GRect min_rect = GRect(bounds.size.w / 2 + 6, 12, bounds.size.w / 2 - 6, 74);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, hour_buf, s_time_font, hour_rect,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, min_buf, s_time_font, min_rect,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // --- Weather icon ---
    GRect icon_box = GRect(bounds.size.w / 2 - 38, 92, 76, 72);
    draw_weather_icon(ctx, icon_box, s_weather_valid ? s_weather_code : -1, s_frame);

    // --- Bottom info strip ---
    int bar_top = 172;
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(10, bar_top), GPoint(bounds.size.w - 10, bar_top));

    static char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%a, %b %d", &s_time);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, date_buf, s_date_font, GRect(8, bar_top + 6, 100, 22),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    const char *cond = s_weather_valid ? s_condition : "Loading...";
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, cond, s_small_font, GRect(8, bar_top + 28, 100, 20),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    if (s_weather_valid) {
        static char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%d°", s_temp);
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, temp_buf, s_temp_font, GRect(108, bar_top, 84, 30),
            GTextOverflowModeFill, GTextAlignmentRight, NULL);

        static char hilo_buf[16];
        snprintf(hilo_buf, sizeof(hilo_buf), "%d°/%d°", s_temp_high, s_temp_low);
        graphics_context_set_text_color(ctx, GColorLightGray);
        graphics_draw_text(ctx, hilo_buf, s_small_font, GRect(108, bar_top + 30, 84, 20),
            GTextOverflowModeFill, GTextAlignmentRight, NULL);
    }
}

// ============================================================================
// TIME HANDLING
// ============================================================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    s_time = *tick_time;
    s_time_valid = true;
    s_frame++;
    layer_mark_dirty(s_canvas_layer);

    if (tick_time->tm_min % 30 == 0) {
        request_weather();
    }
}

// ============================================================================
// WINDOW HANDLERS
// ============================================================================

static void window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    time_t now = time(NULL);
    struct tm *now_tm = localtime(&now);
    if (now_tm) {
        s_time = *now_tm;
        s_time_valid = true;
    }

    s_time_font = fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49);
    s_temp_font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
    s_date_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_small_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    request_weather();
}

static void window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
    s_window = window_create();
    window_set_background_color(s_window, GColorBlack);
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload
    });
    window_stack_push(s_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_open(256, 256);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
