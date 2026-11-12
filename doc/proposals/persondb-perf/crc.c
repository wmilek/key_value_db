/* Self-consistent CRCs for the host harness (same functions write and check). */
#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		seed ^= src[i];
		for (int b = 0; b < 8; b++) {
			seed = (seed & 1) ? (seed >> 1) ^ 0x8408 : (seed >> 1);
		}
	}
	return seed;
}

uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < len; i++) {
		c ^= data[i];
		for (int b = 0; b < 8; b++) {
			c = (c & 1) ? (c >> 1) ^ 0xedb88320u : (c >> 1);
		}
	}
	return ~c;
}
