#ifdef HAVE_KERNEL_OPTION_HEADERS
#include "opt_snd.h"
#endif

#include <dev/sound/pcm/sound.h>
#include <dev/sound/pcm/ac97.h>

#if defined __DragonFly__
#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#elif defined __FreeBSD__
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#else
#error "Platform not supported"
#endif
#include <sys/sysctl.h>
#include <sys/endian.h>

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include "mixer_if.h"
#include "xonar.h"
#include "xonar_compat.h"
#include "xonar_io.h"

#define CHAN_STATE_INIT 	0
#define CHAN_STATE_ACTIVE 	1
#define CHAN_STATE_INACTIVE	2
#define CHAN_STATE_INVALID 	3

#define OUTPUT_LINE 		0
#define OUTPUT_REAR_HP 		1
#define OUTPUT_HP 		2

#if defined __DragonFly__
/* stubs */
#define AFMT_BIT(fmt) 16
#define AFMT_CHANNEL(fmt) 2
#endif

static int xonar_init(struct xonar_info *);
static void xonar_cleanup(struct xonar_info *);

static const struct {
	uint16_t vendor;
	uint16_t devid;
	char *desc;
} xonar_hw[] = {
	/* we actually support only this one, it shouldn't be too hard to add others */
	{ ASUS_VENDOR_ID, SUBID_XONAR_STX, "Asus Xonar Essence STX (AV100)" 	},
	{ ASUS_VENDOR_ID, SUBID_XONAR_ST,  "Asus Xonar Essence ST (AV100)" 	},
#if 0
	{ ASUS_VENDOR_ID, SUBID_XONAR_D1,  "Asus Xonar D1 (AV100)" 		},
	{ ASUS_VENDOR_ID, SUBID_XONAR_DX,  "Asus Xonar DX (AV100)" 		},
	{ ASUS_VENDOR_ID, SUBID_XONAR_D2,  "Asus Xonar D2 (AV200)" 		},
	{ ASUS_VENDOR_ID, SUBID_XONAR_D2X, "Asus Xonar D2X (AV200)" 		},
	{ ASUS_VENDOR_ID, SUBID_XONAR_DS,  "Asus Xonar DS (AV66)" 		},
#endif
};

#if defined __DragonFly__
static u_int32_t xonar_fmt[] = {
	AFMT_S16_LE | AFMT_STEREO,
	AFMT_S24_LE | AFMT_STEREO,
	AFMT_S32_LE | AFMT_STEREO,
	0
};
#elif defined __FreeBSD__
static u_int32_t xonar_fmt[] = {
	SND_FORMAT(AFMT_S16_LE, 2, 0),
	SND_FORMAT(AFMT_S24_LE, 2, 0),
	SND_FORMAT(AFMT_S32_LE, 2, 0),
	0
};
#endif

static struct pcmchan_caps xonar_caps = { 32000, 192000, xonar_fmt, 0 };

/* ST/STX only. Do we have pcm1796 in other cards? */
static void pcm1796_write (struct xonar_info *sc, uint8_t reg, uint8_t data)
{
    uint8_t *sync_reg = &(sc->pcm1796.regs[reg - PCM1796_REGBASE]);
    cmi8788_write_i2c (sc, XONAR_STX_FRONTDAC, reg, data, sync_reg);
}

static int
xonar_ac97_read_mthd (kobj_t obj, void *devinfo, int reg)
{
    return xonar_ac97_read (devinfo, 0, reg);
}

static int
xonar_ac97_write_mthd (kobj_t obj, void *devinfo, int reg, uint32_t data)
{
    xonar_ac97_write (devinfo, 0, reg, data);
    return 0;
}

static kobj_method_t xonar_ac97_methods[] = {
	KOBJMETHOD(ac97_read,		xonar_ac97_read_mthd),
    KOBJMETHOD(ac97_write,		xonar_ac97_write_mthd),
	KOBJMETHOD_END
};
AC97_DECLARE(xonar_ac97);

static unsigned int
pcm1796_vol_scale(int vol)
{
	/* 0-14 - mute, 255 - max */
	return (vol * 241)/100;
}

static void
pcm1796_set_volume(struct xonar_info *sc, int left, int right)
{
    pcm1796_write(sc, 16, pcm1796_vol_scale(left));
	pcm1796_write(sc, 17, pcm1796_vol_scale(right));
}

static void
pcm1796_set_mute(struct xonar_info *sc, int mute) 
{
	uint16_t reg = sc->pcm1796.regs[PCM1796_REG18];

	if (mute)
		pcm1796_write(sc, 18, reg | PCM1796_MUTE);
	else
		pcm1796_write(sc, 18, reg & ~PCM1796_MUTE);
}

static int
pcm1796_get_deemph(struct xonar_info *sc)
{
	uint16_t res;

    snd_mtxlock (sc->lock);
    res = (sc->pcm1796.regs[PCM1796_REG18] & PCM1796_DME) ? 1 : 0;
    snd_mtxunlock (sc->lock);
    return res;
}

static void
pcm1796_set_deemph(struct xonar_info *sc, int deemph)
{
	uint16_t reg;
	/* XXX: set DMF */

    snd_mtxlock (sc->lock);
    reg = sc->pcm1796.regs[PCM1796_REG18];
	if (deemph)
		pcm1796_write(sc, 18, reg | PCM1796_DME);
	else
		pcm1796_write(sc, 18, reg & ~PCM1796_DME);
    snd_mtxunlock (sc->lock);
}

