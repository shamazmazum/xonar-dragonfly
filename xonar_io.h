#ifndef XONAR_IO_H
#define XONAR_IO_H

#include "xonar.h"

extern void cmi8788_write_4 (struct xonar_info *sc, int reg, u_int32_t data);
extern void cmi8788_write_2 (struct xonar_info *sc, int reg, u_int16_t data);
extern void cmi8788_write_1 (struct xonar_info *sc, int reg, u_int8_t  data);

extern uint32_t cmi8788_read_4 (struct xonar_info *sc, int reg);
extern uint16_t cmi8788_read_2 (struct xonar_info *sc, int reg);
extern uint8_t  cmi8788_read_1 (struct xonar_info *sc, int reg);

extern void cmi8788_setandclear_4 (struct xonar_info *sc, int reg,
                                   u_int32_t set, u_int32_t clear);
extern void cmi8788_setandclear_2 (struct xonar_info *sc, int reg,
                                   u_int16_t set, u_int16_t clear);
extern void cmi8788_setandclear_1( struct xonar_info *sc, int reg,
                                   u_int8_t set, u_int8_t clear);

extern int cmi8788_write_i2c (struct xonar_info *sc, uint8_t codec_num,
                              uint8_t reg, uint8_t data, uint8_t *sync);
#endif
