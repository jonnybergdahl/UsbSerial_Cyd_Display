#pragma once

/* Tell LVGL we provide our own config */
#define LV_CONF_H 1

/* Color depth: TFT_eSPI with ILI9341 is typically RGB565 */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

/* Enable what you use */
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* Fonts (pick what you actually need) */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Optional: reduce feature set to speed compile a bit */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1