static int
pcm1796_get_rolloff(struct xonar_info *sc)
{
	uint16_t res;

    snd_mtxlock (sc->lock);
    res = (sc->pcm1796.regs[PCM1796_REG19] & PCM1796_FLT) ? 1 : 0;
    snd_mtxunlock (sc->lock);
    return res;
}

static void
pcm1796_set_rolloff(struct xonar_info *sc, int rolloff)
{
	uint16_t reg;

    snd_mtxlock (sc->lock);
    reg = sc->pcm1796.regs[PCM1796_REG19];
	if (rolloff)
		pcm1796_write(sc, 19, reg | PCM1796_FLT);
	else
		pcm1796_write(sc, 19, reg & ~PCM1796_FLT);
    snd_mtxunlock (sc->lock);
}

static int
pcm1796_get_bypass(struct xonar_info *sc)
{
	uint16_t res;

    snd_mtxlock (sc->lock);
    res = (sc->pcm1796.regs[PCM1796_REG20] & PCM1796_DFTH) ? 1 : 0;
    snd_mtxunlock (sc->lock);
    return res;
}

static void
pcm1796_set_bypass(struct xonar_info *sc, int bypass)
{
	uint16_t reg;

    snd_mtxlock (sc->lock);
    reg = sc->pcm1796.regs[PCM1796_REG20];
	if (bypass) /* just disables sound */
		pcm1796_write(sc, 20, reg | PCM1796_DFTH);
	else
		pcm1796_write(sc, 20, reg & ~PCM1796_DFTH);
    snd_mtxunlock (sc->lock);
}

static void 
xonar_chan_reset(struct xonar_chinfo *ch, uint8_t which)
{
	struct xonar_info *sc = ch->parent;

    cmi8788_setandclear_1 (sc, CHAN_RESET, which, 0);
    DELAY(10);
    cmi8788_setandclear_1 (sc, CHAN_RESET, 0, which);
}

static int
i2s_get_rate(int rate)
{
	int i2s_rate;

	switch (rate) {
	case 32000:
		i2s_rate = I2S_FMT_RATE32;
		break;
	case 44100:
		i2s_rate = I2S_FMT_RATE44;
		break;
	default:
	case 48000:
		i2s_rate = I2S_FMT_RATE48;
		break;
	case 64000:
		i2s_rate = I2S_FMT_RATE64;
		break;
	case 88200:
		i2s_rate = I2S_FMT_RATE88;
		break;
	case 96000:
		i2s_rate = I2S_FMT_RATE96;
		break;
	case 176400:
		i2s_rate = I2S_FMT_RATE176;
		break;
	case 192000:
		i2s_rate = I2S_FMT_RATE192;
		break;
	}

	return i2s_rate;
}

static int
i2s_get_bits(int bits)
{
	int i2s_bits;

	switch (bits) {
	case AFMT_S24_LE:
		i2s_bits = I2S_FMT_BITS24;
		break;
	case AFMT_S32_LE:
		i2s_bits = I2S_FMT_BITS32;
		break;
	case AFMT_S16_LE:
	default:
		i2s_bits = I2S_FMT_BITS16;
		break;
	}

	return i2s_bits;
}

static void *
xonar_chan_init(kobj_t obj, void *devinfo,
	     struct snd_dbuf *b, struct pcm_channel *c, int dir)
{
	struct xonar_info *sc = devinfo;
	struct xonar_chinfo *ch;
    int offset = (dir == PCMDIR_PLAY) ? 0 : MAX_PORTS_PLAY;
    bus_dma_tag_t dmat = (dir == PCMDIR_PLAY) ? sc->dmats[0] : sc->dmats[1];

	ch = &sc->chan[sc->pnum+offset];
	ch->buffer = b;
	ch->parent = sc;
	ch->channel = c;
	ch->dir = dir;
	ch->blksz = 2048;
	ch->bps = 16;
	switch (sc->pnum) {
	case 0:
	case 1:
		device_printf(sc->dev, "channel%d (Multichannel)\n", sc->pnum);
		ch->dac_type = 1;
		switch (sc->model) {
			case SUBID_XONAR_D1:
			case SUBID_XONAR_DX:
			case SUBID_XONAR_D2:
			case SUBID_XONAR_D2X:
			case SUBID_XONAR_STX:
			case SUBID_XONAR_ST:
				ch->adc_type = 2;
				break;
			case SUBID_XONAR_DS:
				ch->adc_type = 1;
				break;
			default: 
				ch->adc_type = 1;
                cmi8788_setandclear_1 (sc, REC_ROUTING, 0x18, 0);
				break;
		}
		break;
	case 2:
		/* if there is no front panel AC97, then skip the device */
		break;
	case 3:
		device_printf(sc->dev, "channel%d (SPDIF)\n", sc->pnum);
		break;
	}
	if (sc->pnum == 0) {
		if (pcm_sndbuf_alloc(ch->buffer, dmat, 0, sc->bufsz) != 0) {
			device_printf(sc->dev, "Cannot allocate sndbuf\n");
			return NULL;
		}
#if defined __FreeBSD__
		DEB(device_printf(sc->dev, "%s buf %d alignment %d\n", (dir == PCMDIR_PLAY)?
				  "play" : "rec", (uint32_t)sndbuf_getbufaddr(ch->buffer),
						  sndbuf_getalign(ch->buffer)));
#endif
	}
	ch->state = CHAN_STATE_INIT;
	return ch;
}

static struct pcmchan_caps *
xonar_chan_getcaps(kobj_t obj, void *data)
{
	/* XXX: proper caps */
	return &xonar_caps;
}

