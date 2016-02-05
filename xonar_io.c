#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include "xonar_io.h"
#include "xonar.h"

#define DEFINE_WRITE_N(name, n, type) void name ## _write_ ## n    \
    (struct xonar_info *sc, int reg, type data) {                  \
        bus_space_write_ ## n (sc->st, sc->sh, reg, data);}

DEFINE_WRITE_N(cmi8788, 4, uint32_t)
DEFINE_WRITE_N(cmi8788, 2, uint16_t)
DEFINE_WRITE_N(cmi8788, 1, uint8_t )

#define DEFINE_READ_N(name, n, type) type name ## _read_ ## n      \
    (struct xonar_info *sc, int reg) {                             \
        return bus_space_read_ ## n (sc->st, sc->sh, reg);}

DEFINE_READ_N(cmi8788, 4, uint32_t)
DEFINE_READ_N(cmi8788, 2, uint16_t)
DEFINE_READ_N(cmi8788, 1, uint8_t )

#define DEFINE_SETANDCLEAR_N(name, n, type) void name ## _setandclear_ ## n \
    (struct xonar_info *sc, int reg, type set, type clear) {            \
        type val = name ## _read_ ## n (sc, reg);                       \
            val &= ~clear; val |= set;                                  \
            name ## _write_ ## n (sc, reg, val);}

DEFINE_SETANDCLEAR_N (cmi8788, 4, uint32_t)
DEFINE_SETANDCLEAR_N (cmi8788, 2, uint16_t)
DEFINE_SETANDCLEAR_N (cmi8788, 1, uint8_t )

/* Not MPSAFE */
int cmi8788_write_i2c (struct xonar_info *sc, uint8_t codec_num, uint8_t reg,
                       uint8_t data)
{
    int count = 50;

    /* Wait for it to stop being busy */
    while ((cmi8788_read_2(sc, I2C_CTRL) & TWOWIRE_BUSY) && (count > 0)) {
        DELAY(10);
        count--;
    }
    if (count == 0) {
        device_printf (sc->dev, "i2c timeout\n");
        return -1;
    }

    /* first write the Register Address into the MAP register */
    cmi8788_write_1(sc, I2C_MAP, reg);
    /* now write the data */
    cmi8788_write_1(sc, I2C_DATA, data);
    /* select the codec number to address */
    cmi8788_write_1(sc, I2C_ADDR, codec_num);
    DELAY(100);

    return 0;
}

int cmi8788_read_i2c (struct xonar_info *sc, uint8_t codec_num,
                      uint8_t reg)
{
    uint8_t res;
    int count = 50;

    /* Wait for it to stop being busy */
    while ((cmi8788_read_2(sc, I2C_CTRL) & TWOWIRE_BUSY) && (count > 0)) {
        DELAY(10);
        count--;
    }
    if (count == 0) {
        device_printf (sc->dev, "i2c timeout\n");
        return -1;
    }

    /* first write the Register Address into the MAP register */
    cmi8788_write_1(sc, I2C_MAP, reg);
    /* select the codec number to address */
    cmi8788_write_1(sc, I2C_ADDR, codec_num | 0x1);
    DELAY (100);
    /* now read the data */
    res = cmi8788_read_1(sc, I2C_DATA);

    return res;
}

uint32_t xonar_ac97_read (struct xonar_info *sc, int which, int reg)
{
    uint32_t val;

    val = 0;
    val |= reg << 16;
    val |= 1 << 23;     /*ac97 read the reg address */
    val |= which << 24;
    cmi8788_write_4 (sc, AC97_CMD_DATA, val);
    DELAY (200);
    val = cmi8788_read_4 (sc, AC97_CMD_DATA) & 0xFFFF;
    return val;
}

void xonar_ac97_write (struct xonar_info *sc, int which, int reg, uint32_t data)
{
    uint32_t val;

    val = 0;
    val |= reg << 16;
    val |= 0 << 23;     /*ac97 read the reg address */
    val |= which << 24;
    val |= data & 0xFFFF;
    cmi8788_write_4 (sc, AC97_CMD_DATA, val);
    DELAY (200);
}
