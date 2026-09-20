/* __BOARD_NAME__ — physical layout served to KeSp_controller (JSON, format v2:
 * x/y per key, optional rotation r and width w). A plain grid to start with;
 * see boards/kase_layout.inc for a column-staggered example with thumbs. */
#include "board.h"

#define _STR(x) #x
#define STR(x)  _STR(x)

const char board_layout_json[] =
"{"
  "\"name\":\"" PRODUCT_NAME "\","
  "\"rows\":" STR(MATRIX_ROWS) ",\"cols\":" STR(MATRIX_COLS) ","
  "\"groups\":[{\"x\":0,\"y\":0,\"r\":0,\"cols\":" STR(MATRIX_COLS) ",\"rows\":" STR(MATRIX_ROWS) "}]"
"}";