static u_int32_t
xonar_chan_setspeed(kobj_t obj, void *data, u_int32_t speed)
{
#define GPIO_CS53x1_M_MASK	0x000c
#define GPIO_CS53x1_M_SINGLE	0x0000
#define GPIO_CS53x1_M_DOUBLE	0x0004
#define GPIO_CS53x1_M_QUAD	0x0008
	struct xonar_chinfo *ch = data;
	struct xonar_info *sc = ch->parent;
	int i2s_rate, i2s_rate_where, cs53x1_value;

	i2s_rate = i2s_get_rate(speed);
    i2s_rate_where = 0;
    switch (ch->dir) {
    case PCMDIR_PLAY:
        i2s_rate_where = I2S_MULTICH_FORMAT;
        break;
    case PCMDIR_REC:
        switch (ch->adc_type) {
        case 1:
            i2s_rate_where = I2S_ADC1_FORMAT;
            break;
        case 2:
            i2s_rate_where = I2S_ADC2_FORMAT;
            break;
        }

        if (speed <= 54000) cs53x1_value = GPIO_CS53x1_M_SINGLE;
        else if (speed <= 108000) cs53x1_value = GPIO_CS53x1_M_DOUBLE;
        else cs53x1_value = GPIO_CS53x1_M_QUAD;
        cmi8788_setandclear_2(sc, GPIO_DATA, cs53x1_value, GPIO_CS53x1_M_MASK);
        break;
    }

    if (i2s_rate_where) {
        ch->spd = speed;
        cmi8788_setandclear_1 (sc, i2s_rate_where, i2s_rate, I2S_FMT_RATE_MASK);
    }
	return ch->spd;
}

static int
xonar_chan_setformat(kobj_t obj, void *data, u_int32_t format)
{
	struct xonar_chinfo *ch = data;
	struct xonar_info *sc = ch->parent;
	int bits, bits_where;
    bits_where = 0;

	DEB(device_printf(sc->dev, "%s %dbits %dchans\n", __func__, AFMT_BIT(format),
					  AFMT_CHANNEL(format)));

	if (format & AFMT_S32_LE)
		bits = 8;
	else if (format & AFMT_S16_LE)
		bits = 0;
	else if (format & AFMT_S24_LE)
		bits = 4;
	else {
		kern_printf("format unknown\n");
		return 1;
	}

    switch (ch->dir) {
    case PCMDIR_PLAY:
        bits_where = PLAY_FORMAT;
        break;
    case PCMDIR_REC:
        bits_where = REC_FORMAT;
        break;
    }

    if (bits_where) {
        ch->fmt = format;
        ch->bps = bits;
        cmi8788_setandclear_1 (sc, bits_where, bits, MULTICH_FORMAT_MASK);
    }

	return 0;
}

static void
xonar_prepare_input(struct xonar_chinfo *ch)
{
	struct xonar_info *sc = ch->parent;
	int i2s_bits;
	uint32_t addr = sndbuf_getbufaddr(ch->buffer);

	switch (ch->adc_type) {
	case 2:
		ch->dma_start = 0x2;
		ch->irq_mask = 0x2;
		xonar_chan_reset(ch, 0x2);

		DEB(device_printf(sc->dev, "buffer addr = 0x%x size = %d\n",
						  addr, sndbuf_getsize(ch->buffer)));

		cmi8788_write_4(sc, RECB_ADDR, addr);
		cmi8788_write_2(sc, RECB_SIZE, sc->bufsz / 4 - 1);
		cmi8788_write_2(sc, RECB_FRAG, 1024 / 4 /* XXX */ - 1);

        /* Vasily: Should we move it to xonar_set_format? */
		/* setup i2s bits in the i2s register */
		i2s_bits = i2s_get_bits (ch->fmt);
        cmi8788_setandclear_1 (sc, I2S_ADC2_FORMAT, i2s_bits, I2S_BITS_MASK);
		break;
	default:
		break;
	}
}

static void
xonar_prepare_output(struct xonar_chinfo *ch)
{
	struct xonar_info *sc = ch->parent;
	int i2s_bits;
	uint32_t addr = sndbuf_getbufaddr(ch->buffer);

	switch (ch->dac_type) {
	case 1:
		ch->dma_start = CHANNEL_MULTICH;
		ch->irq_mask = CHANNEL_MULTICH;
		xonar_chan_reset(ch, CHANNEL_MULTICH);

		int channels;
		switch (AFMT_CHANNEL(ch->fmt)) {
		default:
		case 2:
			channels = MULTICH_MODE_2CH;
			break;
		case 4:
			channels = MULTICH_MODE_4CH;
			break;
		case 6:
			channels = MULTICH_MODE_6CH;
			break;
		case 8:
			channels = MULTICH_MODE_8CH;
			break;
		}

		DEB(device_printf(sc->dev, "buffer addr = 0x%x size = %d\n",
						  addr, sndbuf_getsize(ch->buffer)));

		cmi8788_write_4(sc, MULTICH_ADDR, addr);
		cmi8788_write_4(sc, MULTICH_SIZE, sc->bufsz / 4 - 1);
		/* what is this 1024 you ask
		 * i have no idea
		 * oss uses dmap->fragment_size
		 * alsa uses params_period_bytes()
		 */
		cmi8788_write_4(sc, MULTICH_FRAG, 1024 / 4 /* XXX */ - 1);

        cmi8788_setandclear_1 (sc, MULTICH_MODE, channels, MULTICH_MODE_CH_MASK);

		/* setup i2s bits in the i2s register */
		i2s_bits = i2s_get_bits (ch->fmt);
        cmi8788_setandclear_1 (sc, I2S_MULTICH_FORMAT, i2s_bits, I2S_BITS_MASK);
		break;
	default:
		break;
	}
}

