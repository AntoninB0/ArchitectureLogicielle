#ifndef CRC16_H
#define CRC16_H

/* CRC-16/CCITT-FALSE : polynôme 0x1021, valeur initiale 0xFFFF.
 * C pur, sans Zephyr : testé sur PC (tests/). */

#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFF

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc);

#endif
