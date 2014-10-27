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
                       uint8_t data, uint8_t *sync)
{
	int count = 50;

	/* Wait for it to stop being busy */
	while ((cmi8788_read_2(sc, I2C_CTRL) & TWOWIRE_BUSY) && (count > 0)) {
		DELAY(10);
		count--;
	}
	if (count == 0) {
        device_printf (sc->dev, "i2c timeout\n");
		return EIO;
	}

	/* first write the Register Address into the MAP register */
	cmi8788_write_1(sc, I2C_MAP, reg);

	/* now write the data */
	cmi8788_write_1(sc, I2C_DATA, data);

	/* select the codec number to address */
	cmi8788_write_1(sc, I2C_ADDR, codec_num);
	DELAY(100);

    if (sync != NULL) *sync = data;

	return 0;
}