static int
xonar_chan_trigger(kobj_t obj, void *data, int go) 
{
	struct xonar_chinfo *ch = data;
	struct xonar_info *sc = ch->parent;
    void (*prepare_func) (struct xonar_chinfo*) =
        (ch->dir == PCMDIR_PLAY) ? xonar_prepare_output : xonar_prepare_input;

	if (!PCMTRIG_COMMON(go))
		return 0;

	if (ch->state == CHAN_STATE_INVALID)
		return EINVAL;

	snd_mtxlock(sc->lock);
	switch (go) {
	case PCMTRIG_START:
		DEB(device_printf(sc->dev, "trigger start\n"));
		DEB(device_printf(sc->dev, "bufsz = %d\n", (int)sc->bufsz));
		DEB(device_printf(sc->dev, "chan state = 0x%x\n", ch->state));
		if (ch->state == CHAN_STATE_ACTIVE)
			break;
		if (ch->state == CHAN_STATE_INIT)
            prepare_func (ch);
		ch->state = CHAN_STATE_ACTIVE;
		/* enable irq */
        cmi8788_setandclear_2 (sc, IRQ_MASK, ch->irq_mask, 0);
		/* enable dma */
        cmi8788_setandclear_2 (sc, DMA_START, ch->dma_start, 0);
		break;

	case PCMTRIG_ABORT:
	case PCMTRIG_STOP:
		DEB(device_printf(sc->dev, "trigger stop\n"));
		if (!(ch->state == CHAN_STATE_ACTIVE))
			break;
		ch->state = CHAN_STATE_INACTIVE;
		/* disable dma */
        cmi8788_setandclear_2 (sc, DMA_START, 0, ch->dma_start);
		/* disable irq */
        cmi8788_setandclear_2 (sc, IRQ_MASK, 0, ch->irq_mask);
		break;
	default:
		break;
	}
	snd_mtxunlock(sc->lock);
	return (0);
}

static u_int32_t 
xonar_chan_setblocksize(kobj_t obj, void *data, u_int32_t blocksize)
{
	struct xonar_chinfo *ch = data;

	if (ch->blksz != blocksize) {
		ch->blksz = blocksize;
		ch->state = CHAN_STATE_INIT;
	}
	return blocksize;
}

static u_int32_t
xonar_chan_getptr(kobj_t obj, void *data)
{
	struct xonar_chinfo *ch = data;
	struct xonar_info *sc = ch->parent;
    int reg = 0;

	switch (ch->dir) {
    case PCMDIR_PLAY:
		switch (ch->dac_type) {
		case 1:
			reg = MULTICH_ADDR;
            break;
		}
        break;
    case PCMDIR_REC:
        switch (ch->adc_type) {
		case 2:
			reg = RECB_ADDR;
            break;
		}
        break;
	}
    if (reg) return cmi8788_read_4(sc, reg);
    else return 0;
}

static kobj_method_t xonar_chan_methods[] = {
    	KOBJMETHOD(channel_init,		xonar_chan_init),
    	KOBJMETHOD(channel_getcaps,		xonar_chan_getcaps),
    	KOBJMETHOD(channel_setformat,		xonar_chan_setformat),
    	KOBJMETHOD(channel_trigger,		xonar_chan_trigger),
    	KOBJMETHOD(channel_setspeed,		xonar_chan_setspeed),
    	KOBJMETHOD(channel_getptr,		xonar_chan_getptr),
    	KOBJMETHOD(channel_setblocksize,	xonar_chan_setblocksize),
	KOBJMETHOD_END
};
CHANNEL_DECLARE(xonar_chan);

/* mixer interface */
static int
xonar_mixer_init(struct snd_mixer *m)
{
	/* struct xonar_info *sc = mix_getdevinfo(m); */

	mix_setdevs(m, SOUND_MASK_VOLUME);
	return (0);
}

static int
xonar_mixer_set(struct snd_mixer *m, unsigned dev, unsigned left, unsigned right)
{
	struct xonar_info *sc = mix_getdevinfo(m);

	snd_mtxlock(sc->lock);
	switch (dev) {
	case SOUND_MIXER_VOLUME:
		sc->vol[0] = left;
		sc->vol[1] = right;
		pcm1796_set_volume(sc, left, right);
		break;
	}
	snd_mtxunlock(sc->lock);
	return (0);
}

static kobj_method_t xonar_mixer_methods[] = {
	KOBJMETHOD(mixer_init, xonar_mixer_init),
	KOBJMETHOD(mixer_set, xonar_mixer_set),
	KOBJMETHOD_END
};
MIXER_DECLARE(xonar_mixer);

static void
cmi8788_toggle_sound(struct xonar_info *sc, int output) {
	if (output) {
        cmi8788_setandclear_2 (sc, GPIO_CONTROL, sc->output_control_gpio, 0);
		tsleep (sc, 0, "apop", sc->anti_pop_delay);
        cmi8788_setandclear_2 (sc, GPIO_DATA, sc->output_control_gpio, 0);
	} else {
		/* Mute DAC before toggle GPIO to avoid another pop */
		pcm1796_set_mute (sc, 1);
        cmi8788_setandclear_2 (sc, GPIO_DATA, 0, sc->output_control_gpio);
		pcm1796_set_mute (sc, 0);
	}
}

/* Bits in monitor register are not clear for me,
   but I think it is enough to set it to 0xf */
