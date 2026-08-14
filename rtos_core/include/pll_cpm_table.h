#ifndef PLL_CPM_TABLE_H
#define PLL_CPM_TABLE_H

#include <stdint.h>

/* Frequency(MHz)->PLL-register-code lookup table. This is factual
 * calibration data for the A3197S's PLL synthesizer, not project logic --
 * see pll_cpm_table.c's header comment for its licensing status. Kept in
 * its own translation unit, separate from asic_engine.c's control logic,
 * specifically so a notice on this file doesn't have to cover code that
 * doesn't need one. */
#define PLL_TABLE_ROWS 293

extern const uint32_t cpm_table[PLL_TABLE_ROWS][2];

#endif
