/*
 * Per-chip PVT (process/voltage/temperature) sensor reading, via the
 * TVSENW (select/arm) and TVSENR (result) registers. Arms a conversion
 * with a select-mode write burst, polls the result register for the
 * ready bit, then decodes the raw temperature/voltage value.
 *
 * Organized around these operations:
 *   sensor selection + conversion start -- a3197s_pvt_select_mode(). One
 *     hardware-coupled operation, not two: selecting a new sel/mode
 *     requires re-latching under the old state, then re-latching and
 *     re-pulsing go/start under the new state (see its own comment).
 *   ready polling            -- pvt_sample_with_retry()
 *   raw extraction            -- pvt_vcore_update()/pvt_tcore_update()
 *   voltage conversion        -- volt_decode()
 *   temperature conversion    -- temp_decode()
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "util.h"
#include "asic_engine.h"

#define PVT_MODE_VOLTAGE 0
#define PVT_MODE_TEMP    1

/* Both temperature and voltage sampling use PVT channel selector 2. */
#define PVT_CHANNEL_SEL 2

static uint8_t sel_state[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
static uint8_t mode_state[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
static uint16_t raw_temp[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
static uint16_t raw_volt[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];

/* One TVSENW write using the chip's CURRENT sel/mode state plus the
 * given control bits (bit2=start, bit1=go -- see the call sites for the
 * exact sequence each transition needs). */
static void write_tvsenw(uint8_t miner_id, uint16_t asic_id, uint32_t ctrl_bits)
{
        a3197s_set_reg(miner_id, REG_PVT_SELECT,
                DEFAULT_PVT_SELECT | (sel_state[miner_id][asic_id] << 3) |
                ctrl_bits | mode_state[miner_id][asic_id]);
}

/* Arms the PVT sensor: latches the old sel/mode, updates state, latches
 * the new sel/mode, then pulses go and start+go. */
static void a3197s_pvt_select_mode(uint8_t miner_id, uint8_t mode, uint16_t asic_id, uint8_t sel)
{
        write_tvsenw(miner_id, asic_id, 0);   /* latch under OLD sel/mode */

        mode_state[miner_id][asic_id] = mode;
        sel_state[miner_id][asic_id] = sel;

        write_tvsenw(miner_id, asic_id, 0);   /* latch under NEW sel/mode */
        write_tvsenw(miner_id, asic_id, 2);   /* bit1 (go) */
        write_tvsenw(miner_id, asic_id, 6);   /* bit2+bit1 (start+go) */
}

/* Raw TVSENR code -> degrees C, quartic polynomial. Coefficients are
 * hardware-calibration behavior determined experimentally against this
 * chip family -- no vendor datasheet or derivation is known/cited for
 * them. Do not change these values; if they're ever revisited, that
 * has to be re-validated against real chip readings, not derived from
 * first principles. */
static double temp_decode(uint16_t raw)
{
        double temp;
        double term1, term2, term3, term4;

        term1 = pow(raw, 4) * pow(10, -10) *  2.28746;
        term2 = pow(raw, 3) * pow(10,  -6) * -2.84236;
        term3 = pow(raw, 2) * pow(10,  -2) *  1.31753;
        term4 = -26.70380 * raw + 19725.61;

        temp = term1 + term2 + term3 + term4;

        if ((temp < -50) || (temp > 200))
                temp = -273;

        return temp;
}

/* Raw TVSENR code -> mV, linear. Same provenance note as temp_decode():
 * experimentally-derived hardware calibration constants, no known
 * datasheet citation, do not change without re-validating against real
 * chip readings. */
static double volt_decode(uint16_t raw)
{
        if (raw < 14)
                return 0;

        return (raw * 1000 + 19144.2) / 3434.4847;
}

void a3197s_pvt_init(uint8_t miner_id, uint16_t asic_id)
{
        a3197s_select_chip(miner_id, asic_id);
        memset(sel_state[miner_id], 0, sizeof(sel_state[miner_id]));
        memset(mode_state[miner_id], 0, sizeof(mode_state[miner_id]));

        write_tvsenw(miner_id, asic_id, 0);
        write_tvsenw(miner_id, asic_id, 2);
        write_tvsenw(miner_id, asic_id, 2);
        write_tvsenw(miner_id, asic_id, 6);

        memset(raw_temp[miner_id], 0, sizeof(raw_temp[miner_id]));
        memset(raw_volt[miner_id], 0, sizeof(raw_volt[miner_id]));
}

/*
 * Ready-bit retry budget: up to PVT_READY_MAX_RETRY attempts,
 * PVT_READY_RETRY_DELAY_MS apart. TVSENR's ready flag is cleared by the
 * read itself, so each retry re-issues the full select-mode write-burst
 * before re-reading, rather than only re-reading. A retry stops early if
 * the chip's register-read error counter changes during the attempt
 * (transport failure), since that case will not be resolved by waiting
 * longer for the ready bit.
 */
#define PVT_READY_MAX_RETRY 100
#define PVT_READY_RETRY_DELAY_MS 30
#define PVT_READY_BIT 0x01000000u

static uint32_t regread_err_count(uint8_t miner_id)
{
        uint32_t nre, nbl, nov, rre;

        asic_get_errcnt_detail(miner_id, &nre, &nbl, &nov, &rre);
        return rre;
}

static uint32_t pvt_sample_with_retry(uint8_t miner_id, uint16_t asic_id, uint8_t mode)
{
        uint32_t reg;
        uint32_t rre_before;
        int retry;

        a3197s_pvt_select_mode(miner_id, mode, asic_id, PVT_CHANNEL_SEL);
        usleep(700);
        rre_before = regread_err_count(miner_id);
        reg = a3197s_get_reg(miner_id, REG_PVT_RESULT);

        for (retry = 0; !(reg & PVT_READY_BIT) && retry < PVT_READY_MAX_RETRY; retry++) {
                if (regread_err_count(miner_id) != rre_before)
                        break;

                delay_ms(PVT_READY_RETRY_DELAY_MS);
                a3197s_pvt_select_mode(miner_id, mode, asic_id, PVT_CHANNEL_SEL);
                usleep(700);
                rre_before = regread_err_count(miner_id);
                reg = a3197s_get_reg(miner_id, REG_PVT_RESULT);
        }

        return reg;
}

void pvt_vcore_update(uint8_t miner_id, uint16_t asic_id)
{
        uint32_t reg;

        a3197s_select_chip(miner_id, asic_id);
        reg = pvt_sample_with_retry(miner_id, asic_id, PVT_MODE_VOLTAGE);

        raw_volt[miner_id][asic_id] = (reg & PVT_READY_BIT) ? ((reg >> 12) & 0xfff) : 0;
}

void pvt_tcore_update(uint8_t miner_id, uint16_t asic_id)
{
        uint32_t reg;

        a3197s_select_chip(miner_id, asic_id);
        reg = pvt_sample_with_retry(miner_id, asic_id, PVT_MODE_TEMP);

        raw_temp[miner_id][asic_id] = (reg & PVT_READY_BIT) ? (reg & 0xfff) : 0;
}

double pvt_vcore_get(uint8_t miner_id, uint16_t asic_id)
{
        return volt_decode(raw_volt[miner_id][asic_id]);
}

double pvt_tcore_get(uint8_t miner_id, uint16_t asic_id)
{
        return temp_decode(raw_temp[miner_id][asic_id]);
}

double pvt_tcore_avg_get(uint16_t asic_count, double *temp_avgs)
{
        uint16_t board, chip;
        uint16_t total_cnt = 0;
        double total_avg = 0;

        for (board = 0; board < HBOARD_COUNT; board++) {
                double board_avg = 0;
                uint16_t board_cnt = 0;

                for (chip = 0; chip < asic_count; chip++) {
                        board_avg += temp_decode(raw_temp[board][chip]);
                        board_cnt++;
                }

                total_avg += board_avg;
                total_cnt += board_cnt;
                temp_avgs[board] = board_avg / board_cnt;
        }

        if (!total_cnt)
                return 1.0;

        return total_avg / total_cnt;
}