static void
cmi8788_set_rec_monitor(struct xonar_info *sc, int set)
{
    snd_mtxlock(sc->lock);
    if (set) cmi8788_setandclear_1 (sc, REC_MONITOR, 0x0f, 0);
    else cmi8788_setandclear_1 (sc, REC_MONITOR, 0, 0x0f);
    snd_mtxunlock(sc->lock);
}

static int
cmi8788_get_rec_monitor(struct xonar_info *sc)
{
    int res;
    snd_mtxlock(sc->lock);
    res = (cmi8788_read_1 (sc, REC_MONITOR) & 0x0f) ? 1: 0;
    snd_mtxunlock(sc->lock);
    return res;
}

static void
cmi8788_set_output(struct xonar_info *sc, int which)
{
    snd_mtxlock(sc->lock);
    cmi8788_toggle_sound(sc, 0);
    switch (sc->model) {
    case SUBID_XONAR_ST:
    case SUBID_XONAR_STX:
        /*
         * GPIO1 - front (0) or rear (1) HP jack
         * GPIO7 - speakers (0) or HP (1)
         */
        switch (which) {
        case OUTPUT_LINE:
            cmi8788_setandclear_2 (sc, GPIO_DATA, 0, GPIO_PIN7|GPIO_PIN1);
            break;
        case OUTPUT_REAR_HP:
            cmi8788_setandclear_2 (sc, GPIO_DATA, GPIO_PIN7|GPIO_PIN1, 0);
            break;
        case OUTPUT_HP:
            cmi8788_setandclear_2 (sc, GPIO_DATA, GPIO_PIN7, GPIO_PIN1);
            break;
        }
        break;
    }
    cmi8788_toggle_sound(sc, 1);
    snd_mtxunlock(sc->lock);
}

static int
cmi8788_get_output(struct xonar_info *sc)
{
    uint16_t val;
    int res = -1;

    snd_mtxlock(sc->lock);
    switch (sc->model) {
    case SUBID_XONAR_ST:
    case SUBID_XONAR_STX:
        val = cmi8788_read_2(sc, GPIO_DATA);
        if (!(val & GPIO_PIN7)) res = OUTPUT_LINE;
        else if (! (val & GPIO_PIN1)) res = OUTPUT_HP;
        else res = OUTPUT_REAR_HP;
        break;
    }
    snd_mtxunlock(sc->lock);

    return res;
}

/* Copied from OSS driver */
static void
ac97_init (struct xonar_info *sc)
{
    /* Gpio #0 programmed as output, set CMI9780 Reg0x70 */
    xonar_ac97_write(sc, 0, 0x70, 0x100);

    /* LI2LI,MIC2MIC; let them always on, FOE on, ROE/BKOE/CBOE off */
    xonar_ac97_write(sc, 0, 0x62, 0x180F);

    /* change PCBeep path, set Mix2FR on, option for quality issue */
    xonar_ac97_write(sc, 0, 0x64, 0x8043);
#if 0
   /* unmute Master Volume */
    xonar_ac97_write(sc, 0, 0x02, 0x0);

    /* mute PCBeep, option for quality issues */
    xonar_ac97_write(sc, 0, 0x0A, 0x8000);

    /* Record Select Control Register (Index 1Ah) */
    xonar_ac97_write(sc, 0, 0x1A, 0x0000);

    /* set Mic Volume Register 0x0Eh umute and enable micboost */
    xonar_ac97_write(sc, 0, 0x0E, 0x0848);

    /* set Line in Volume Register 0x10h mute */
    xonar_ac97_write(sc, 0, 0x10, 0x8808);

    /* set CD Volume Register 0x12h mute */
    xonar_ac97_write(sc, 0, 0x12, 0x8808);

    /* set AUX Volume Register 0x16h max */
    xonar_ac97_write(sc, 0, 0x16, 0x0808);

    /* set record gain Register 0x1Ch to max */
    xonar_ac97_write(sc, 0, 0x1C, 0x0F0F);
#endif
    xonar_ac97_write(sc, 0, 0x71, 0x0001);
}

