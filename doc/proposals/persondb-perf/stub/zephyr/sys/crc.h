#ifndef STUB_CRC_H
#define STUB_CRC_H
#include <stdint.h>
#include <stddef.h>
uint32_t crc32_ieee(const uint8_t *data, size_t len);
uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len);
#endif
