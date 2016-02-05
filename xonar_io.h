#ifndef XONAR_IO_H
#define XONAR_IO_H

#include "xonar.h"

void cmi8788_write_4 (struct xonar_info *sc, int reg, u_int32_t data);
void cmi8788_write_2 (struct xonar_info *sc, int reg, u_int16_t data);
void cmi8788_write_1 (struct xonar_info *sc, int reg, u_int8_t  data);

uint32_t cmi8788_read_4 (struct xonar_info *sc, int reg);
uint16_t cmi8788_read_2 (struct xonar_info *sc, int reg);
uint8_t  cmi8788_read_1 (struct xonar_info *sc, int reg);

void cmi8788_setandclear_4 (struct xonar_info *sc, int reg,
                                   u_int32_t set, u_int32_t clear);
void cmi8788_setandclear_2 (struct xonar_info *sc, int reg,
                                   u_int16_t set, u_int16_t clear);
void cmi8788_setandclear_1( struct xonar_info *sc, int reg,
                                   u_int8_t set, u_int8_t clear);

int cmi8788_write_i2c (struct xonar_info *sc, uint8_t codec_num,
                       uint8_t reg, uint8_t data);
int cmi8788_read_i2c (struct xonar_info *sc, uint8_t codec_num,
                      uint8_t reg);

uint32_t xonar_ac97_read (struct xonar_info *sc, int which, int reg);
void xonar_ac97_write (struct xonar_info *sc, int which, int reg, uint32_t data);
#endif