static int
xonar_init(struct xonar_info *sc)
{
	uint16_t sVal;
	uint16_t sDac;
	uint8_t bVal;
	int count;

	/* Init CMI controller */
	sVal = cmi8788_read_2(sc, CTRL_VERSION);
	if (!(sVal & CTRL_VERSION2)) {
		bVal = cmi8788_read_1(sc, MISC_REG);
		bVal |= MISC_PCI_MEM_W_1_CLOCK;
		cmi8788_write_1(sc, MISC_REG, bVal);
	}
	bVal = cmi8788_read_1(sc, FUNCTION);
	bVal |= FUNCTION_RESET_CODEC;
	cmi8788_write_1(sc, FUNCTION, bVal);

	/* set up DAC related settings */
	sDac = I2S_MASTER | I2S_FMT_RATE48 | I2S_FMT_LJUST | I2S_FMT_BITS16;

	switch (sc->model) {
	case SUBID_XONAR_D1:
	case SUBID_XONAR_DX:
	case SUBID_XONAR_D2:
	case SUBID_XONAR_D2X:
	case SUBID_XONAR_STX:
	case SUBID_XONAR_DS:
		/* Must set master clock. */
		sDac |= XONAR_MCLOCK_256;
		break;
	case SUBID_XONAR_ST:
		sDac |= XONAR_MCLOCK_512;
	}
	cmi8788_write_2(sc, I2S_MULTICH_FORMAT, sDac);
	cmi8788_write_2(sc, I2S_ADC1_FORMAT, sDac);
	cmi8788_write_2(sc, I2S_ADC2_FORMAT, sDac);
	cmi8788_write_2(sc, I2S_ADC3_FORMAT, sDac);

	/* setup routing regs with default values */
	cmi8788_write_2(sc, PLAY_ROUTING, 0xE400);
	cmi8788_write_1(sc, REC_ROUTING, 0x00);
	cmi8788_write_1(sc, REC_MONITOR, 0x00);
	cmi8788_write_1(sc, MONITOR_ROUTING, 0xE4);

	/* Cold reset onboard AC97 */
	cmi8788_write_2(sc, AC97_CTRL, AC97_COLD_RESET);
	count = 100;
	while ((cmi8788_read_2(sc, AC97_CTRL) & AC97_STATUS_SUSPEND) && (count--))
	{
        cmi8788_setandclear_2(sc, AC97_CTRL, AC97_RESUME, AC97_STATUS_SUSPEND);
		DELAY(100);
	}

	if (!count)
		device_printf(sc->dev, "AC97 not ready\n");

	sVal = cmi8788_read_2(sc, AC97_CTRL);

	/* check if there's an onboard AC97 codec */
	if (sVal & AC97_CODEC0) {
        /* FIXME: Set in and out chan config regs to 0 as in OSS driver */
        cmi8788_write_2 (sc, AC97_OUT_CHAN_CONFIG, 0);
        cmi8788_write_2 (sc, AC97_IN_CHAN_CONFIG, 0);
        sc->ac97_codec_0 = AC97_CREATE (sc->dev, sc, xonar_ac97);
		device_printf(sc->dev, "AC97 codec0 found\n");
    }
	/* check if there's an front panel AC97 codec */
	if (sVal & AC97_CODEC1) {
        cmi8788_setandclear_2 (sc, AC97_OUT_CHAN_CONFIG, 0x0033, 0);
        cmi8788_setandclear_2 (sc, AC97_IN_CHAN_CONFIG, 0x0033, 0);
		device_printf(sc->dev, "AC97 codec1 found\n");
    }

	switch (sc->model) {
	case SUBID_XONAR_STX:
		sc->anti_pop_delay = 800;
		sc->output_control_gpio = GPIO_PIN0;

        cmi8788_setandclear_1 (sc, FUNCTION, FUNCTION_2WIRE, 0);
        cmi8788_setandclear_2 (sc, GPIO_CONTROL, 0x018F, 0);
		cmi8788_setandclear_2(sc, GPIO_DATA, GPIO_PIN0 | GPIO_PIN4 | GPIO_PIN8, 0);
        cmi8788_setandclear_2(sc, I2C_CTRL, TWOWIRE_SPEED_FAST, 0);

		pcm1796_write(sc, 20, PCM1796_SRST);
		pcm1796_write(sc,  20, 0);
		pcm1796_set_volume(sc, 75, 75);
		pcm1796_write(sc, 18, PCM1796_FMT_24L|PCM1796_ATLD);
		pcm1796_write(sc, 19, 0);

        ac97_init (sc);
		break;
	case SUBID_XONAR_ST:
		sc->anti_pop_delay = 100;
		sc->output_control_gpio = GPIO_PIN0;

        cmi8788_setandclear_1 (sc, FUNCTION, FUNCTION_2WIRE, 0);
        cmi8788_setandclear_2 (sc, GPIO_CONTROL, 0x01FF, 0);
		cmi8788_setandclear_2(sc, GPIO_DATA, GPIO_PIN0, GPIO_PIN8);
        cmi8788_setandclear_2(sc, I2C_CTRL, TWOWIRE_SPEED_FAST, 0);

		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x5, 0x9, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x2, 0x0, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x3, 0x0 | (0 << 3) | 0x0 | 0x1, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x4, (0 << 1) | 0x0, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x06, 0x00, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x07, 0x10, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x08, 0x00, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x09, 0x00, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x16, 0x10, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x17, 0, NULL);
		cmi8788_write_i2c(sc, XONAR_ST_CLOCK, 0x5, 0x1, NULL);

		/* Init DAC */
		pcm1796_write(sc, 20, 0);
		pcm1796_write(sc, 18, PCM1796_FMT_24L|PCM1796_ATLD);
		pcm1796_set_volume(sc, 75, 75);
		pcm1796_write(sc, 19, 0);

        /* Init AC97 codec 0 */
        ac97_init (sc);
	}

	/* check if MPU401 is enabled in MISC register */
	if (cmi8788_read_1 (sc, MISC_REG) & MISC_MIDI)
		device_printf(sc->dev, "MPU401 found\n");

	return (0);
}

static void
xonar_cleanup(struct xonar_info *sc)
{
    int i;

    for (i=0; i<2; i++) {
        if (sc->dmats[i]) {
            bus_dma_tag_destroy(sc->dmats[i]);
            sc->dmats[i] = NULL;
        }
    }
	if (sc->ih) {
		bus_teardown_intr(sc->dev, sc->irq, sc->ih);
		sc->ih = NULL;
	}
	if (sc->reg) {
		bus_release_resource(sc->dev, sc->regtype, sc->regid, sc->reg);
		sc->reg = NULL;
	}
	if (sc->irq) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irqid, sc->irq);
		sc->irq = NULL;
	}
	if (sc->lock) {
		snd_mtxfree(sc->lock);
		sc->lock = NULL;
	}

    if (sc->ac97_codec_0) {
        ac97_destory (sc->ac97_codec_0);
        sc->ac97_codec_0 = NULL;
    }

    if (sc->ac97_codec_1) {
        ac97_destory (sc->ac97_codec_1);
        sc->ac97_codec_1 = NULL;
    }

	kern_free(sc, M_DEVBUF);
}

