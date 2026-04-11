#include "../kase_v2/board.h"

/* Override GPIOs that differ on the debug board */
#undef COLS7
#define COLS7  GPIO_NUM_21

#undef COLS8
#define COLS8  GPIO_NUM_4

/* Override product info */
#undef GATTS_TAG
#define GATTS_TAG   "KaSe_V2_DBG"

#undef PRODUCT_NAME
#define PRODUCT_NAME  "KaSe V2 Debug"
