#ifndef PVT_SENSOR_H
#define PVT_SENSOR_H

#include <stdint.h>

void a3197s_pvt_init(uint8_t miner_id, uint16_t asic_id);
void pvt_vcore_update(uint8_t miner_id, uint16_t asic_id);
void pvt_tcore_update(uint8_t miner_id, uint16_t asic_id);
double pvt_vcore_get(uint8_t miner_id, uint16_t asic_id);
double pvt_tcore_get(uint8_t miner_id, uint16_t asic_id);
double pvt_tcore_avg_get(uint16_t asic_count, double *temp_avgs);

#endif