/* this one is a BIG HUGE XXX 
 *
 * it replaces pcm_channel's buffers with
 * newly allocated different sized
 * this is insane, fix it
 */
static int
chan_reset_buf(struct xonar_chinfo *ch)
{
	struct snd_dbuf *b, *bs;
	struct pcm_channel *c;
	struct xonar_info *sc;

	c = ch->channel;
	sc = ch->parent;

	b = sndbuf_create(c->dev, c->name, "primary", c);
	bs = sndbuf_create(c->dev, c->name, "secondary", c);
	device_printf(sc->dev, "Replacing buffers %p->%p, %p->%p..",
			c->bufhard, b, c->bufsoft, bs);
	sndbuf_destroy(c->bufsoft);
	sndbuf_destroy(c->bufhard);
	if (pcm_sndbuf_alloc(b, sc->dmats[0], 0, sc->bufsz) != 0) {
		kern_printf("failed\n");
		device_printf(sc->dev, "Cannot allocate sndbuf\n");
		goto out;
	}
	sndbuf_setup(bs, NULL, 0);
	c->bufhard = b;
	c->bufsoft = bs;
	ch->buffer = b;

	sndbuf_setfmt(b, c->format);
	sndbuf_setspd(b, c->speed);
	sndbuf_setfmt(bs, c->format);
	sndbuf_setspd(bs, c->speed);

#if defined __FreeBSD__
	if (c->direction == PCMDIR_PLAY) {
		bs->sl = sndbuf_getmaxsize(bs);
		bs->shadbuf = kern_malloc(bs->sl, M_DEVBUF, M_NOWAIT);
		if (bs->shadbuf == NULL)
			goto out;
	}
#endif
	kern_printf("succeed\n");
	return 0;
out:
	sndbuf_destroy(b);
	sndbuf_destroy(bs);
	ch->state = CHAN_STATE_INVALID;
	return 1;
}

static int
sysctl_xonar_buffersize(SYSCTL_HANDLER_ARGS) 
{
	struct xonar_info *sc;
	struct xonar_chinfo *ch;
	device_t dev;
	int val, err;
	int obufsz;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = sc->bufsz;
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if ((val < 2048) || (val > sc->bufmaxsz))
		return (EINVAL);
	ch = &sc->chan[0];
	if (ch->state == CHAN_STATE_ACTIVE)
		return EBUSY;
	if (val != sc->bufsz) {
		obufsz = sc->bufsz;
		sc->bufsz = 2048*(val/2048);
		ch->state = CHAN_STATE_INIT;
		if (chan_reset_buf(ch)) {
			sc->bufsz = obufsz;
			return EINVAL;
		}
	}
	return err;
}

static int
sysctl_xonar_rec_monitor(SYSCTL_HANDLER_ARGS)
{
	struct xonar_info *sc;
	device_t dev;
	int val, err;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = cmi8788_get_rec_monitor (sc);
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if (val < 0 || val > 1)
		return (EINVAL);
	cmi8788_set_rec_monitor(sc, val);
	return err;
}

static int
sysctl_xonar_output(SYSCTL_HANDLER_ARGS) 
{
	struct xonar_info *sc;
	device_t dev;
	int val, err;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = cmi8788_get_output (sc);
    if (val < 0)
        return EINVAL;
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if (val < 0 || val > 2)
		return (EINVAL);
	cmi8788_set_output(sc, val);
	return err;
}

static int
sysctl_xonar_rolloff(SYSCTL_HANDLER_ARGS) 
{
	struct xonar_info *sc;
	device_t dev;
	int val, err;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = pcm1796_get_rolloff (sc);
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if (val < 0 || val > 1)
		return (EINVAL);
	pcm1796_set_rolloff(sc, val);
	return err;
}

static int
sysctl_xonar_bypass(SYSCTL_HANDLER_ARGS) 
{
	struct xonar_info *sc;
	device_t dev;
	int val, err;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = pcm1796_get_bypass (sc);
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if (val < 0 || val > 1)
		return (EINVAL);
	pcm1796_set_bypass(sc, val);
	return err;
}

static int
sysctl_xonar_deemph(SYSCTL_HANDLER_ARGS) 
{
	struct xonar_info *sc;
	device_t dev;
	int val, err;

	dev = oidp->oid_arg1;
	sc = pcm_getdevinfo(dev);
	if (sc == NULL)
		return EINVAL;
	val = pcm1796_get_deemph (sc);
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err || req->newptr == NULL)
		return (err);
	if (val < 0 || val > 1)
		return (EINVAL);
	pcm1796_set_deemph(sc, val);
	return err;
}

static void
xonar_intr(void *p) {
	struct xonar_info *sc = p;
	struct xonar_chinfo *ch;
	unsigned int intstat;
    int i;

    if ((intstat = cmi8788_read_2(sc, IRQ_STAT)) == 0)
		return;

    for (i=0; i < MAX_PORTS_PLAY+MAX_PORTS_REC; i++) {
        ch = &(sc->chan[i]);
        if ((ch->state == CHAN_STATE_ACTIVE) && (intstat & ch->irq_mask)) {
            /* Acknowledge the interrupt by disabling and enabling the irq */
            cmi8788_setandclear_2 (sc, IRQ_MASK, 0, ch->irq_mask);
            cmi8788_setandclear_2 (sc, IRQ_MASK, ch->irq_mask, 0);
            chn_intr(ch->channel);
        }
    }
}

