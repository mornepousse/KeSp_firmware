#include "graphic_manager.h"
#include "display_manager.h"


gtext_t* gtext_create(uint16_t x, uint16_t y, uint16_t color, uint16_t bg_color, char *text) {
  gtext_t *gtext = malloc(sizeof(gtext_t));
  if (gtext == NULL) {
    //ESP_LOGE(TAG_GM, "Failed to allocate memory for gtext");
    return gtext;
  }
  gtext->x = x;
  gtext->y = y;
  gtext->text = text;
  gtext->color = color;
  gtext->bg_color = bg_color;
  gtext->is_changed = 1;
  gtext_count++;
    if (gtext_count > 10) {
        //ESP_LOGE(TAG_GM, "Maximum number of gtext reached");
        free(gtext);
        return gtext;
    }
    gtext[gtext_count - 1] = *gtext;
  return gtext;
}

void gtext_set_text(gtext_t *gtext, char *text) {
  gtext->text = text;
  gtext->is_changed = 1;
}
void gtext_set_color(gtext_t *gtext, uint16_t color) {
  gtext->color = color;
  gtext->is_changed = 1;
}
void gtext_set_bg_color(gtext_t *gtext, uint16_t bg_color) {
  gtext->bg_color = bg_color;
  gtext->is_changed = 1;
}


void graphic_update() {
  for (uint8_t i = 0; i < gtext_count; i++) {
    if (gtext_list[i].is_changed == 1) {
      write_txt(gtext_list[i].text, gtext_list[i].x, gtext_list[i].y);
      gtext_list[i].is_changed = 0;
    }
  }
}