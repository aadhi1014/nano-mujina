#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <unistd.h>

static inline void delay_ms(uint32_t ms)
{
	usleep(ms * 1000);
}

static inline uint32_t bswap_32(uint32_t v)
{
	return ((v & 0x000000ffU) << 24) |
	       ((v & 0x0000ff00U) << 8) |
	       ((v & 0x00ff0000U) >> 8) |
	       ((v & 0xff000000U) >> 24);
}

#endif