/* device interface */
static int
xonar_probe(device_t dev)
{
	int i;
	uint16_t subvid, subid;

	subvid = pci_get_subvendor(dev);
	subid = pci_get_subdevice(dev);

	if ((pci_get_vendor(dev) != CMEDIA_VENDOR_ID) || 
		 (pci_get_device(dev) != CMEDIA_CMI8788))
		return (ENXIO);

	for (i = 0; i < sizeof(xonar_hw) / sizeof(xonar_hw[0]); i++) {
		if ((subvid == xonar_hw[i].vendor) &&
		    (subid  == xonar_hw[i].devid))
			device_set_desc(dev, xonar_hw[i].desc);
	}
	return (BUS_PROBE_DEFAULT);
}

static int
xonar_attach(device_t dev) 
{
	struct xonar_info *sc;
	char status[SND_STATUSLEN];
    int i;

	sc = kern_malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->lock = snd_mtxcreate(device_get_nameunit(dev), "snd_cmi8788 softc");
	sc->dev = dev;

	pci_enable_busmaster(dev);
	pci_enable_io(dev, SYS_RES_IOPORT);

	sc->model = pci_get_subdevice(dev);
	
	sc->regid = PCIR_BAR(0);
	sc->regtype = SYS_RES_IOPORT;
	sc->reg = bus_alloc_resource_any(dev, sc->regtype,
	    &sc->regid, RF_ACTIVE);
	if (!sc->reg) {
		device_printf(dev, "unable to allocate register space\n");
		goto bad;
	}
	sc->st = rman_get_bustag(sc->reg);
	sc->sh = rman_get_bushandle(sc->reg);

    xonar_init(sc);

    sc->irqid = 0;
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->irqid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (!sc->irq || snd_setup_intr(dev, sc->irq, INTR_MPSAFE,
	    xonar_intr, sc, &sc->ih)) {
		device_printf(dev, "unable to map interrupt\n");
		goto bad;
	}

	sc->bufmaxsz = sc->bufsz = pcm_getbuffersize(dev, 2048, DEFAULT_BUFFER_BYTES_MULTICH, 65536);
    for (i=0; i<2; i++) {
        if (xonar_create_dma_tag(&(sc->dmats[i]), sc->bufsz, bus_get_dma_tag(dev), sc->lock) != 0) {
            device_printf(sc->dev, "unable to create dma tag\n");
            return (ENOMEM);
        }
    }

	if (mixer_init(dev, &xonar_mixer_class, sc))
		goto bad;

	if (pcm_register(dev, sc, MAX_PORTS_PLAY, MAX_PORTS_REC))
		goto bad;

	for(int i = 0; i < MAX_PORTS_PLAY; i++) {
		pcm_addchan(dev, PCMDIR_PLAY, &xonar_chan_class, sc);
		sc->pnum++;
	}
    sc->pnum = 0;
    for(int i = 0; i < MAX_PORTS_REC; i++) {
		pcm_addchan(dev, PCMDIR_REC, &xonar_chan_class, sc);
		sc->pnum++;
	}

	kern_snprintf(status, SND_STATUSLEN, "at io 0x%lx irq %ld %s",
		 rman_get_start(sc->reg), rman_get_start(sc->irq),
		 PCM_KLDSTRING(snd_cmi8788));
	pcm_setstatus(dev, status);

	SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"buffersize", CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_ANYBODY,
			sc->dev, sizeof(sc->dev), sysctl_xonar_buffersize, "I",
			"Set buffer size");
	SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"output", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, sc->dev,
			sizeof(sc->dev), sysctl_xonar_output, "I",
			"Set output: 0 - line, 1 - rear hp, 2 - hp");
	SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"rolloff", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, sc->dev,
			sizeof(sc->dev), sysctl_xonar_rolloff, "I",
			"Set rolloff: 0 - sharp, 1 - slow");
	SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"de-emph", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, sc->dev,
			sizeof(sc->dev), sysctl_xonar_deemph, "I",
			"Set de-emphasis: 0 - disabled, 1 - enabled");
	SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"dfbypass", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, sc->dev,
			sizeof(sc->dev), sysctl_xonar_bypass, "I",
			"Set digital filter bypass: 0 - disabled, 1 - enabled");
    SYSCTL_ADD_PROC(kern_sysctl_ctx(sc->dev),
			SYSCTL_CHILDREN(kern_sysctl_tree(sc->dev)), OID_AUTO,
			"monitor", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, sc->dev,
			sizeof(sc->dev), sysctl_xonar_rec_monitor, "I",
			"Enable recording monitor");

	return (0);
bad:
	xonar_cleanup(sc);
	return (ENXIO);
}

static int
xonar_detach(device_t dev) 
{
	struct xonar_info *sc;
	int r;

	sc = pcm_getdevinfo(dev);
	r = pcm_unregister(dev);
	if (r)
		return r;

	xonar_cleanup(sc);
	return (0);
}

static device_method_t cmi8788_methods[] = {
	/* Methods from the device interface */
	DEVMETHOD(device_probe,         xonar_probe),
	DEVMETHOD(device_attach,        xonar_attach),
	DEVMETHOD(device_detach,        xonar_detach),

	/* Terminate method list */
	{ 0, 0 }
};

static driver_t cmi8788_driver = {
	"pcm",
	cmi8788_methods,
	PCM_SOFTC_SIZE,
};

DRIVER_MODULE(snd_cmi8788, pci, cmi8788_driver, pcm_devclass, NULL, NULL);
MODULE_DEPEND(snd_cmi8788, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
MODULE_VERSION(snd_cmi8788, 0);
