/* Generic board contract for boards/conchodytes — see test/board_contract.inc. */
#include "test_framework.h"
#include "driver/gpio.h"        /* host stub: GPIO_NUM_* */
#ifndef SPI2_HOST
#define SPI2_HOST 1
#define SPI3_HOST 2
#endif
#include "../boards/conchodytes/board.h"
#define BOARD_CONTRACT_NAME "conchodytes"
#define BOARD_CONTRACT_FN   test_board_contract_conchodytes
#include "board_contract.inc"
