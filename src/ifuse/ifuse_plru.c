#include <stdint.h>

#include "ifuse_plru.h"

static void ifuse_plru_touch_4way(uint8_t* plru_state, unsigned int set_idx,
                                  unsigned int way) {
    uint8_t state = plru_state[set_idx] & 0x07U;

    switch (way) {
        case 0U:
            state |= 0x03U; /* bit0=1, bit1=1 */
            break;
        case 1U:
            state |= 0x01U; /* bit0=1, bit1=0 */
            state &= (uint8_t)~0x02U;
            break;
        case 2U:
            state |= 0x04U; /* bit0=0, bit2=1 */
            state &= (uint8_t)~0x01U;
            break;
        case 3U:
            state &= (uint8_t)~0x05U; /* bit0=0, bit2=0 */
            break;
        default:
            return;
    }

    plru_state[set_idx] = state;
}

static unsigned int ifuse_plru_victim_4way(const uint8_t* plru_state,
                                           unsigned int set_idx) {
    uint8_t state = plru_state[set_idx] & 0x07U;

    if ((state & 0x1U) != 0U) {
        return ((state & 0x2U) != 0U) ? 0U : 1U;
    }
    return ((state & 0x4U) != 0U) ? 2U : 3U;
}

/**
 * Tree layout for 8-way PLRU:
 *           bit0
 *          /    \
 *       bit1    bit2
 *      /  \    /  \
 *   bit3 bit4 bit5 bit6
 *   W0 W1 W2 W3 W4 W5 W6 W7
 */
static void ifuse_plru_touch_8way(uint8_t* plru_state, unsigned int set_idx,
                                  unsigned int way) {
    uint8_t state = plru_state[set_idx] & 0x7FU;

    switch (way) {
        case 0U:
            state |= 0x0BU; /* bit0=1, bit1=1, bit3=1 */
            break;
        case 1U:
            state |= 0x03U; /* bit0=1, bit1=1 */
            state &= (uint8_t)~0x08U; /* bit3=0 */
            break;
        case 2U:
            state |= 0x11U; /* bit0=1, bit4=1 */
            state &= (uint8_t)~0x02U; /* bit1=0 */
            break;
        case 3U:
            state |= 0x01U; /* bit0=1 */
            state &= (uint8_t)~0x12U; /* bit1=0, bit4=0 */
            break;
        case 4U:
            state |= 0x24U; /* bit2=1, bit5=1 */
            state &= (uint8_t)~0x01U; /* bit0=0 */
            break;
        case 5U:
            state |= 0x04U; /* bit2=1 */
            state &= (uint8_t)~0x21U; /* bit0=0, bit5=0 */
            break;
        case 6U:
            state |= 0x40U; /* bit6=1 */
            state &= (uint8_t)~0x05U; /* bit0=0, bit2=0 */
            break;
        case 7U:
            state &= (uint8_t)~0x45U; /* bit0=0, bit2=0, bit6=0 */
            break;
        default:
            return;
    }

    plru_state[set_idx] = state;
}

static unsigned int ifuse_plru_victim_8way(const uint8_t* plru_state,
                                           unsigned int set_idx) {
    uint8_t state = plru_state[set_idx] & 0x7FU;

    if ((state & 0x1U) != 0U) {
        if ((state & 0x2U) != 0U) {
            return ((state & 0x8U) != 0U) ? 0U : 1U;
        }
        return ((state & 0x10U) != 0U) ? 2U : 3U;
    }
    if ((state & 0x4U) != 0U) {
        return ((state & 0x20U) != 0U) ? 4U : 5U;
    }
    return ((state & 0x40U) != 0U) ? 6U : 7U;
}

void ifuse_plru_touch(uint8_t* plru_state, unsigned int set_idx,
                      unsigned int way, unsigned int num_ways) {
    if (num_ways == 4U) {
        ifuse_plru_touch_4way(plru_state, set_idx, way);
        return;
    }
    if (num_ways == 8U) {
        ifuse_plru_touch_8way(plru_state, set_idx, way);
    }
}

unsigned int ifuse_plru_victim(const uint8_t* plru_state, unsigned int set_idx,
                               unsigned int num_ways) {
    if (num_ways == 4U) {
        return ifuse_plru_victim_4way(plru_state, set_idx);
    }
    if (num_ways == 8U) {
        return ifuse_plru_victim_8way(plru_state, set_idx);
    }
    return 0U;
}
