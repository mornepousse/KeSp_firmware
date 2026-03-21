#ifndef BOARD_POSITION_MAP_H
#define BOARD_POSITION_MAP_H

#include <stdint.h>

// Mapping table: v1_to_v2[v1_row][v1_col] = v2_row * 16 + v2_col (packed)
// If value is 0xFF, position doesn't exist in other version
static const uint8_t v1_to_v2_map[5][13] = {
    // V1 row0: ESC,ENT,2,3,4,5,6,7,8,9,TO_L2,INT3,LBRC -> V2 positions
    {4*16+0, 4*16+1, 0*16+2, 0*16+3, 0*16+4, 0*16+5, 0*16+7, 0*16+8, 0*16+9, 0*16+10, 4*16+12, 0xFF, 0*16+6},
    // V1 row1: DEL,1,COMM,DOT,P,Y,F,G,C,R,0,EQUAL,MO_L1
    {0*16+0, 0*16+1, 1*16+2, 1*16+3, 1*16+4, 1*16+5, 1*16+7, 1*16+8, 1*16+9, 1*16+10, 0*16+11, 0*16+12, 1*16+6},
    // V1 row2: TAB,QUOT,O,E,U,I,D,H,T,N,L,SLSH,RBRC
    {1*16+0, 1*16+1, 2*16+2, 2*16+3, 2*16+4, 2*16+5, 2*16+7, 2*16+8, 2*16+9, 2*16+10, 1*16+11, 1*16+12, 2*16+6},
    // V1 row3: RALT,A,Q,J,K,X,B,M,W,V,S,MINUS,RSHIFT
    {2*16+0, 2*16+1, 3*16+2, 3*16+3, 3*16+4, 3*16+5, 3*16+7, 3*16+8, 3*16+9, 3*16+10, 2*16+11, 2*16+12, 0xFF},
    // V1 row4: LCTRL,SCLN,LALT,LWIN,LSHIFT,SPACE,BSPACE,ENTER,BSLSH,Z,Z,GRV,NO
    {3*16+0, 3*16+1, 4*16+2, 3*16+6, 4*16+4, 4*16+5, 4*16+7, 4*16+8, 4*16+9, 3*16+11, 3*16+11, 3*16+12, 0xFF}
};

// Reverse mapping: v2_to_v1[v2_row][v2_col] = v1_row * 16 + v1_col
static const uint8_t v2_to_v1_map[5][13] = {
    // V2 row0: DEL,1,2,3,4,5,LBRC,6,7,8,9,0,EQL
    {1*16+0, 1*16+1, 0*16+2, 0*16+3, 0*16+4, 0*16+5, 0*16+12, 0*16+6, 0*16+7, 0*16+8, 0*16+9, 1*16+10, 1*16+11},
    // V2 row1: TAB,QUOT,COMM,DOT,P,Y,MO_L2,F,G,C,R,L,SLSH
    {2*16+0, 2*16+1, 1*16+2, 1*16+3, 1*16+4, 1*16+5, 1*16+12, 1*16+6, 1*16+7, 1*16+8, 1*16+9, 2*16+10, 2*16+11},
    // V2 row2: RALT,A,O,E,U,I,RBRC,D,H,T,N,S,MINUS
    {3*16+0, 3*16+1, 2*16+2, 2*16+3, 2*16+4, 2*16+5, 2*16+12, 2*16+6, 2*16+7, 2*16+8, 2*16+9, 3*16+10, 3*16+11},
    // V2 row3: LCTRL,SCLN,Q,J,K,X,LWIN,B,M,W,V,Z,GRV
    {4*16+0, 4*16+1, 3*16+2, 3*16+3, 3*16+4, 3*16+5, 4*16+3, 3*16+6, 3*16+7, 3*16+8, 3*16+9, 4*16+9, 4*16+11},
    // V2 row4: ESC,ENTER,LALT,LWIN,LSHIFT,SPACE,MO_L2,BSPACE,ENTER,BSLSH,RWIN,HELP,TO_L3
    {0*16+0, 0*16+1, 4*16+2, 4*16+3, 4*16+4, 4*16+5, 0xFF, 4*16+6, 4*16+7, 4*16+8, 0xFF, 0xFF, 0*16+10}
};

// Get V1 internal position from V2 CDC position
static inline void v2_to_v1_pos(int v2_row, int v2_col, int *v1_row, int *v1_col) {
    uint8_t packed = v2_to_v1_map[v2_row][v2_col];
    if (packed == 0xFF) {
        *v1_row = v2_row;
        *v1_col = v2_col;
    } else {
        *v1_row = packed >> 4;
        *v1_col = packed & 0x0F;
    }
}

// Get V2 CDC position from V1 internal position
static inline void v1_to_v2_pos(int v1_row, int v1_col, int *v2_row, int *v2_col) {
    uint8_t packed = v1_to_v2_map[v1_row][v1_col];
    if (packed == 0xFF) {
        *v2_row = v1_row;
        *v2_col = v1_col;
    } else {
        *v2_row = packed >> 4;
        *v2_col = packed & 0x0F;
    }
}

#endif /* BOARD_POSITION_MAP_H */
