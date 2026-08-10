/**
 * Sense Flip Clock — Pebble Time 2 (Emery, 200x228)
 *
 * Inspired by the HTC Sense flip-clock widget: bare digits for hour/minute
 * (no card background) set in Open Sans Light (SIL Open Font License —
 * safe to redistribute in a published app, unlike the earlier Segoe UI
 * test font), a colour weather icon bitmap in the middle, and a bottom
 * info strip with date/conditions + temperature/hi-lo.
 *
 * Battery-efficient: redraws on MINUTE_UNIT only. The weather icon bitmap
 * is only (re)loaded when new weather data arrives (~every 30 min), not
 * on every draw.
 *
 * Light/dark mode (EXPERIMENTAL): a Clay settings page toggle inverts the
 * background/time/date/temperature colors. Weather icon bitmaps are drawn
 * unmodified either way — the toggle never touches their pixel data.
 */

#include <pebble.h>

#define SETTINGS_KEY 1

// ============================================================================
// GLOBAL STATE
// ============================================================================

static Window *s_window;
static Layer *s_canvas_layer;

static bool s_light_mode = false;

static GFont s_time_font;
static GFont s_temp_font;
static GFont s_date_font;
static GFont s_small_font;

static struct tm s_time;
static bool s_time_valid = false;

static bool s_weather_valid = false;
static int s_weather_code = -1;
static bool s_is_day = true;
static int s_temp = 0;
static int s_temp_high = 0;
static int s_temp_low = 0;
static char s_condition[16] = "";
static GBitmap *s_weather_bitmap = NULL;

// ============================================================================
// SETTINGS
// ============================================================================

static void load_settings(void) {
    s_light_mode = persist_exists(SETTINGS_KEY) ? persist_read_bool(SETTINGS_KEY) : false;
}

static void save_settings(void) {
    persist_write_bool(SETTINGS_KEY, s_light_mode);
}

// ============================================================================
// WEATHER
// ============================================================================

// Maps a raw WMO weather_code (+ day/night) to a bitmap resource ID.
// Night variants only exist for a subset of codes (clear/cloudy/fog/rain/
// snow) — everything else falls back to its day icon regardless of s_is_day.
// NOTE: the "71 + 86 + 87" night icon filename didn't match a real WMO code
// (87 isn't one) — treated here as 71/85/86 to mirror the "85 + 86" day
// icon; flag if that assumption is wrong.
static uint32_t resource_for_weather(int code, bool is_day) {
    if (!is_day) {
        switch (code) {
            case 0: return RESOURCE_ID_ICON_0_NIGHT;
            case 1: return RESOURCE_ID_ICON_1_NIGHT;
            case 2: return RESOURCE_ID_ICON_2_NIGHT;
            case 3: return RESOURCE_ID_ICON_3_NIGHT;
            case 45:
            case 48: return RESOURCE_ID_ICON_45_48_NIGHT;
            case 61:
            case 63:
            case 65: return RESOURCE_ID_ICON_61_63_65_NIGHT;
            case 71:
            case 85:
            case 86: return RESOURCE_ID_ICON_71_85_86_NIGHT;
            default: break;
        }
    }

    switch (code) {
        case 0: return RESOURCE_ID_ICON_0;
        case 1: return RESOURCE_ID_ICON_1;
        case 2: return RESOURCE_ID_ICON_2;
        case 3: return RESOURCE_ID_ICON_3;
        case 45: return RESOURCE_ID_ICON_45;
        case 48: return RESOURCE_ID_ICON_48;
        case 51:
        case 53: return RESOURCE_ID_ICON_51_53;
        case 55: return RESOURCE_ID_ICON_55;
        case 56:
        case 57: return RESOURCE_ID_ICON_56_57;
        case 61:
        case 63:
        case 65: return RESOURCE_ID_ICON_61_63_65;
        case 66:
        case 67: return RESOURCE_ID_ICON_66_67;
        case 71: return RESOURCE_ID_ICON_71;
        case 73: return RESOURCE_ID_ICON_73;
        case 75: return RESOURCE_ID_ICON_75;
        case 77: return RESOURCE_ID_ICON_77;
        case 80:
        case 81:
        case 82: return RESOURCE_ID_ICON_80_81_82;
        case 85:
        case 86: return RESOURCE_ID_ICON_85_86;
        case 95:
        case 96:
        case 99: return RESOURCE_ID_ICON_95_96_99;
        default: return RESOURCE_ID_ICON_2;
    }
}

static void update_weather_bitmap(void) {
    if (s_weather_bitmap) {
        gbitmap_destroy(s_weather_bitmap);
        s_weather_bitmap = NULL;
    }
    s_weather_bitmap = gbitmap_create_with_resource(resource_for_weather(s_weather_code, s_is_day));
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
    Tuple *code_tuple = dict_find(iterator, MESSAGE_KEY_WEATHER_CODE);
    Tuple *day_tuple = dict_find(iterator, MESSAGE_KEY_IS_DAY);
    Tuple *light_mode_tuple = dict_find(iterator, MESSAGE_KEY_LIGHT_MODE);

    if (temp_tuple && cond_tuple && code_tuple) {
        s_temp = (int)temp_tuple->value->int32;
        if (high_tuple) s_temp_high = (int)high_tuple->value->int32;
        if (low_tuple) s_temp_low = (int)low_tuple->value->int32;
        snprintf(s_condition, sizeof(s_condition), "%s", cond_tuple->value->cstring);
        s_weather_code = (int)code_tuple->value->int32;
        s_is_day = day_tuple ? (day_tuple->value->int32 != 0) : true;
        update_weather_bitmap();
        s_weather_valid = true;
        layer_mark_dirty(s_canvas_layer);
    }

    if (light_mode_tuple) {
        s_light_mode = (light_mode_tuple->value->int32 != 0);
        save_settings();
        window_set_background_color(s_window, s_light_mode ? GColorWhite : GColorBlack);
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
// MAIN CANVAS DRAWING
// ============================================================================

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    // Weather icon bitmaps are drawn unmodified further down regardless of
    // this — only these two text/background colors ever flip.
    GColor bg_color = s_light_mode ? GColorWhite : GColorBlack;
    GColor primary_color = s_light_mode ? GColorBlack : GColorWhite;
    GColor secondary_color = s_light_mode ? GColorDarkGray : GColorLightGray;

    graphics_context_set_fill_color(ctx, bg_color);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (!s_time_valid) return;

    // --- Time: hour : minute, tight against a centered colon ---
    static char hour_buf[4];
    static char min_buf[4];
    strftime(hour_buf, sizeof(hour_buf), clock_is_24h_style() ? "%H" : "%I", &s_time);
    strftime(min_buf, sizeof(min_buf), "%M", &s_time);

    GRect hour_rect = GRect(0, 12, 90, 74);
    GRect colon_rect = GRect(90, 12, 20, 74);
    GRect min_rect = GRect(110, 12, 90, 74);

    graphics_context_set_text_color(ctx, primary_color);
    graphics_draw_text(ctx, hour_buf, s_time_font, hour_rect,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, ":", s_time_font, colon_rect,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, min_buf, s_time_font, min_rect,
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // --- Weather icon (unaffected by light/dark mode) ---
    if (s_weather_bitmap) {
        GRect icon_box = GRect(bounds.size.w / 2 - 38, 90, 76, 76);
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, s_weather_bitmap, icon_box);
    }

    // --- Bottom info strip ---
    // Column widths measured against Open Sans Light's actual glyph
    // widths (not Segoe UI's) — worst-case date strings need ~101px at
    // 18pt, worst-case hi/lo pairs need ~70px at 16pt.
    int bar_top = 172;
    graphics_context_set_stroke_color(ctx, GColorDarkGray); // reads fine on black or white, no need to flip
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(6, bar_top), GPoint(bounds.size.w - 6, bar_top));

    static char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%a, %b %d", &s_time);
    graphics_context_set_text_color(ctx, primary_color);
    graphics_draw_text(ctx, date_buf, s_date_font, GRect(6, bar_top + 6, 110, 22),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    const char *cond = s_weather_valid ? s_condition : "Loading...";
    graphics_context_set_text_color(ctx, secondary_color);
    graphics_draw_text(ctx, cond, s_small_font, GRect(6, bar_top + 28, 110, 20),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    if (s_weather_valid) {
        static char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%d°", s_temp);
        graphics_context_set_text_color(ctx, primary_color);
        graphics_draw_text(ctx, temp_buf, s_temp_font, GRect(120, bar_top, 74, 30),
            GTextOverflowModeFill, GTextAlignmentRight, NULL);

        static char hilo_buf[16];
        snprintf(hilo_buf, sizeof(hilo_buf), "%d°/%d°", s_temp_high, s_temp_low);
        graphics_context_set_text_color(ctx, secondary_color);
        graphics_draw_text(ctx, hilo_buf, s_small_font, GRect(120, bar_top + 30, 74, 20),
            GTextOverflowModeFill, GTextAlignmentRight, NULL);
    }
}

// ============================================================================
// TIME HANDLING
// ============================================================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    s_time = *tick_time;
    s_time_valid = true;
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

    s_time_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_OPENSANS_TIME_58));
    s_temp_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_OPENSANS_TEMP_30));
    s_date_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_OPENSANS_DATE_18));
    s_small_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_OPENSANS_SMALL_16));

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    request_weather();
}

static void window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
    if (s_weather_bitmap) {
        gbitmap_destroy(s_weather_bitmap);
        s_weather_bitmap = NULL;
    }
    fonts_unload_custom_font(s_time_font);
    fonts_unload_custom_font(s_temp_font);
    fonts_unload_custom_font(s_date_font);
    fonts_unload_custom_font(s_small_font);
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
    load_settings();

    s_window = window_create();
    window_set_background_color(s_window, s_light_mode ? GColorWhite : GColorBlack);
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
