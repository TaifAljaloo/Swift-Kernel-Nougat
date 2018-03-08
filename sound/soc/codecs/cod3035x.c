/*
 * Copyright (c) 2014 Samsung Electronics Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <sound/samsung/abox.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/completion.h>

#include <linux/mfd/samsung/s2mpu08-private.h>
#include <sound/cod3035x.h>
#include "cod3035x.h"

#define COD3035X_SAMPLE_RATE_48KHZ	48000
#define COD3035X_SAMPLE_RATE_192KHZ	192000

#define COD3035X_MAX_IRQ_CHK_BITS	7
#define COD3035X_START_IRQ_CHK_BIT	2
#define COD3035X_MJ_DET_INVALID		(-1)

#ifdef CONFIG_SND_SOC_SAMSUNG_VERBOSE_DEBUG
#ifdef dev_dbg
#undef dev_dbg
#endif
#ifdef dev_info
#undef dev_info
#endif
#if 1 /* if: print option */
#define dev_dbg dev_err
#define dev_info dev_err
#else /* else: print option */
static void no_dev_dbg(void *v, char *s, ...)
{
}
#define dev_dbg no_dev_dbg
#define dev_info no_dev_dbg
#endif /* endif: print option */
#endif

#define COD3035X_WATER_DET_THRESHOLD_MAX 3280
#define COD3035X_WATER_DET_THRESHOLD_MIN 120
#define COD3035X_GDET_FINISH_CHK_MORE_NO 3
#define COD3035X_GDET_FINISH_CHK_MIN 30
#define COD3035X_MIC_DET_DELAY 300

/* defined for impedance calculate */
#define COD3035X_IMP_C1 99
#define COD3035X_IMP_C2 33
#define COD3035X_IMP_C3 418
#define COD3035X_IMP_LOW 11
#define COD3035X_IMP_HIGH 200
#define COD3035X_PARAM_SHIFT 5
#define COD3035X_PARAM_1_WIDTH 3
#define COD3035X_PARAM_2_WIDTH 2
#define COD3035X_PARAM_1_MASK MASK(COD3035X_PARAM_1_WIDTH, COD3035X_PARAM_SHIFT)
#define COD3035X_PARAM_2_MASK MASK(COD3035X_PARAM_2_WIDTH, COD3035X_PARAM_SHIFT)
#define COD3035X_P3_BIT_CHECK BIT(9)

/* Forward Declarations */
static void cod3035x_reset_io_selector_bits(struct snd_soc_codec *codec);
static void cod3035x_configure_mic_bias(struct snd_soc_codec *codec);
static int cod3035x_disable(struct device *dev);
static int cod3035x_enable(struct device *dev);

static inline void cod3035x_usleep(unsigned int u_sec)
{
	usleep_range(u_sec, u_sec + 10);
}

/**
 * Helper functions to read ADC value for button detection
 */

#define COD3035X_ADC_SAMPLE_SIZE	5

static void cod3035x_adc_start(struct cod3035x_priv *cod3035x)
{
	cod3035x->jack_adc = iio_channel_get_all(cod3035x->dev);
}

static void cod3035x_adc_stop(struct cod3035x_priv *cod3035x)
{
	iio_channel_release(cod3035x->jack_adc);
}

static int cod3035x_gdet_adc_get_value(struct cod3035x_priv *cod3035x)
{
    int adc_data = -1;
    int adc_max = 0;
    int adc_min = 0xFFFF;
    int adc_total = 0;
    int adc_retry_cnt = 0;
    int i;
    struct iio_channel *jack_adc = cod3035x->jack_adc;

	for (i = 0; i < COD3035X_ADC_SAMPLE_SIZE; i++) {
		iio_read_channel_raw(&jack_adc[1], &adc_data);

		/* if adc_data is negative, ignore */
		while (adc_data < 0) {
			adc_retry_cnt++;
			if (adc_retry_cnt > 10)
				return adc_data;
			iio_read_channel_raw(&jack_adc[1], &adc_data);
		}

		/* Update min/max values */
		if (adc_data > adc_max)
			adc_max = adc_data;
		if (adc_data < adc_min)
			adc_min = adc_data;
		adc_total += adc_data;
	}
	return (adc_total - adc_max - adc_min) / (COD3035X_ADC_SAMPLE_SIZE - 2);
}

static int cod3035x_adc_get_value(struct cod3035x_priv *cod3035x)
{
	int adc_data = -1;
	int adc_max = 0;
	int adc_min = 0xFFFF;
	int adc_total = 0;
	int adc_retry_cnt = 0;
	int i;
	struct iio_channel *jack_adc = cod3035x->jack_adc;

	for (i = 0; i < COD3035X_ADC_SAMPLE_SIZE; i++) {
		iio_read_channel_raw(&jack_adc[0], &adc_data);
		/* if adc_data is negative, ignore */
		while (adc_data < 0) {
			adc_retry_cnt++;
			if (adc_retry_cnt > 10)
				return adc_data;
			iio_read_channel_raw(&jack_adc[0], &adc_data);
		}

		/* Update min/max values */
		if (adc_data > adc_max)
			adc_max = adc_data;
		if (adc_data < adc_min)
			adc_min = adc_data;

		adc_total += adc_data;
	}
	return (adc_total - adc_max - adc_min) / (COD3035X_ADC_SAMPLE_SIZE - 2);
}

/**
 * Return value:
 * true: if the register value cannot be cached, hence we have to read from the
 * hardware directly
 * false: if the register value can be read from cache
 */
static bool cod3035x_volatile_register(struct device *dev, unsigned int reg)
{
	/**
	 * For all the registers for which we want to restore the value during
	 * regcache_sync operation, we need to return true here. For registers
	 * whose value need not be cached and restored should return false here.
	 *
	 * For the time being, let us cache the value of all registers other
	 * than the IRQ pending and IRQ status registers.
	 */
	switch (reg) {
	case COD3035X_01_IRQ1PEND ... COD3035X_04_IRQ4PEND:
	case COD3035X_09_STATUS1 ... COD3035X_0D_STATUS5:
	case COD3035X_60_IRQ_SENSOR ... COD3035X_63_OFFSET_DA:
	case COD3035X_80_PDB_ACC1 ... COD3035X_8F_CTR_DLY2:
	case COD3035X_90_CTR_DLY3 ... COD3035X_9F_IMP_CNT4:
	case COD3035X_A0_IMP_CNT5 ... COD3035X_AF_TEST_ADC5:
		return true;
	default:
		return false;
	}
}

/**
 * Return value:
 * true: if the register value can be read
 * flase: if the register cannot be read
 */
static bool cod3035x_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case COD3035X_01_IRQ1PEND ... COD3035X_0D_STATUS5:
	case COD3035X_10_PD_REF ... COD3035X_1D_SV_DA:
	case COD3035X_20_VOL_AD1 ... COD3035X_28_DSM_ADS:
	case COD3035X_30_VOL_HPL ... COD3035X_3E_OVP_2:
	case COD3035X_40_DIGITAL_POWER ... COD3035X_4F_DMIC4:
	case COD3035X_50_DAC1 ... COD3035X_5F_CRO3:
	case COD3035X_60_IRQ_SENSOR ... COD3035X_6C_MIC_ON:
	case COD3035X_70_CLK1_COD ... COD3035X_7C_LPF_AD:
	case COD3035X_80_PDB_ACC1 ... COD3035X_8F_CTR_DLY2:
	case COD3035X_90_CTR_DLY3 ... COD3035X_9F_IMP_CNT4:
	case COD3035X_A0_IMP_CNT5 ... COD3035X_AF_TEST_ADC5:
	case COD3035X_B0_AUTO_HP1 ... COD3035X_BE_ODSEL2:
	case COD3035X_D0_CTRL_IREF1 ... COD3035X_DC_CTRL_EPS:
	case COD3035X_E1_PRESET_AVC ... COD3035X_EF_I_GAIN_FILTER1:
	case COD3035X_F2_PRESET_AVC ... COD3035X_F6_PRESET_AVC:
		return true;
	default:
		return false;
	}
}

static bool cod3035x_writeable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* Reg-0x09 to Reg-0x0D are read-only status registers */
	case COD3035X_01_IRQ1PEND ... COD3035X_08_IRQ4M:
	case COD3035X_10_PD_REF ... COD3035X_1D_SV_DA:
	case COD3035X_20_VOL_AD1 ... COD3035X_28_DSM_ADS:
	case COD3035X_30_VOL_HPL ... COD3035X_3E_OVP_2:
	case COD3035X_40_DIGITAL_POWER ... COD3035X_4F_DMIC4:
	case COD3035X_50_DAC1 ... COD3035X_5F_CRO3:
	/* Reg-0x60 is read-only */
	case COD3035X_61_OFFSET_AD1 ... COD3035X_6C_MIC_ON:
	case COD3035X_70_CLK1_COD ... COD3035X_7C_LPF_AD:
	case COD3035X_80_PDB_ACC1 ... COD3035X_8F_CTR_DLY2:
	case COD3035X_90_CTR_DLY3 ... COD3035X_9F_IMP_CNT4:
	case COD3035X_A0_IMP_CNT5 ... COD3035X_AF_TEST_ADC5:
	case COD3035X_B0_AUTO_HP1 ... COD3035X_BE_ODSEL2:
	case COD3035X_D0_CTRL_IREF1 ... COD3035X_DC_CTRL_EPS:
	case COD3035X_E1_PRESET_AVC ... COD3035X_EF_I_GAIN_FILTER1:
	case COD3035X_F2_PRESET_AVC ... COD3035X_F6_PRESET_AVC:
		return true;
	default:
		return false;
	}
}

const struct regmap_config cod3035x_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	/* "speedy" string should be described in the name field
	 *  this will be used for the speedy inteface,
	 *  when read/write operations are used in the regmap driver.
	 * APM functions will be called instead of the I2C
	 * refer to the "drivers/base/regmap/regmap-i2c.c
	 */
	.name = "speedy, COD3035X",
	.max_register = COD3035X_MAX_REGISTER,
	.readable_reg = cod3035x_readable_register,
	.writeable_reg = cod3035x_writeable_register,
	.volatile_reg = cod3035x_volatile_register,

	.use_single_rw = true,
	.cache_type = REGCACHE_RBTREE,
};

/**
 * TLV_DB_SCALE_ITEM
 *
 * (TLV: Threshold Limit Value)
 *
 * For various properties, the dB values don't change linearly with respect to
 * the digital value of related bit-field. At most, they are quasi-linear,
 * that means they are linear for various ranges of digital values. Following
 * table define such ranges of various properties.
 *
 * TLV_DB_RANGE_HEAD(num)
 * num defines the number of linear ranges of dB values.
 *
 * s0, e0, TLV_DB_SCALE_ITEM(min, step, mute),
 * s0: digital start value of this range (inclusive)
 * e0: digital end valeu of this range (inclusive)
 * min: dB value corresponding to s0
 * step: the delta of dB value in this range
 * mute: ?
 *
 * Example:
 *	TLV_DB_RANGE_HEAD(3),
 *	0, 1, TLV_DB_SCALE_ITEM(-2000, 2000, 0),
 *	2, 4, TLV_DB_SCALE_ITEM(1000, 1000, 0),
 *	5, 6, TLV_DB_SCALE_ITEM(3800, 8000, 0),
 *
 * The above code has 3 linear ranges with following digital-dB mapping.
 * (0...6) -> (-2000dB, 0dB, 1000dB, 2000dB, 3000dB, 3800dB, 4600dB),
 *
 * DECLARE_TLV_DB_SCALE
 *
 * This macro is used in case where there is a linear mapping between
 * the digital value and dB value.
 *
 * DECLARE_TLV_DB_SCALE(name, min, step, mute)
 *
 * name: name of this dB scale
 * min: minimum dB value corresponding to digital 0
 * step: the delta of dB value
 * mute: ?
 *
 * NOTE: The information is mostly for user-space consumption, to be viewed
 * alongwith amixer.
 */

/**
 * cod3035x_ctvol_bst_tlv
 *
 * (0...2) -> (0dB, 1200dB, 2000dB)
 */
static const unsigned int cod3035x_ctvol_bst_tlv[] = {
	TLV_DB_RANGE_HEAD(2),
	0, 1, TLV_DB_SCALE_ITEM(0, 1200, 0),
	2, 2, TLV_DB_SCALE_ITEM(2000, 0, 0),
};

/**
 * cod3035x_avc_ctvol_hp_tlv
 *
 * Range: 0dB to +4dB, step 1dB
 *
 * CTVOL_HP, reg(0x57), shift(0), width(3), invert(1), max(5)
 */
static const DECLARE_TLV_DB_SCALE(cod3035x_avc_ctvol_hp_tlv, 0, 100, 0);

/**
 * cod3019_ctvol_ep_tlv
 *
 * Range: 0dB to +12dB, step 1dB
 *
 * CTVOL_EP, reg(0x32), shift(4), width(4), invert(0), max(12)
 */
static const DECLARE_TLV_DB_SCALE(cod3035x_ctvol_ep_tlv, 0, 100, 0);

/**
 * cod3035x_dvol_adc_tlv
 *
 * Map as per data-sheet:
 * (0x00 to 0x86) -> (+12dB to -55dB, step 0.5dB)
 * (0x87 to 0x91) -> (-56dB to -66dB, step 1dB)
 * (0x92 to 0x94) -> (-68dB to -72dB, step 2dB)
 * (0x95 to 0x96) -> (-78dB to -84dB, step 6dB)
 *
 * When the map is in descending order, we need to set the invert bit
 * and arrange the map in ascending order. The offsets are calculated as
 * (max - offset).
 *
 * offset_in_table = max - offset_actual;
 *
 * DVOL_ADL, reg(0x47), shift(0), width(8), invert(1), max(0x96)
 * DVOL_ADR, reg(0x48), shift(0), width(8), invert(1), max(0x96)
 * DVOL_ADC, reg(0x4C), shift(0), width(8), invert(1), max(0x96)
 */
static const unsigned int cod3035x_dvol_adc_tlv[] = {
	TLV_DB_RANGE_HEAD(4),
	0x00, 0x01, TLV_DB_SCALE_ITEM(-8400, 600, 0),
	0x02, 0x04, TLV_DB_SCALE_ITEM(-7200, 200, 0),
	0x05, 0x09, TLV_DB_SCALE_ITEM(-6600, 100, 0),
	0x10, 0x96, TLV_DB_SCALE_ITEM(-5500, 50, 0),
};

/**
 * cod3035x_dvol_dac_tlv
 *
 * Map as per data-sheet:
 * 0x00 ~ 0xE0 : +42dB to -70dB, step 0.5dB
 * 0xE1 ~ 0xE5 : -72dB to -80dB, step 2dB
 * 0xE6 : -82.4dB
 * 0xE7 ~ 0xE9 : -84.3dB to -96.3dB, step 6dB
 *
 * When the map is in descending order, we need to set the invert bit
 * and arrange the map in ascending order. The offsets are calculated as
 * (max - offset).
 *
 * offset_in_table = max - offset_actual;
 *
 * DAC_DAL, reg(0x51), shift(0), width(8), invert(1), max(0xE9)
 * DAC_DAR, reg(0x52), shift(0), width(8), invert(1), max(0xE9)
 */
static const unsigned int cod3035x_dvol_dac_tlv[] = {
	TLV_DB_RANGE_HEAD(4),
	0x00, 0x03, TLV_DB_SCALE_ITEM(-9630, 600, 0),
	0x04, 0x04, TLV_DB_SCALE_ITEM(-8240, 0, 0),
	0x05, 0x09, TLV_DB_SCALE_ITEM(-8000, 200, 0),
	0x0A, 0xE9, TLV_DB_SCALE_ITEM(-7000, 50, 0),
};



static const unsigned int cod3035x_hp_avol_bypass_mode_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0x00, 0x3F, TLV_DB_SCALE_ITEM(-5900, 100, 0),
};

/**
 * cod3035x_dnc_min_gain_tlv
 *
 * Range: -6dB to 0dB, step 1dB
 *
 * DNC_MINGAIN , reg(0x55), shift(5), width(3)
 */
static const unsigned int cod3035x_dnc_min_gain_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0x00, 0x06, TLV_DB_SCALE_ITEM(-600, 0, 0),
};

/**
 * cod3035x_dnc_max_gain_tlv
 *
 * Range: 0dB to 24dB, step 1dB
 *
 * DNC_MAXGAIN , reg(0x55), shift(0), width(5)
 */
static const unsigned int cod3035x_dnc_max_gain_tlv[] = {
	TLV_DB_RANGE_HEAD(1),
	0x06, 0x1e, TLV_DB_SCALE_ITEM(0, 2400, 0),
};

/**
 * cod3035x_dnc_lvl_tlv
 *
 * Range: -10.5dB to 0dB, step 1.5dB
 *
 * DNCLVL_R/L, reg(0x55), shift(0/4), width(3), invert(0), max(7)
 */
static const DECLARE_TLV_DB_SCALE(cod3035x_dnc_lvl_tlv, -1050, 0, 0);

/**
 * DMIC GAIN
 *
 * Selection digital mic gain through conversion code level.
 */
static const char *cod3035x_dmic_code_level_text[] = {
	"0", "1", "2", "3",
	"4", "5", "6", "7"
};

static const struct soc_enum cod3035x_dmic2_gain_code_level_enum =
	SOC_ENUM_SINGLE(COD3035X_4A_DMIC2, DMIC_GAIN1_SHIFT,
			ARRAY_SIZE(cod3035x_dmic_code_level_text),
			cod3035x_dmic_code_level_text);

static const struct soc_enum cod3035x_dmic4_gain_code_level_enum =
	SOC_ENUM_SINGLE(COD3035X_4F_DMIC4, DMIC4_GAIN_SHIFT,
			ARRAY_SIZE(cod3035x_dmic_code_level_text),
			cod3035x_dmic_code_level_text);

/**
 * HP amp reference current
 *
 * Selection HP amp reference current through conversion code level.
 */
static const char *cod3035x_hp_code_level_text[] = {
	"0", "1", "2", "3",
	"4", "5", "6", "7"
};

static const struct soc_enum cod3035x_in_mu_ctmi_hpa_code_level_enum =
	SOC_ENUM_SINGLE(COD3035X_BB_AUTO_HP12, IN_MU_CTMI_HPA_SHIFT,
			ARRAY_SIZE(cod3035x_hp_code_level_text),
			cod3035x_hp_code_level_text);

static const struct soc_enum cod3035x_out_mu_ctmi_hpa_code_level_enum =
	SOC_ENUM_SINGLE(COD3035X_BB_AUTO_HP12, OUT_MU_CTMI_HPA_SHIFT,
			ARRAY_SIZE(cod3035x_hp_code_level_text),
			cod3035x_hp_code_level_text);

/**
 * mono_mix_mode
 *
 * Selecting the Mode of Mono Mixer (inside DAC block)
 */
static const char *cod3035x_mono_mix_mode_text[] = {
	"Disable", "R", "L", "LR-Invert",
	"(L+R)/2", "L+R"
};

static const struct soc_enum cod3035x_mono_mix_mode_enum =
SOC_ENUM_SINGLE(COD3035X_50_DAC1, DAC1_MONOMIX_SHIFT,
		ARRAY_SIZE(cod3035x_mono_mix_mode_text),
		cod3035x_mono_mix_mode_text);


static int dac_soft_mute_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	       return 0;
}


static int dac_soft_mute_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	int value = ucontrol->value.integer.value[0];

	if (!value)
		/* enable soft mute */
		snd_soc_update_bits(codec, COD3035X_50_DAC1,
				DAC1_SOFT_MUTE_MASK, DAC1_SOFT_MUTE_MASK);
	else
		/* diable soft mute */
		snd_soc_update_bits(codec, COD3035X_50_DAC1,
				DAC1_SOFT_MUTE_MASK, 0x0);

	dev_info(codec->dev, "%s: soft mute : %s\n", __func__,
			(!value) ? "on":"off");
	return 0;
}

static int avc_bypass_mode_get(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
           return 0;
}

static int avc_bypass_mode_put(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    int value = ucontrol->value.integer.value[0];

    if (value)
        /* enable avc bypass mode */
        snd_soc_update_bits(codec, COD3035X_54_AVC1,
                AVC_BYPS_MASK, AVC_BYPS_MASK);
    else
        /* diable avc bypass mode */
        snd_soc_update_bits(codec, COD3035X_54_AVC1,
                AVC_BYPS_MASK, 0x0);

    dev_info(codec->dev, "%s: avc bypass mode : %s\n", __func__,
            (value) ? "on":"off");
    return 0;
}

static int avc_mute_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int avc_mute_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	int value = ucontrol->value.integer.value[0];

	if (value)
		snd_soc_update_bits(codec, COD3035X_54_AVC1,
				AVC_MU_EN_MASK, AVC_MU_EN_MASK);
	else
		snd_soc_update_bits(codec, COD3035X_54_AVC1,
				AVC_MU_EN_MASK, 0x0);

	dev_info(codec->dev, "%s: avc mute enable : %s\n", __func__,
            (value) ? "on":"off");
	return 0;
}


static int hp_avol_mode_get(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
           return 0;
}

static int hp_avol_mode_put(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    int value = ucontrol->value.integer.value[0];

	dev_info(codec->dev, "%s: hp analog volume : %d\n", __func__,
		value);

	if (value >= 0) {
		snd_soc_update_bits(codec, COD3035X_E3_PRESET_AVC,
			HP_PN_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_E5_PRESET_AVC,
			HP_PN_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_E3_PRESET_AVC,
			HP_AVOL_MASK, value);
		snd_soc_update_bits(codec, COD3035X_E5_PRESET_AVC,
	        HP_AVOL_MASK, value);
	} else {
		snd_soc_update_bits(codec, COD3035X_E3_PRESET_AVC,
			HP_PN_MASK, HP_PN_MASK);
		snd_soc_update_bits(codec, COD3035X_E5_PRESET_AVC,
			HP_PN_MASK, HP_PN_MASK);
		snd_soc_update_bits(codec, COD3035X_E3_PRESET_AVC,
			HP_AVOL_MASK, -value);
		snd_soc_update_bits(codec, COD3035X_E5_PRESET_AVC,
	        HP_AVOL_MASK, -value);
	}

    return 0;
}

static int mcb2_chop_mode_get(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
           return 0;
}

static int mcb2_chop_mode_put(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
    int value = ucontrol->value.integer.value[0];

    if (value)
        /* enable mcb2 chop mode */
        snd_soc_update_bits(codec, COD3035X_76_CHOP_AD,
                EN_MCB2_CHOP_MASK, EN_MCB2_CHOP_MASK);
    else
        /* diable mcb2 chop mode */
        snd_soc_update_bits(codec, COD3035X_76_CHOP_AD,
                EN_MCB2_CHOP_MASK, 0x0);

    dev_info(codec->dev, "%s: mcb2 chop mode : %s\n", __func__,
            (value) ? "on":"off");
    return 0;
}

/**
 * struct snd_kcontrol_new cod3035x_snd_control
 *
 * Every distinct bit-fields within the CODEC SFR range may be considered
 * as a control elements. Such control elements are defined here.
 *
 * Depending on the access mode of these registers, different macros are
 * used to define these control elements.
 *
 * SOC_ENUM: 1-to-1 mapping between bit-field value and provided text
 * SOC_SINGLE: Single register, value is a number
 * SOC_SINGLE_TLV: Single register, value corresponds to a TLV scale
 * SOC_SINGLE_TLV_EXT: Above + custom get/set operation for this value
 * SOC_SINGLE_RANGE_TLV: Register value is an offset from minimum value
 * SOC_DOUBLE: Two bit-fields are updated in a single register
 * SOC_DOUBLE_R: Two bit-fields in 2 different registers are updated
 */

/**
 * All the data goes into cod3035x_snd_controls.
 * All path inter-connections goes into cod3035x_dapm_routes
 */
static const struct snd_kcontrol_new cod3035x_snd_controls[] = {
	SOC_SINGLE_TLV("MIC1 Boost Volume", COD3035X_20_VOL_AD1,
			VOLAD1_CTVOL_BST1_SHIFT,
			(BIT(VOLAD1_CTVOL_BST1_WIDTH) - 1), 0,
			cod3035x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("MIC2 Boost Volume", COD3035X_21_VOL_AD2,
			VOLAD2_CTVOL_BST2_SHIFT,
			(BIT(VOLAD2_CTVOL_BST2_WIDTH) - 1), 0,
			cod3035x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("MIC3 Boost Volume", COD3035X_22_VOL_AD3,
			VOLAD3_CTVOL_BST3_SHIFT,
			(BIT(VOLAD3_CTVOL_BST3_WIDTH) - 1), 0,
			cod3035x_ctvol_bst_tlv),

	SOC_SINGLE_TLV("Headphone Volume", COD3035X_57_AVC4,
			AVC_CTVOL_HP_SHIFT,
			(BIT(AVC_CTVOL_HP_WIDTH) - 1), 0,
			cod3035x_avc_ctvol_hp_tlv),

	SOC_SINGLE_TLV("Earphone Volume", COD3035X_32_VOL_EP,
			CTVOL_EP_SHIFT,
			(BIT(CTVOL_EP_WIDTH) - 1), 0,
			cod3035x_ctvol_ep_tlv),

	SOC_SINGLE_TLV("ADC Left Gain", COD3035X_47_AVOLL1,
			AD_DA_DVOL_SHIFT,
			AD_DVOL_MAXNUM, 1, cod3035x_dvol_adc_tlv),

	SOC_SINGLE_TLV("ADC Right Gain", COD3035X_48_AVOLR1,
			AD_DA_DVOL_SHIFT,
			AD_DVOL_MAXNUM, 1, cod3035x_dvol_adc_tlv),

	SOC_SINGLE_TLV("ADC Center Gain", COD3035X_4C_AVOLL2,
			AD_DA_DVOL_SHIFT,
			AD_DVOL_MAXNUM, 1, cod3035x_dvol_adc_tlv),

	SOC_DOUBLE_R_TLV("DAC Gain", COD3035X_51_DVOLL, COD3035X_52_DVOLR,
			AD_DA_DVOL_SHIFT,
			DA_DVOL_MAXNUM, 1, cod3035x_dvol_dac_tlv),

	SOC_DOUBLE_R_TLV("HP Analog Volume AVCBypass", COD3035X_30_VOL_HPL, COD3035X_31_VOL_HPR,
			CTVOL_HP_AVCBYPASS_SHIFT,
			CTVOL_HP_AVCBYPASS_MAX_NUM, 1, cod3035x_hp_avol_bypass_mode_tlv),

	SOC_ENUM("MonoMix Mode", cod3035x_mono_mix_mode_enum),

	SOC_SINGLE_EXT("DAC Soft Mute", SND_SOC_NOPM, 0, 100, 0,
			dac_soft_mute_get, dac_soft_mute_put),

	SOC_SINGLE_EXT("AVC Bypass Mode", SND_SOC_NOPM, 0, 100, 0,
			avc_bypass_mode_get, avc_bypass_mode_put),

	SOC_SINGLE_EXT("AVC Mute Enable", SND_SOC_NOPM, 0, 100, 0,
			avc_mute_get, avc_mute_put),

	SOC_SINGLE_EXT("MCB2 Chop Mode", SND_SOC_NOPM, 0, 100, 0,
			mcb2_chop_mode_get, mcb2_chop_mode_put),

	SOC_SINGLE_EXT("HP Analog Volume", SND_SOC_NOPM, 0, 100, 0,
			hp_avol_mode_get, hp_avol_mode_put),

	SOC_ENUM("DMIC1 Volume", cod3035x_dmic2_gain_code_level_enum),

	SOC_ENUM("DMIC2 Volume", cod3035x_dmic4_gain_code_level_enum),

	SOC_ENUM("In Mute HP Current", cod3035x_in_mu_ctmi_hpa_code_level_enum),

	SOC_ENUM("Out Mute HP Current", cod3035x_out_mu_ctmi_hpa_code_level_enum),
};

static int dac_ev(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol,
		int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* DAC digital power On */
		snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
				PDB_DACDIG_MASK, PDB_DACDIG_MASK);

		/* DAC digital Reset On/Off */
		snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
				RSTB_DAT_DA_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
				RSTB_DAT_DA_MASK, RSTB_DAT_DA_MASK);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		/* DAC digital Reset Off */
		snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
				RSTB_DAT_DA_MASK, 0x0);

		/* DAC digital power Off */
		snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
				PDB_DACDIG_MASK, 0x0);
		break;

	default:
		break;
	}

	return 0;
}

static void cod3035x_adc_digital_mute(struct snd_soc_codec *codec, bool on)
{
	dev_dbg(codec->dev, "%s called, %s\n", __func__,
			on ? "Mute" : "Unmute");

	if (on)
		snd_soc_update_bits(codec, COD3035X_46_ADC1,
				ADC1_MUTE_AD_EN_MASK, ADC1_MUTE_AD_EN_MASK);
	else
		snd_soc_update_bits(codec, COD3035X_46_ADC1,
				ADC1_MUTE_AD_EN_MASK, 0);
}

static int cod3035x_capture_init_manual_mode(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_write(codec, COD3035X_BC_ODSEL0, 0x03);

	/* VMID On */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_VMID_MASK, PDB_VMID_MASK);

	/* VMID Fast Charging On */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTMF_VMID_MASK, (CTMF_VMID_1K_OM << CTMF_VMID_SHIFT));

	/* VREFP_AD Pre Charging ON */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			EN_DSMC_PREQ_MASK, EN_DSMC_PREQ_MASK);

	/* IGEN On */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_IGEN_MASK, PDB_IGEN_MASK);

	return 0;
}

static int cod3035x_capture_init(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "%s called\n", __func__);

	mutex_lock(&cod3035x->adc_mute_lock);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xFF);
	/* Enable ADC digital mute before configuring ADC */
	cod3035x_adc_digital_mute(codec, true);
	mutex_unlock(&cod3035x->adc_mute_lock);

	/* DAC digital power On */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			PDB_DACDIG_MASK, PDB_DACDIG_MASK);

	/* Recording Digital Power on */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			PDB_ADCDIG_MASK, PDB_ADCDIG_MASK);

	/* Recording Digital Reset on/off */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, RSTB_DAT_AD_MASK);

	/* Power up ADC channel 2 */
	snd_soc_update_bits(codec, COD3035X_4B_ADC2,
			ADC2_PDB_ADCDIG2_MASK, ADC2_PDB_ADCDIG2_MASK);

	cod3035x_capture_init_manual_mode(codec);

	return 0;
}

static int cod3035x_dmic_capture_init(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "%s called\n", __func__);

	mutex_lock(&cod3035x->adc_mute_lock);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xFF);
	/* enable ADC digital mute before configuring ADC */
	cod3035x_adc_digital_mute(codec, true);
	mutex_unlock(&cod3035x->adc_mute_lock);
	/* Recording Digital  Power on */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			PDB_ADCDIG_MASK, PDB_ADCDIG_MASK);

	/* Recording Digital Reset on/off */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, RSTB_DAT_AD_MASK);

	/* Power up ADC channel 2 */
	snd_soc_update_bits(codec, COD3035X_4B_ADC2,
			ADC2_PDB_ADCDIG2_MASK, ADC2_PDB_ADCDIG2_MASK);

	return 0;
}

static void cod3035x_capture_deinit_manual_mode(struct snd_soc_codec *codec)
{
	/* VMID OFF */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF, PDB_VMID_MASK, 0);

	snd_soc_write(codec, COD3035X_BC_ODSEL0, 0x00);
}

static int cod3035x_capture_deinit(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "%s called\n", __func__);

	cod3035x_capture_deinit_manual_mode(codec);

	/* Power down ADC channel 2 */
	snd_soc_update_bits(codec, COD3035X_4B_ADC2, ADC2_PDB_ADCDIG2_MASK, 0);

	/* Recording Digital Reset on */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, 0);

	/* Recording Digital Power off */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			PDB_ADCDIG_MASK, 0);

	mutex_lock(&cod3035x->adc_mute_lock);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xFF);
	/* disable ADC digital mute after configuring ADC */
	cod3035x_adc_digital_mute(codec, false);
	mutex_unlock(&cod3035x->adc_mute_lock);

	return 0;
}

static int cod3035x_dmic_capture_deinit(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	/* Power down ADC channel 2 */
	snd_soc_update_bits(codec, COD3035X_4B_ADC2, ADC2_PDB_ADCDIG2_MASK, 0);

	/* Recording Digital Reset on */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			RSTB_DAT_AD_MASK, 0x0);

	/* Recording Digital  Power off */
	snd_soc_update_bits(codec, COD3035X_40_DIGITAL_POWER,
			PDB_ADCDIG_MASK, 0x0);

	mutex_lock(&cod3035x->adc_mute_lock);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xFF);
	/* disable ADC digital mute after configuring ADC */
	cod3035x_adc_digital_mute(codec, false);
	mutex_unlock(&cod3035x->adc_mute_lock);
	return 0;
}

static int adc_ev(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol,
		int event)
{
	int dac_on;
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);
	dac_on = snd_soc_read(codec, COD3035X_40_DIGITAL_POWER);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		break;

	case SND_SOC_DAPM_POST_PMU:
		/* disable ADC digital mute after configuring ADC */
		queue_work(cod3035x->adc_mute_wq, &cod3035x->adc_mute_work);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		mutex_lock(&cod3035x->adc_mute_lock);
		/* enable ADC digital mute before configuring ADC */
		cod3035x_adc_digital_mute(codec, true);
		mutex_unlock(&cod3035x->adc_mute_lock);
		break;

	default:
		break;
	}

	return 0;
}

static int dadc_ev(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol,
		int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		break;

	case SND_SOC_DAPM_POST_PMU:
		/* disable ADC digital mute after configuring ADC */
		queue_work(cod3035x->adc_mute_wq, &cod3035x->adc_mute_work);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		mutex_lock(&cod3035x->adc_mute_lock);
		/* enable ADC digital mute before configuring ADC */
		cod3035x_adc_digital_mute(codec, true);
		mutex_unlock(&cod3035x->adc_mute_lock);
		break;

	default:
		break;
	}

	return 0;
}

int cod3035x_mic_bias_ev(struct snd_soc_codec *codec, int mic_bias, int event)
{
	int is_other_mic_on, mask;

	dev_dbg(codec->dev, "%s Called, Mic bias = %d, Event = %d\n",
				__func__, mic_bias, event);

	is_other_mic_on = snd_soc_read(codec, COD3035X_10_PD_REF);

	if (mic_bias == COD3035X_MICBIAS1) {
		is_other_mic_on &= PDB_MCB2_CODEC_MASK;
		mask = is_other_mic_on ? PDB_MCB1_MASK :
			PDB_MCB1_MASK | PDB_MCB_LDO_CODEC_MASK;
	} else if (mic_bias == COD3035X_MICBIAS2) {
		is_other_mic_on &= PDB_MCB1_MASK;
		mask = is_other_mic_on ? PDB_MCB2_CODEC_MASK :
			PDB_MCB2_CODEC_MASK | PDB_MCB_LDO_CODEC_MASK;
	} else {
		dev_err(codec->dev, "%s Called , Invalid MIC ID\n", __func__);
		return -1;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (mic_bias == COD3035X_MICBIAS2)
			snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
					CTRM_MCB2_MASK, CTRM_MCB2_MASK);
		else
			snd_soc_update_bits(codec, COD3035X_10_PD_REF, mask, mask);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (mic_bias == COD3035X_MICBIAS2)
			snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
					CTRM_MCB2_MASK, 0);
		else
			snd_soc_update_bits(codec, COD3035X_10_PD_REF, mask, 0);
		break;
	}

	return 0;
}

/**
  * Mute mic if it is active
  *
  * Returns -1 if error, else 0
  */
static int cod3035x_mute_mic(struct snd_soc_codec *codec, bool on)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	dev_dbg(codec->dev, "%s called, %s\n", __func__,
			on ? "Mute" : "Unmute");

	if (on) {
		mutex_lock(&cod3035x->adc_mute_lock);
		cod3035x_adc_digital_mute(codec, true);
		mutex_unlock(&cod3035x->adc_mute_lock);
	} else {
		mutex_lock(&cod3035x->adc_mute_lock);
		cod3035x_adc_digital_mute(codec, false);
		mutex_unlock(&cod3035x->adc_mute_lock);
	}

	return 0;
}

static void cod3035x_save_dac_value(struct snd_soc_codec *codec)
{
	unsigned char lvol = 0x0, rvol = 0x0;
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s called \n", __func__);
	lvol = snd_soc_read(codec, COD3035X_51_DVOLL);
	rvol = snd_soc_read(codec, COD3035X_52_DVOLR);

	if (lvol == 0xff && rvol == 0xff) {
		/* checking the DAC is already muted */
		dev_dbg(codec->dev, "DAC is already muted.\n");
	}
	else {
		/* save the dac gains */
		cod3035x->lvol = lvol;
		cod3035x->rvol = rvol;
	}
}

static void cod3035x_dac_mute(struct snd_soc_codec *codec, bool on)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s called, %s\n", __func__,
			on ? "Mute" : "Unmute");

	if (on) {
		cod3035x_save_dac_value(codec);
		snd_soc_write(codec, COD3035X_51_DVOLL, 0xff);
		snd_soc_write(codec, COD3035X_52_DVOLR, 0xff);
		dev_dbg(codec->dev, "Mute: lvol = 0xff, rvol = 0xff.\n");
	}
	else {
		snd_soc_write(codec, COD3035X_51_DVOLL, cod3035x->lvol);
		snd_soc_write(codec, COD3035X_52_DVOLR, cod3035x->rvol);
		dev_dbg(codec->dev, "Unmute: lvol = 0x%x, rvol = 0x%x.\n",
				cod3035x->lvol, cod3035x->rvol);
	}
}

/* process the button events based on the need */
void cod3035x_process_button_ev(struct snd_soc_codec *codec, int code, int on)
{
	bool key_press = on ? true : false;

	cod3035x_mute_mic(codec, key_press);
}


int dump_register(struct snd_soc_codec *codec)
{
	int i;
	int v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, va, vb, vc, vd, ve, vf;

	dev_dbg(codec->dev, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
	for (i = 0; i < 16; i++) {
		v0 = 0x00FF & snd_soc_read(codec, 0 + i*16);
		cod3035x_usleep(10);
		v1 = 0x00FF & snd_soc_read(codec, 1 + i*16);
		cod3035x_usleep(10);
		v2 = 0x00FF & snd_soc_read(codec, 2 + i*16);
		cod3035x_usleep(10);
		v3 = 0x00FF & snd_soc_read(codec, 3 + i*16);
		cod3035x_usleep(10);
		v4 = 0x00FF & snd_soc_read(codec, 4 + i*16);
		cod3035x_usleep(10);
		v5 = 0x00FF & snd_soc_read(codec, 5 + i*16);
		cod3035x_usleep(10);
		v6 = 0x00FF & snd_soc_read(codec, 6 + i*16);
		cod3035x_usleep(10);
		v7 = 0x00FF & snd_soc_read(codec, 7 + i*16);
		cod3035x_usleep(10);
		v8 = 0x00FF & snd_soc_read(codec, 8 + i*16);
		cod3035x_usleep(10);
		v9 = 0x00FF & snd_soc_read(codec, 9 + i*16);
		cod3035x_usleep(10);
		va = 0x00FF & snd_soc_read(codec, 10 + i*16);
		cod3035x_usleep(10);
		vb = 0x00FF & snd_soc_read(codec, 11 + i*16);
		cod3035x_usleep(10);
		vc = 0x00FF & snd_soc_read(codec, 12 + i*16);
		cod3035x_usleep(10);
		vd = 0x00FF & snd_soc_read(codec, 13 + i*16);
		cod3035x_usleep(10);
		ve = 0x00FF & snd_soc_read(codec, 14 + i*16);
		cod3035x_usleep(10);
		vf = 0x00FF & snd_soc_read(codec, 15 + i*16);
		dev_dbg(codec->dev, "%x0: %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x %2x",
				i,
				v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, va, vb, vc, vd, ve, vf);
		cod3035x_usleep(10);
	}
	return 0;
}

static int cod3035_power_on_mic1(struct snd_soc_codec *codec)
{
	unsigned int mix_val1, mix_val2 = 0;
	dev_dbg(codec->dev, "%s called\n", __func__);
	mix_val1 = snd_soc_read(codec, COD3035X_25_MIX_AD1);
	mix_val1 &= EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK;
	mix_val2 = snd_soc_read(codec, COD3035X_26_MIX_ADC);
	mix_val2 &= EN_MIX_MIC1C_MASK;

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_MIC1C_MASK, 0x0);

	/* ADC Current reference ON */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	/* MIC1 On */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST1_MASK|PDB_MIC_PGA1_MASK,
			PDB_MIC_BST1_MASK|PDB_MIC_PGA1_MASK);

	/* Mixer on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_MIXL_MASK|PDB_MIXR_MASK, PDB_MIXL_MASK|PDB_MIXR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_MIXC_MASK, PDB_MIXC_MASK);

	/* DSM on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_DSML_MASK|PDB_DSMR_MASK, PDB_DSML_MASK|PDB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_DSMC_MASK, PDB_DSMC_MASK);

	/* Reset Micbias1 on */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_BST1_MASK, 0);

	/* Mixer path selection */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_MIC1L_MASK | EN_MIX_MIC1R_MASK, mix_val1);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_MIC1C_MASK, mix_val2);

	msleep(50);

	/* EN_BST_DIODE, RESETB_BST1 OFF */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, EN_BST_DIODE_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_BST1_MASK, RESETB_BST1_MASK);

	/* VREFP_AD Pre Charging OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			EN_DSMC_PREQ_MASK, 0);

	/* Fast VMID Charging OFF */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTMF_VMID_MASK, (CTMF_VMID_60K_OM << CTMF_VMID_SHIFT));

	/* DSM RESET OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			RESETB_DSML_MASK|RESETB_DSMR_MASK,
			RESETB_DSML_MASK|RESETB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_DSMC_MASK, RESETB_DSMC_MASK);

	return 0;
}

static int cod3035_power_on_dmic1(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			EN_DMIC_MASK, EN_DMIC_MASK);
	snd_soc_update_bits(codec, COD3035X_4A_DMIC2,
			DMIC_OSR_MASK, OSR64 << DMIC_OSR_SHIFT);

	return 0;
}

static int cod3035_power_off_mic1(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= (EN_MIC2_MASK | EN_MIC3_MASK | EN_LN_MASK);

	if (!other_mic) {
		/* DSM RESET ON */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				RESETB_DSML_MASK|RESETB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* DSM OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_DSML_MASK|PDB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* MIXER OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_MIXL_MASK|PDB_MIXR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_MIXC_MASK, 0);
	}

	/* MIC1 OFF */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST1_MASK|PDB_MIC_PGA1_MASK, 0);

	return 0;
}

static int cod3035_power_off_dmic1(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= EN_DMIC2_MASK;

	if (!other_mic) {
		snd_soc_update_bits(codec, COD3035X_4A_DMIC2,
			DMIC_GAIN1_MASK | DMIC_OSR_MASK,
			LEVEL1 << DMIC_GAIN1_SHIFT | OSR64 << DMIC_OSR_SHIFT);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			EN_DMIC_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			SEL_DMIC_L_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			SEL_DMIC_R_MASK, 0x0);
	}

	return 0;
}

static int cod3035_power_on_mic2(struct snd_soc_codec *codec)
{
	unsigned int mix_val1, mix_val2 = 0;
	dev_dbg(codec->dev, "%s called\n", __func__);
	mix_val1 = snd_soc_read(codec, COD3035X_25_MIX_AD1);
	mix_val1 &= EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK;
	mix_val2 = snd_soc_read(codec, COD3035X_26_MIX_ADC);
	mix_val2 &= EN_MIX_MIC2C_MASK;

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
					EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
					EN_MIX_MIC2C_MASK, 0x0);

	/* ADC Current reference ON */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	/* MIC2 On */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST2_MASK|PDB_MIC_PGA2_MASK,
			PDB_MIC_BST2_MASK|PDB_MIC_PGA2_MASK);

	/* Mixer on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_MIXL_MASK|PDB_MIXR_MASK, PDB_MIXL_MASK|PDB_MIXR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_MIXC_MASK, PDB_MIXC_MASK);

	/* DSM on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_DSML_MASK|PDB_DSMR_MASK, PDB_DSML_MASK|PDB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_DSMC_MASK, PDB_DSMC_MASK);

	/* Reset Micbias2 on */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_BST2_MASK, 0);

	/* Mixer path selection */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_MIC2L_MASK | EN_MIX_MIC2R_MASK, mix_val1);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_MIC2C_MASK, mix_val2);

	msleep(50);

	/* EN_BST_DIODE, RESETB_BST2 OFF */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, EN_BST_DIODE_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_BST2_MASK, RESETB_BST2_MASK);

	/* VREFP_AD Pre Charging OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			EN_DSMC_PREQ_MASK, 0);

	/* Fast VMID Charging OFF */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTMF_VMID_MASK, (CTMF_VMID_60K_OM << CTMF_VMID_SHIFT));

	/* DSM RESET OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			RESETB_DSML_MASK|RESETB_DSMR_MASK,
			RESETB_DSML_MASK|RESETB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_DSMC_MASK, RESETB_DSMC_MASK);

	return 0;
}

static int cod3035_power_on_dmic2(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			EN_DMIC_MASK, EN_DMIC_MASK);
	snd_soc_update_bits(codec, COD3035X_4A_DMIC2,
			DMIC_OSR_MASK, OSR64 << DMIC_OSR_SHIFT);

	return 0;
}

static int cod3035_power_off_mic2(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= (EN_MIC1_MASK | EN_MIC3_MASK | EN_LN_MASK);

	if (!other_mic) {
		/* DSM RESET ON */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				RESETB_DSML_MASK|RESETB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* DSM OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_DSML_MASK|PDB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* MIXER OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_MIXL_MASK|PDB_MIXR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_MIXC_MASK, 0);
	}

	/* MIC2 OFF */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST2_MASK|PDB_MIC_PGA2_MASK, 0);

	return 0;
}

static int cod3035_power_off_dmic2(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= EN_DMIC1_MASK;

	if (!other_mic) {
		snd_soc_update_bits(codec, COD3035X_4A_DMIC2,
			DMIC_GAIN1_MASK | DMIC_OSR_MASK,
		LEVEL1 << DMIC_GAIN1_SHIFT | OSR64 << DMIC_OSR_SHIFT);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
		    EN_DMIC_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			SEL_DMIC_L_MASK, 0x0);
		snd_soc_update_bits(codec, COD3035X_49_DMIC1,
			SEL_DMIC_R_MASK, 0x0);
	}

	return 0;
}

static int cod3035_power_on_mic3(struct snd_soc_codec *codec)
{
	unsigned int mix_val1, mix_val2 = 0;
	dev_dbg(codec->dev, "%s called\n", __func__);
	mix_val1 = snd_soc_read(codec, COD3035X_25_MIX_AD1);
	mix_val1 &= EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK;
	mix_val2 = snd_soc_read(codec, COD3035X_26_MIX_ADC);
	mix_val2 &= EN_MIX_MIC3C_MASK;

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_MIC3C_MASK, 0x0);

	/* ADC Current reference ON */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	/* MIC2 On */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST3_MASK|PDB_MIC_PGA3_MASK,
			PDB_MIC_BST3_MASK|PDB_MIC_PGA3_MASK);

	/* Mixer on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_MIXL_MASK|PDB_MIXR_MASK, PDB_MIXL_MASK|PDB_MIXR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_MIXC_MASK, PDB_MIXC_MASK);

	/* DSM on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_DSML_MASK|PDB_DSMR_MASK, PDB_DSML_MASK|PDB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			PDB_DSMC_MASK, PDB_DSMC_MASK);

	/* Reset Micbias3 on */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_BST3_MASK, 0);

	/* Mixer path selection */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_MIC3L_MASK | EN_MIX_MIC3R_MASK, mix_val1);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_MIC3C_MASK, mix_val2);

	msleep(50);

	/* EN_BST_DIODE, RESETB_BST2 OFF */
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3, EN_BST_DIODE_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_BST3_MASK, RESETB_BST3_MASK);

	/* VREFP_AD Pre Charging OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK, 0);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			EN_DSMC_PREQ_MASK, 0);

	/* Fast VMID Charging OFF */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTMF_VMID_MASK, (CTMF_VMID_60K_OM << CTMF_VMID_SHIFT));

	/* DSM RESET OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			RESETB_DSML_MASK|RESETB_DSMR_MASK,
			RESETB_DSML_MASK|RESETB_DSMR_MASK);
	snd_soc_update_bits(codec, COD3035X_13_PD_AD3,
			RESETB_DSMC_MASK, RESETB_DSMC_MASK);

	return 0;
}

static int cod3035_power_off_mic3(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= (EN_MIC1_MASK | EN_MIC2_MASK | EN_LN_MASK);

	if (!other_mic) {
		/* DSM RESET ON */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				RESETB_DSML_MASK|RESETB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, RESETB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* DSM OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_DSML_MASK|PDB_DSMR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_DSMC_MASK, 0);
	}

	if (!other_mic) {
		/* MIXER OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_MIXL_MASK|PDB_MIXR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_MIXC_MASK, 0);
	}

	/* MIC3 OFF */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_MIC_BST3_MASK|PDB_MIC_PGA3_MASK, 0);

	return 0;
}

static int cod3035_power_on_linein(struct snd_soc_codec *codec)
{
	unsigned int mix_val1, mix_val2, mix_val3 = 0;
	dev_dbg(codec->dev, "%s called\n", __func__);
	mix_val1 = snd_soc_read(codec, COD3035X_25_MIX_AD1);
	mix_val1 &= EN_MIX_LNLL_MASK | EN_MIX_LNRR_MASK;
	mix_val2 = snd_soc_read(codec, COD3035X_26_MIX_ADC);
	mix_val2 &= EN_MIX_LNLC_MASK | EN_MIX_LNRC_MASK;
	mix_val3 = snd_soc_read(codec, COD3035X_27_MIX_AD2);
	mix_val3 &= EN_MIX_LNLR_MASK | EN_MIX_LNRL_MASK;

	/* Reset the mixer-switches before powering on */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_LNLL_MASK | EN_MIX_LNRR_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_LNLC_MASK | EN_MIX_LNRC_MASK, 0x0);
	snd_soc_update_bits(codec, COD3035X_27_MIX_AD2,
			EN_MIX_LNLR_MASK | EN_MIX_LNRL_MASK, 0x0);

	/* RESETB_LN ON */
	snd_soc_update_bits(codec, COD3035X_28_DSM_ADS,
			RESETB_LN_MASK, RESETB_LN_MASK);

	/* IGEN_AD ON */
	snd_soc_update_bits(codec, COD3035X_10_PD_REF,
			PDB_IGEN_AD_MASK, PDB_IGEN_AD_MASK);

	/* Mixer on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_MIXL_MASK|PDB_MIXR_MASK, PDB_MIXL_MASK|PDB_MIXR_MASK);

	/* DSM on */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			PDB_DSML_MASK|PDB_DSMR_MASK, PDB_DSML_MASK|PDB_DSMR_MASK);

	/* Mixer path selection */
	snd_soc_update_bits(codec, COD3035X_25_MIX_AD1,
			EN_MIX_LNLL_MASK | EN_MIX_LNRR_MASK, mix_val1);
	snd_soc_update_bits(codec, COD3035X_26_MIX_ADC,
			EN_MIX_LNLC_MASK | EN_MIX_LNRC_MASK, mix_val2);
	snd_soc_update_bits(codec, COD3035X_27_MIX_AD2,
			EN_MIX_LNLR_MASK | EN_MIX_LNRL_MASK, mix_val3);

	msleep(50);

	/* RESETB_LN OFF */
	snd_soc_update_bits(codec, COD3035X_28_DSM_ADS,
			RESETB_LN_MASK, 0);

	/* VREFP_AD Pre Charging OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			EN_DSML_PREQ_MASK|EN_DSMR_PREQ_MASK, 0);

	/* Fast VMID Charging OFF */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTMF_VMID_MASK, (CTMF_VMID_60K_OM << CTMF_VMID_SHIFT));

	/* DSM RESET OFF */
	snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
			RESETB_DSML_MASK|RESETB_DSMR_MASK,
			RESETB_DSML_MASK|RESETB_DSMR_MASK);

	return 0;
}

static int cod3035_power_off_linein(struct snd_soc_codec *codec)
{
	int other_mic;
	dev_dbg(codec->dev, "%s called\n", __func__);

	other_mic = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	other_mic &= (EN_MIC1_MASK | EN_MIC2_MASK | EN_MIC3_MASK);

	if (!other_mic) {
		/* DSM RESET ON */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				RESETB_DSML_MASK|RESETB_DSMR_MASK, 0);
	}

	if (!other_mic) {
		/* DSM OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_DSML_MASK|PDB_DSMR_MASK, 0);
	}

	if (!other_mic) {
		/* MIXER OFF */
		snd_soc_update_bits(codec, COD3035X_11_PD_AD1,
				PDB_MIXL_MASK|PDB_MIXR_MASK, 0);
		snd_soc_update_bits(codec, COD3035X_13_PD_AD3, PDB_MIXC_MASK, 0);
	}

	/* LINE OFF */
	snd_soc_update_bits(codec, COD3035X_12_PD_AD2,
			PDB_LNL_MASK | PDB_LNR_MASK, 0);

	return 0;
}

static int vmid_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);

	dev_dbg(codec->dev, "%s called\n", __func__);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035x_capture_init(codec);
		break;

	case SND_SOC_DAPM_POST_PMU:
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035x_capture_deinit(codec);
		break;

	default:
		break;
	}

	return 0;
}

static int dvmid_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	dev_dbg(codec->dev, "%s called\n", __func__);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035x_dmic_capture_init(codec);
		break;

	case SND_SOC_DAPM_POST_PMU:
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035x_dmic_capture_deinit(codec);
		break;

	default:
		break;
	}

	return 0;
}

static int cod3035x_hp_playback_init(struct snd_soc_codec *codec)
{
	dev_dbg(codec->dev, "%s called\n", __func__);

	/* 0x45: |0x10, DAC Input Data Selection */
	snd_soc_update_bits(codec, COD3035X_45_IF1_FORMAT5, 0x10, 0x10);

	cod3035x_usleep(100);

	return 0;
}

static int hpdrv_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int hp_on, ep_on;
	int chop_val;
	int avc_val;
	int hp_avol;
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);

	avc_val = snd_soc_read(codec, COD3035X_54_AVC1);
	hp_avol = snd_soc_read(codec, COD3035X_30_VOL_HPL);
	chop_val = snd_soc_read(codec, COD3035X_77_CHOP_DA);
	hp_on = chop_val & EN_HP_CHOP_MASK;
	ep_on = chop_val & EN_EP_CHOP_MASK;

	if (!hp_on) {
		dev_dbg(codec->dev, "%s called but headphone not enabled\n",
				__func__);
		return 0;
	}

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035x_dac_mute(codec, true);
		cod3035x_hp_playback_init(codec);
		break;

	case SND_SOC_DAPM_POST_PMU:
		if (avc_val & AVC_BYPS_MASK) {
			snd_soc_update_bits(codec, COD3035X_54_AVC1,
					DAC_VOL_BYPS_MASK | AVC_BYPS_MASK, DAC_VOL_BYPS_MASK | AVC_BYPS_MASK);
					snd_soc_write(codec, COD3035X_B9_AUTO_HP10, hp_avol);
		}
		snd_soc_update_bits(codec, COD3035X_37_MIX_DA1,
				EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK,
				EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK);

		snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
				APW_HP_MASK, APW_HP_MASK);

		msleep(135);

		cod3035x_dac_mute(codec, false);
		snd_soc_write(codec, COD3035X_BA_AUTO_HP11, 0x05);

		snd_soc_update_bits(codec, COD3035X_3E_OVP_2,
		        LOCK_UP_TIME_SLOT_MASK | OVP_APON_MASK,
				LOCK_UP_TIME_1_5 << LOCK_UP_TIME_SLOT_SHIFT | OVP_APON_MASK);

		if (avc_val & AVC_BYPS_MASK)
			snd_soc_write(codec, COD3035X_BE_ODSEL2, 0x60);

		snd_soc_update_bits(codec, COD3035X_78_CTRL_CP,
				CTMF_CP_CLK_MASK, CTMF_CP_CLK_195KHZ << CTMF_CP_CLK_SHIFT);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035x_dac_mute(codec, true);
		msleep(100);
		snd_soc_write(codec, COD3035X_BA_AUTO_HP11, 0x02);

		snd_soc_update_bits(codec, COD3035X_78_CTRL_CP,
				CTMF_CP_CLK_MASK, CTMF_CP_CLK_780KHZ << CTMF_CP_CLK_SHIFT);

		/* 0x18 <-- 0x00: ~0x02, HP Path Auto Power Off */
		if (ep_on) {
			snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
					APW_HP_MASK, 0);
		} else {
			snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
					PW_AUTO_DA_MASK | APW_HP_MASK, 0);
		}

		cod3035x_usleep(3000);

		snd_soc_update_bits(codec, COD3035X_3E_OVP_2,
		        OVP_APON_MASK, 0);

		snd_soc_update_bits(codec, COD3035X_37_MIX_DA1,
				EN_HP_MIXL_DCTL_MASK | EN_HP_MIXR_DCTR_MASK, 0);

		break;

	default:
		break;
	}

	return 0;
}

static int epdrv_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int hp_on, ep_on;
	int chop_val;
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);

	chop_val = snd_soc_read(codec, COD3035X_77_CHOP_DA);
	hp_on = chop_val & EN_HP_CHOP_MASK;
	ep_on = chop_val & EN_EP_CHOP_MASK;

	if (!ep_on) {
		dev_dbg(codec->dev, "%s called but ear-piece not enabled\n",
				__func__);
		return 0;
	}

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, COD3035X_54_AVC1,
				AVC_BYPS_MASK, AVC_BYPS_MASK);

		snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
				APW_EP_MASK | PW_AUTO_DA_MASK,
				APW_EP_MASK | PW_AUTO_DA_MASK);

		snd_soc_update_bits(codec, COD3035X_38_MIX_DA2,
				EN_EP_MIX_DCTL_MASK, EN_EP_MIX_DCTL_MASK);

		msleep(136);

		snd_soc_update_bits(codec, COD3035X_78_CTRL_CP,
	        CTMF_CP_CLK_MASK, CTMF_CP_CLK_97_5KHZ << CTMF_CP_CLK_SHIFT);

		break;

	case SND_SOC_DAPM_PRE_PMD:

		snd_soc_update_bits(codec, COD3035X_54_AVC1,
				AVC_BYPS_MASK, 0x0);

		snd_soc_update_bits(codec, COD3035X_78_CTRL_CP,
			CTMF_CP_CLK_MASK, CTMF_CP_CLK_780KHZ << CTMF_CP_CLK_SHIFT);

		snd_soc_write(codec, COD3035X_32_VOL_EP, 0x06);

		if (hp_on) {
			snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
					APW_EP_MASK, 0x0);
		} else {
			snd_soc_update_bits(codec, COD3035X_18_PWAUTO_DA,
					PW_AUTO_DA_MASK | APW_EP_MASK, 0x0);
		}
		cod3035x_usleep(1000);

		snd_soc_update_bits(codec, COD3035X_38_MIX_DA2,
				EN_EP_MIX_DCTL_MASK, 0x0);

		break;

	default:
		break;
	}

	return 0;
}

static int mic1_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	int mic_on;

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(mic_on & EN_MIC1_MASK)) {
		dev_dbg(codec->dev, "%s: MIC1 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_mic1(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_mic1(codec);
		break;

	default:
		break;
	}

	return 0;
}

static int dmic1_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int mic_on;
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(mic_on & EN_DMIC1_MASK)) {
		dev_dbg(codec->dev, "%s: DMIC1 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_dmic1(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_dmic1(codec);
		break;

	default:
		break;
	}

	return 0;
}

static int mic2_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	int mic_on;

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(mic_on & EN_MIC2_MASK)) {
		dev_dbg(codec->dev, "%s: MIC2 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_mic2(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_mic2(codec);
		break;
	default:
		break;
	}

	return 0;
}

static int dmic2_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	int mic_on;
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(mic_on & EN_DMIC2_MASK)) {
		dev_dbg(codec->dev, "%s: DMIC2 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_dmic2(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_dmic2(codec);
		break;
	default:
		break;
	}

	return 0;
}
static int mic3_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	int mic_on;

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	mic_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(mic_on & EN_MIC3_MASK)) {
		dev_dbg(codec->dev, "%s: MIC3 is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_mic3(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_mic3(codec);
		break;

	default:
		break;
	}

	return 0;
}

static int linein_pga_ev(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	int linein_on;

	dev_dbg(codec->dev, "%s called, event = %d\n", __func__, event);

	linein_on = snd_soc_read(codec, COD3035X_6C_MIC_ON);
	if (!(linein_on & EN_LN_MASK)) {
		dev_dbg(codec->dev, "%s: LINE IN is not enabled, returning.\n",
								__func__);
		return 0;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		cod3035_power_on_linein(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		cod3035_power_off_linein(codec);
		break;

	default:
		break;
	}

	return 0;
}

/*DMIC1 DMIC1 L [6:4] */
static const char * const cod3035x_dmicl_src1[] = {
		"AMICL DMIC1L", "AMICR DMIC1L", "AMICC DMIC1L", "Zero DMIC1L",
		"DMIC1L DMIC1L", "DMIC1R DMIC1L", "DMIC2L DMIC1L", "DMIC2R DMIC1L"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_dmicl_enum1, COD3035X_49_DMIC1,
		SEL_DMIC_L_SHIFT, cod3035x_dmicl_src1);

static const struct snd_kcontrol_new cod3035x_dmicl_mux1 =
		SOC_DAPM_ENUM("DMICL Mux1", cod3035x_dmicl_enum1);

/*DMIC1 DMIC1 R [2:0] */
static const char * const cod3035x_dmicr_src1[] = {
		"AMICR DMIC1R", "AMICL DMIC1R", "AMICC DMIC1R", "Zero DMIC1R",
		"DMIC1L DMIC1R", "DMIC1R DMIC1R", "DMIC2L DMIC1R", "DMIC2R DMIC1R"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_dmicr_enum1, COD3035X_49_DMIC1,
		SEL_DMIC_R_SHIFT, cod3035x_dmicr_src1);

static const struct snd_kcontrol_new cod3035x_dmicr_mux1 =
		SOC_DAPM_ENUM("DMICR Mux1", cod3035x_dmicr_enum1);

/*DMIC3 DMIC3 L [6:4] */
static const char * const cod3035x_dmicl_src2[] = {
		"AMICL DMIC2L", "AMICR DMIC2L", "AMICC DMIC2L", "Zero DMIC2L",
		"DMIC1L DMIC2L", "DMIC1R DMIC2L", "DMIC2L DMIC2L", "DMIC2R DMIC2L"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_dmicl_enum2, COD3035X_4E_DMIC3,
		SEL_DMIC_L_SHIFT, cod3035x_dmicl_src2);

static const struct snd_kcontrol_new cod3035x_dmicl_mux2 =
		SOC_DAPM_ENUM("DMICL Mux2", cod3035x_dmicl_enum2);

/*DMIC3 DMIC3 R [2:0] */
static const char * const cod3035x_dmicr_src2[] = {
		"AMICR DMIC2R", "AMICL DMIC2R", "AMICC DMIC2R", "Zero DMIC2R",
		"DMIC1L DMIC2R", "DMIC1R DMIC2R", "DMIC2L DMIC2R", "DMIC2R DMIC2R"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_dmicr_enum2, COD3035X_4E_DMIC3,
		SEL_DMIC_R_SHIFT, cod3035x_dmicr_src2);

static const struct snd_kcontrol_new cod3035x_dmicr_mux2 =
		SOC_DAPM_ENUM("DMICR Mux2", cod3035x_dmicr_enum2);

/*SEL_ADC0 [1:0] */
static const char * const cod3035x_adc_dat_src0[] = {
        "ADC1", "ADC2", "ADC3", "Off"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_adc_dat_enum0, COD3035X_44_IF1_FORMAT4,
        SEL_ADC0_SHIFT, cod3035x_adc_dat_src0);

static const struct snd_kcontrol_new cod3035x_adc_dat_mux0 =
        SOC_DAPM_ENUM("ADC DAT Mux0", cod3035x_adc_dat_enum0);

/*SEL_ADC1 [3:2] */
static const char * const cod3035x_adc_dat_src1[] = {
        "ADC1", "ADC2", "ADC3", "Off"
};

static SOC_ENUM_SINGLE_DECL(cod3035x_adc_dat_enum1,
COD3035X_44_IF1_FORMAT4,
        SEL_ADC1_SHIFT, cod3035x_adc_dat_src1);

static const struct snd_kcontrol_new cod3035x_adc_dat_mux1 =
        SOC_DAPM_ENUM("ADC DAT Mux1", cod3035x_adc_dat_enum1);

static const struct snd_kcontrol_new adcl_mix[] = {
	SOC_DAPM_SINGLE("MIC1L Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC1L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC2L Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC2L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC3L Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC3L_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINELL Switch", COD3035X_25_MIX_AD1,
			EN_MIX_LNLL_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINERL Switch", COD3035X_27_MIX_AD2,
			EN_MIX_LNRL_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new adcr_mix[] = {
	SOC_DAPM_SINGLE("MIC1R Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC1R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC2R Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC2R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("MIC3R Switch", COD3035X_25_MIX_AD1,
			EN_MIX_MIC3R_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINELR Switch", COD3035X_27_MIX_AD2,
			EN_MIX_LNLR_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINERR Switch", COD3035X_25_MIX_AD1,
			EN_MIX_LNRR_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new adcc_mix[] = {
    SOC_DAPM_SINGLE("MIC1C Switch", COD3035X_26_MIX_ADC,
            EN_MIX_MIC1C_SHIFT, 1, 0),
    SOC_DAPM_SINGLE("MIC2C Switch", COD3035X_26_MIX_ADC,
            EN_MIX_MIC2C_SHIFT, 1, 0),
    SOC_DAPM_SINGLE("MIC3C Switch", COD3035X_26_MIX_ADC,
            EN_MIX_MIC3C_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINELC Switch", COD3035X_26_MIX_ADC,
        EN_MIX_LNLC_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("LINERC Switch", COD3035X_26_MIX_ADC,
        EN_MIX_LNRC_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new hp_on[] = {
	SOC_DAPM_SINGLE("HP On", COD3035X_77_CHOP_DA, EN_HP_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new ep_on[] = {
	SOC_DAPM_SINGLE("EP On", COD3035X_77_CHOP_DA, EN_EP_CHOP_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic1_on[] = {
	SOC_DAPM_SINGLE("MIC1 On", COD3035X_6C_MIC_ON,
			EN_MIC1_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic2_on[] = {
	SOC_DAPM_SINGLE("MIC2 On", COD3035X_6C_MIC_ON,
			EN_MIC2_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new mic3_on[] = {
	SOC_DAPM_SINGLE("MIC3 On", COD3035X_6C_MIC_ON,
			EN_MIC3_SHIFT, 1, 0),
};

static const char * const cod3035x_fm_texts[] = {
	"None",
	"FM On",
};

static SOC_ENUM_SINGLE_VIRT_DECL(cod3035x_fm_enum, cod3035x_fm_texts);

static const struct snd_kcontrol_new cod3035x_fm_mux[] = {
	SOC_DAPM_ENUM("FM Link", cod3035x_fm_enum),
};

static const struct snd_kcontrol_new linein_on[] = {
	SOC_DAPM_SINGLE("LINEIN On", COD3035X_6C_MIC_ON,
					EN_LN_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new dmic1_on[] = {
	SOC_DAPM_SINGLE("DMIC1 On", COD3035X_6C_MIC_ON,
					EN_DMIC1_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new dmic2_on[] = {
	SOC_DAPM_SINGLE("DMIC2 On", COD3035X_6C_MIC_ON,
					EN_DMIC2_SHIFT, 1, 0),
};

static const struct snd_soc_dapm_widget cod3035x_dapm_widgets[] = {
	SND_SOC_DAPM_SWITCH("HP", SND_SOC_NOPM, 0, 0, hp_on),
	SND_SOC_DAPM_SWITCH("EP", SND_SOC_NOPM, 0, 0, ep_on),
	SND_SOC_DAPM_SWITCH("MIC1", SND_SOC_NOPM, 0, 0, mic1_on),
	SND_SOC_DAPM_SWITCH("MIC2", SND_SOC_NOPM, 0, 0, mic2_on),
	SND_SOC_DAPM_SWITCH("MIC3", SND_SOC_NOPM, 0, 0, mic3_on),
	SND_SOC_DAPM_SWITCH("LINEIN", SND_SOC_NOPM, 0, 0, linein_on),
	SND_SOC_DAPM_SWITCH("DMIC1", SND_SOC_NOPM, 0, 0, dmic1_on),
	SND_SOC_DAPM_SWITCH("DMIC2", SND_SOC_NOPM, 0, 0, dmic2_on),

	SND_SOC_DAPM_SUPPLY("VMID", SND_SOC_NOPM, 0, 0, vmid_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_SUPPLY("DVMID", SND_SOC_NOPM, 0, 0, dvmid_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_OUT_DRV_E("EPDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			epdrv_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HPDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			hpdrv_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MUX("FM Link", SND_SOC_NOPM, 0, 0,
			cod3035x_fm_mux),
	SND_SOC_DAPM_PGA_E("MIC1_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic1_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("MIC2_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic2_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("MIC3_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, mic3_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("LINEIN_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, linein_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("DMIC1_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, dmic1_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("DMIC2_PGA", SND_SOC_NOPM, 0, 0,
			NULL, 0, dmic2_pga_ev,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MIXER("ADCL Mixer", SND_SOC_NOPM, 0, 0, adcl_mix,
			ARRAY_SIZE(adcl_mix)),
	SND_SOC_DAPM_MIXER("ADCC Mixer", SND_SOC_NOPM, 0, 0, adcc_mix,
        ARRAY_SIZE(adcc_mix)),
	SND_SOC_DAPM_MIXER("ADCR Mixer", SND_SOC_NOPM, 0, 0, adcr_mix,
			ARRAY_SIZE(adcr_mix)),

	SND_SOC_DAPM_MUX("DMICL Mux1", SND_SOC_NOPM, 0, 0, &cod3035x_dmicl_mux1),
	SND_SOC_DAPM_MUX("DMICR Mux1", SND_SOC_NOPM, 0, 0, &cod3035x_dmicr_mux1),

	SND_SOC_DAPM_MUX("DMICL Mux2", SND_SOC_NOPM, 0, 0, &cod3035x_dmicl_mux2),
	SND_SOC_DAPM_MUX("DMICR Mux2", SND_SOC_NOPM, 0, 0, &cod3035x_dmicr_mux2),

	SND_SOC_DAPM_MUX("ADC DAT Mux0", SND_SOC_NOPM, 0, 0, &cod3035x_adc_dat_mux0),
	SND_SOC_DAPM_MUX("ADC DAT Mux1", SND_SOC_NOPM, 0, 0, &cod3035x_adc_dat_mux1),

	SND_SOC_DAPM_DAC_E("DAC", "AIF Playback", SND_SOC_NOPM, 0, 0,
			dac_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_DAC_E("DAC", "AIF2 Playback", SND_SOC_NOPM, 0, 0,
			dac_ev, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_ADC_E("ADC", "AIF Capture", SND_SOC_NOPM, 0, 0,
			adc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("ADC", "AIF2 Capture", SND_SOC_NOPM, 0, 0,
			adc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("DADC", "AIF Capture", SND_SOC_NOPM, 0, 0,
			dadc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_ADC_E("DADC", "AIF2 Capture", SND_SOC_NOPM, 0, 0,
			dadc_ev, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_OUTPUT("HPOUTLN"),
	SND_SOC_DAPM_OUTPUT("EPOUTN"),
	SND_SOC_DAPM_OUTPUT("AIF4OUT"),

	SND_SOC_DAPM_INPUT("IN1L"),
	SND_SOC_DAPM_INPUT("IN2L"),
	SND_SOC_DAPM_INPUT("IN3L"),
	SND_SOC_DAPM_INPUT("IN4L"),

	SND_SOC_DAPM_INPUT("AIF4IN"),
};

static const struct snd_soc_dapm_route cod3035x_dapm_routes[] = {
	/* Sink, Control, Source */
	{"EPDRV", NULL, "DAC"},
	{"EP", "EP On", "EPDRV"},
	{"EPOUTN", NULL, "EP"},

	{"HPDRV", NULL, "DAC"},
	{"HP", "HP On", "HPDRV"},
	{"HPOUTLN", NULL, "HP"},

	{"DAC", NULL, "AIF Playback"},
	{"DAC", NULL, "AIF2 Playback"},

	{"MIC1_PGA", NULL, "IN1L"},
	{"MIC1_PGA", NULL, "VMID"},
	{"MIC1", "MIC1 On", "MIC1_PGA"},

	{"ADCL Mixer", "MIC1L Switch", "MIC1"},
	{"ADCC Mixer", "MIC1C Switch", "MIC1"},
	{"ADCR Mixer", "MIC1R Switch", "MIC1"},

	{"DMIC1_PGA", NULL, "IN1L"},
	{"DMIC1_PGA", NULL, "DVMID"},
	{"DMIC1", "DMIC1 On", "DMIC1_PGA"},

	{"DMICL Mux1", "DMIC1L DMIC1L", "DMIC1"},
	{"DMICL Mux1", "DMIC1R DMIC1L", "DMIC1"},
	{"DMICR Mux1", "DMIC1L DMIC1R", "DMIC1"},
	{"DMICR Mux1", "DMIC1R DMIC1R", "DMIC1"},
	{"DMICL Mux1", "DMIC2L DMIC1L", "DMIC1"},
	{"DMICL Mux1", "DMIC2R DMIC1L", "DMIC1"},
	{"DMICR Mux1", "DMIC2L DMIC1R", "DMIC1"},
	{"DMICR Mux1", "DMIC2R DMIC1R", "DMIC1"},

	{"MIC2_PGA", NULL, "IN2L"},
	{"MIC2_PGA", NULL, "VMID"},
	{"MIC2", "MIC2 On", "MIC2_PGA"},

	{"ADCL Mixer", "MIC2L Switch", "MIC2"},
	{"ADCC Mixer", "MIC2C Switch", "MIC2"},
	{"ADCR Mixer", "MIC2R Switch", "MIC2"},

	{"DMIC2_PGA", NULL, "IN2L"},
	{"DMIC2_PGA", NULL, "DVMID"},
	{"DMIC2", "DMIC2 On", "DMIC2_PGA"},

	{"DMICL Mux1", "DMIC1L DMIC1L", "DMIC2"},
	{"DMICL Mux1", "DMIC1R DMIC1L", "DMIC2"},
	{"DMICR Mux1", "DMIC1L DMIC1R", "DMIC2"},
	{"DMICR Mux1", "DMIC1R DMIC1R", "DMIC2"},
	{"DMICL Mux1", "DMIC2L DMIC1L", "DMIC2"},
	{"DMICL Mux1", "DMIC2R DMIC1L", "DMIC2"},
	{"DMICR Mux1", "DMIC2L DMIC1R", "DMIC2"},
	{"DMICR Mux1", "DMIC2R DMIC1R", "DMIC2"},

	{"MIC3_PGA", NULL, "IN3L"},
	{"MIC3_PGA", NULL, "VMID"},
	{"MIC3", "MIC3 On", "MIC3_PGA"},

	{"ADCL Mixer", "MIC3L Switch", "MIC3"},
	{"ADCC Mixer", "MIC3C Switch", "MIC3"},
	{"ADCR Mixer", "MIC3R Switch", "MIC3"},

	{"LINEIN_PGA", NULL, "IN4L"},
	{"LINEIN_PGA", NULL, "VMID"},
	{"LINEIN", "LINEIN On", "LINEIN_PGA"},

	{"ADCL Mixer", "LINELL Switch", "LINEIN"},
	{"ADCL Mixer", "LINERL Switch", "LINEIN"},
	{"ADCC Mixer", "LINELC Switch", "LINEIN"},
	{"ADCC Mixer", "LINERC Switch", "LINEIN"},
	{"ADCR Mixer", "LINELR Switch", "LINEIN"},
	{"ADCR Mixer", "LINERR Switch", "LINEIN"},

	{"ADC", NULL, "ADCL Mixer"},
	{"ADC", NULL, "ADCC Mixer"},
	{"ADC", NULL, "ADCR Mixer"},

	{"DADC", NULL, "DMICL Mux1"},
	{"DADC", NULL, "DMICR Mux1"},
	{"DADC", NULL, "DMICL Mux2"},
	{"DADC", NULL, "DMICR Mux2"},

	{"AIF Capture", NULL, "ADC"},
	{"AIF2 Capture", NULL, "ADC"},

	{"AIF Capture", NULL, "DADC"},
	{"AIF2 Capture", NULL, "DADC"},

	{"FM Link", "FM On", "ADC"},
	{"DAC", NULL, "FM Link"},
};

static int cod3035x_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	dev_dbg(codec->dev, "%s called. fmt: %d\n", __func__, fmt);

	/* I2S Mode */
	snd_soc_write(codec, COD3035X_41_IF1_FORMAT1, 0x00);

	/* length set to 16 bits */
	snd_soc_write(codec, COD3035X_42_IF1_FORMAT2, 0x10);

	/* BCLK : 32fs */
	snd_soc_write(codec, COD3035X_43_IF1_FORMAT3, 0x20);

	return 0;
}

static int cod3035x_dai_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	dev_dbg(codec->dev, "(%s) %s completed\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

static void cod3035x_dai_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	dev_dbg(codec->dev, "(%s) %s completed\n",
			substream->stream ? "C" : "P", __func__);
}

static void cod3035x_adc_mute_work(struct work_struct *work)
{
	struct cod3035x_priv *cod3035x =
		container_of(work, struct cod3035x_priv, adc_mute_work);
	struct snd_soc_codec *codec = cod3035x->codec;

	mutex_lock(&cod3035x->adc_mute_lock);
	msleep(200);
	dev_dbg(codec->dev, "%s called\n", __func__);
	cod3035x_adc_digital_mute(codec, false);
	cod3035x_usleep(2000);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xE4);
	mutex_unlock(&cod3035x->adc_mute_lock);
}

static int cod3035x_dai_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	unsigned int cur_aifrate, width;

	/* 192 KHz support */
	cur_aifrate = params_rate(params);
	width = params_width(params);

	dev_dbg(codec->dev, "%s called. cur_aifrate: %d\n", __func__, cur_aifrate);
	dev_dbg(codec->dev, "%s called. width: %d\n", __func__, width);

	if (width == 16) {
		snd_soc_write(codec, COD3035X_41_IF1_FORMAT1, 0x00);
		snd_soc_update_bits(codec, COD3035X_42_IF1_FORMAT2,
				I2S1_DL_MASK, I2S1_DL_16BIT);
		snd_soc_update_bits(codec, COD3035X_43_IF1_FORMAT3,
				I2S1_XFS_MASK, I2S1_XFS_32FS);
	} else if (width == 24) {
		snd_soc_write(codec, COD3035X_41_IF1_FORMAT1, 0x20);
		snd_soc_update_bits(codec, COD3035X_42_IF1_FORMAT2,
				I2S1_DL_MASK, I2S1_DL_24BIT);
		snd_soc_update_bits(codec, COD3035X_43_IF1_FORMAT3,
				I2S1_XFS_MASK, I2S1_XFS_64FS);
	}

	if (cod3035x->aifrate != cur_aifrate) {
		if (cur_aifrate == COD3035X_SAMPLE_RATE_192KHZ) {
			dev_dbg(codec->dev, "%s called. UHQA Mode\n", __func__);
			snd_soc_write(codec, COD3035X_F2_PRESET_AVC, 0x03);
			snd_soc_write(codec, COD3035X_F3_PRESET_AVC, 0x1F);
			snd_soc_write(codec, COD3035X_F4_PRESET_AVC, 0x5F);
			snd_soc_write(codec, COD3035X_5A_AVC7, 0xFF);
			snd_soc_write(codec, COD3035X_F5_PRESET_AVC, 0x10);
			snd_soc_write(codec, COD3035X_F6_PRESET_AVC, 0x0F);
			snd_soc_update_bits(codec, COD3035X_50_DAC1,
					DAC1_INGN_MODE_MASK, DAC1_UHQA_MODE);
			snd_soc_update_bits(codec, COD3035X_53_UHQA,
					UHQA_MODE_MASK, UHQA_ENABLE);
			snd_soc_write(codec, COD3035X_7A_MAN_GN1, 0x2F);
			snd_soc_write(codec, COD3035X_7B_MAN_GN2, 0xE6);
			snd_soc_update_bits(codec, COD3035X_D6_CTRL_IREF5,
					CTMF_DCT_CAP_MASK, CTMF_DTC_CAP_167KHZ << CTMF_DCT_CAP_SHIFT);
			snd_soc_write(codec, COD3035X_B8_AUTO_HP9, 0x55);
			snd_soc_write(codec, COD3035X_BB_AUTO_HP12, 0x66);

		} else if (cod3035x->aifrate == COD3035X_SAMPLE_RATE_192KHZ) {
			dev_dbg(codec->dev, "%s called. NOT UHQA Mode\n", __func__);
			snd_soc_write(codec, COD3035X_F2_PRESET_AVC, 0x03);
			snd_soc_write(codec, COD3035X_F3_PRESET_AVC, 0x1F);
			snd_soc_write(codec, COD3035X_F4_PRESET_AVC, 0x55);
			snd_soc_write(codec, COD3035X_5A_AVC7, 0x44);
			snd_soc_write(codec, COD3035X_F5_PRESET_AVC, 0x1A);
			snd_soc_write(codec, COD3035X_F6_PRESET_AVC, 0xC6);
			snd_soc_update_bits(codec, COD3035X_50_DAC1,
					DAC1_INGN_MODE_MASK, DAC1_NORMAL_MODE);
			snd_soc_update_bits(codec, COD3035X_53_UHQA,
					UHQA_MODE_MASK, UHQA_DISABLE);
			snd_soc_write(codec, COD3035X_7A_MAN_GN1, 0x27);
			snd_soc_write(codec, COD3035X_7B_MAN_GN2, 0xE7);
			snd_soc_update_bits(codec, COD3035X_D6_CTRL_IREF5,
					CTMF_DCT_CAP_MASK, CTMF_DTC_CAP_87KHZ << CTMF_DCT_CAP_SHIFT);
			snd_soc_write(codec, COD3035X_B8_AUTO_HP9, 0x11);
			snd_soc_write(codec, COD3035X_BB_AUTO_HP12, 0x33);
		}

		cod3035x->aifrate = cur_aifrate;
	}

	/*
	 * Codec supports only 24bits per sample, Mixer performs the required
	 * conversion to 24 bits. BFS is fixed at 64fs for mixer<->codec
	 * interface.
	 */


	return 0;
}

static void cod3035x_set_adc_gpio(struct cod3035x_priv *cod3035x, int val)
{
	if (gpio_is_valid(cod3035x->adc_pin))
		gpio_direction_output(cod3035x->adc_pin, val);

	dev_dbg(cod3035x->dev, "%s : adc gpio value: %d\n",
			__func__, gpio_get_value(cod3035x->adc_pin));
}

/**
 * To measure more accurate gdet adc value, call this function.
 *
 * Return value:
 * 0: gdet adc check error
 * 1: jack detection
 * 2: water detection
 * 3: idel status
 */
static int cod3035x_finish_chk_more(struct cod3035x_priv *cod3035x)
{
	struct cod3035x_jack_det *jackdet = &cod3035x->jack_det;
	int finish_chk = 0;
	int i;

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, false);
	snd_soc_write(cod3035x->codec, COD3035X_85_CTR_POP2, 0xC0);
	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, true);

	cod3035x_usleep(100);

	for (i = 0; i < COD3035X_GDET_FINISH_CHK_MORE_NO; i++) {
		/* read adc for water detection */
		if (cod3035x->use_det_gdet_adc_mode == 2)
			cod3035x_set_adc_gpio(cod3035x, 1);
		mdelay(50);
		if (cod3035x->use_det_gdet_adc_mode == 1)
			jackdet->gdet_adc_val = cod3035x_gdet_adc_get_value(cod3035x);
		else if (cod3035x->use_det_gdet_adc_mode == 2)
			jackdet->gdet_adc_val = cod3035x_adc_get_value(cod3035x);

		if (jackdet->gdet_adc_val < cod3035x->water_threshold_adc_min)
			finish_chk = 1;
		else if (jackdet->gdet_adc_val >= cod3035x->water_threshold_adc_min &&
				jackdet->gdet_adc_val < cod3035x->water_threshold_adc_max)
			finish_chk = 2;
		else if (jackdet->gdet_adc_val >= cod3035x->water_threshold_adc_max)
			finish_chk = 3;

		dev_dbg(cod3035x->dev, "%s called. gdet_adc: %d, finish_chk: %d\n",
				__func__, jackdet->gdet_adc_val, finish_chk);
	}

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, false);
	snd_soc_write(cod3035x->codec, COD3035X_85_CTR_POP2, 0x50);
	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, true);

	return finish_chk;
}

static void cod3035x_gdet_adc_work(struct work_struct *work)
{
	struct cod3035x_priv *cod3035x =
		container_of(work, struct cod3035x_priv, gdet_adc_work);
	struct snd_soc_codec *codec = cod3035x->codec;
	struct cod3035x_jack_det *jackdet = &cod3035x->jack_det;

	cancel_work_sync(&cod3035x->jack_det_work);

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, false);

	snd_soc_write(codec, COD3035X_85_CTR_POP2, 0xC0);

	/* read adc for water detection */
	if (cod3035x->use_det_gdet_adc_mode == 1)
		jackdet->gdet_adc_val = cod3035x_gdet_adc_get_value(cod3035x);
	else if (cod3035x->use_det_gdet_adc_mode == 2) {
		cod3035x_set_adc_gpio(cod3035x, 1);
		jackdet->gdet_adc_val = cod3035x_adc_get_value(cod3035x);
	}

	dev_dbg(cod3035x->dev, "%s called. gdet adc: %d\n", __func__,
			jackdet->gdet_adc_val);
	snd_soc_write(codec, COD3035X_85_CTR_POP2, 0x50);

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, true);

	if (jackdet->gdet_adc_val < cod3035x->water_threshold_adc_min) {
		/* Jack detection process */
		dev_dbg(cod3035x->dev, "%s jack is detected.\n", __func__);
		jackdet->jack_det = true;

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		/* IRQ masking off */
		snd_soc_write(cod3035x->codec, COD3035X_05_IRQ1M, 0x00);
		snd_soc_write(cod3035x->codec, COD3035X_06_IRQ2M, 0x00);
		/* Jack in */
		snd_soc_write(cod3035x->codec, COD3035X_93_CTR_DLY6, 0x20);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		cancel_work_sync(&cod3035x->jack_det_work);
		queue_work(cod3035x->jack_det_wq, &cod3035x->jack_det_work);
	} else if (jackdet->gdet_adc_val >= cod3035x->water_threshold_adc_min &&
			jackdet->gdet_adc_val < cod3035x->water_threshold_adc_max) {
		/* Water detection process */
		dev_dbg(cod3035x->dev, "%s water is detected.\n", __func__);
		jackdet->water_det = true;

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		/* IRQ masking on */
		snd_soc_write(codec, COD3035X_05_IRQ1M, 0x01);
		snd_soc_write(codec, COD3035X_06_IRQ2M, 0x01);
		/* Water polling */
		snd_soc_write(cod3035x->codec, COD3035X_93_CTR_DLY6, 0x40);
		/* Set gdet pop resistance */
		snd_soc_write(codec, COD3035X_85_CTR_POP2, 0xC0);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);
	} else {
		/* Water finish check process */
		if (cod3035x_finish_chk_more(cod3035x) == 3) {
			dev_dbg(cod3035x->dev, "%s water is finished. water: %d, jack:%d\n",
					__func__, jackdet->water_det, jackdet->jack_det);

			if (cod3035x->is_suspend)
				regcache_cache_only(cod3035x->regmap, false);

			/* IRQ masking off */
			snd_soc_write(cod3035x->codec, COD3035X_05_IRQ1M, 0x00);
			snd_soc_write(cod3035x->codec, COD3035X_06_IRQ2M, 0x00);
			/* Water out */
			snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x10);

			if (jackdet->jack_det == true) {
				snd_soc_write(codec, COD3035X_99_TEST2, 0x40);
				snd_soc_write(codec, COD3035X_99_TEST2, 0x00);
			}
			snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);

			jackdet->water_det = false;
			jackdet->jack_det = false;

			if (cod3035x->is_suspend)
				regcache_cache_only(cod3035x->regmap, true);
		} else
			queue_work(cod3035x->gdet_adc_wq, &cod3035x->gdet_adc_work);
	}
}

static void cod3035x_jack_imp_cal(struct cod3035x_priv *cod3035x,
		int param1, int param2, int param3, int param4, int param5)
{
	struct snd_soc_codec *codec = cod3035x->codec;
	unsigned long imp1, imp2;
	unsigned long p1, p2, p3, dout;
	long temp1, temp2, temp3, temp4;
	int impedance;

	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, false);

	imp1 = snd_soc_read(codec, COD3035X_AB_TEST_ADC1);
	imp2 = snd_soc_read(codec, COD3035X_AE_TEST_ADC4);

	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, true);

	dev_dbg(cod3035x->dev,
			"%s called. 0x7B: %02x, 0x7C:%02x, 0x7E:%02x, 0x7F:%02x, 0x80:%02x\n",
			__func__, param1, param2, param3, param4, param5);
	dev_dbg(cod3035x->dev,
			"%s called. imp1: %02lx, imp2:%02lx\n", __func__, imp1, imp2);

	p1 = param3;

	p2 = param1 & COD3035X_PARAM_1_MASK;
	p2 = p2 << 3;
	p2 |= param4;

	p3 = param2 & COD3035X_PARAM_2_MASK;
	p3 = p3 << 3;
	p3 |= param5;

	/* 2's complement check */
	if (p3 & COD3035X_P3_BIT_CHECK)
		p3 = p3 - 1024;
	/* p3 parameter correciton */
	p3 = p3 - 15;

	dout = imp2 & GPADC_IMP_DATA_SMP_MASK;
	dout = dout << 4;
	dout |= imp1;

	dev_dbg(cod3035x->dev, "%s called. p1:%ld, p2:%ld, p3:%ld, dout:%ld\n",
			__func__, p1, p2, p3, dout);

	/* calculate impedance check */
	temp1 = p1 * (COD3035X_IMP_C3 * p1 + 1024);
	temp2 = (COD3035X_IMP_C2 * dout) - (COD3035X_IMP_C2 * p3) -
		(COD3035X_IMP_C1 * p2) - (COD3035X_IMP_C2 * p2);
	temp3 = p2 * (COD3035X_IMP_C1 * p1 + COD3035X_IMP_C2 * p1 +
			COD3035X_IMP_C3 * p1);
	temp4 = (dout - p3) * (COD3035X_IMP_C3 * p1 + COD3035X_IMP_C2 * p1);
	impedance = ((temp1 * temp2) / (temp3 - temp4)) / 128;

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, false);

	impedance = impedance - cod3035x->jack_imp_gap;
	/* Set register according to impedance */
	if (impedance < 0) {
		dev_dbg(cod3035x->dev, "%s called. jack impedance is high: %d\n",
				__func__, impedance);
		snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x72);
	} else if (impedance < COD3035X_IMP_LOW) {
		dev_dbg(cod3035x->dev, "%s called. jack impedance is low: %d\n",
				__func__, impedance);
		snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x52);
	} else if (impedance < COD3035X_IMP_HIGH) {
		dev_dbg(cod3035x->dev, "%s called. jack impedance is mid: %d\n",
				__func__, impedance);
		snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x62);
	} else {
		dev_dbg(cod3035x->dev, "%s called. jack impedance is high: %d\n",
				__func__, impedance);
		snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x72);
	}

	if (cod3035x->is_suspend)
		regcache_cache_only(cod3035x->regmap, true);
}

static void cod3035x_jack_det_work(struct work_struct *work)
{
	struct cod3035x_priv *cod3035x =
		container_of(work, struct cod3035x_priv, jack_det_work);
	struct cod3035x_jack_det *jackdet = &cod3035x->jack_det;
	struct snd_soc_codec *codec = cod3035x->codec;
	int jack_det_status, adc;

	dev_dbg(cod3035x->dev, "%s(%d) jackdet: %d\n",
			__func__, __LINE__, jackdet->jack_det);
	mutex_lock(&cod3035x->jackdet_lock);

	if (jackdet->jack_det == true) {
		/* set delay for read correct adc value */
		msleep(cod3035x->mic_det_delay);

		/* check jack det status */
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);
		jack_det_status = snd_soc_read(codec, COD3035X_09_STATUS1);
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		jack_det_status = jack_det_status & STATUS1_JACK_DET_MASK;

		if (jack_det_status == false) {
			dev_dbg(cod3035x->dev, "%s fake jack inserted.\n", __func__);
			queue_work(cod3035x->gdet_adc_wq, &cod3035x->gdet_adc_work);
			mutex_unlock(&cod3035x->jackdet_lock);
			return;
		}

		/* read adc for mic detect */
		adc = cod3035x_adc_get_value(cod3035x);

		dev_dbg(cod3035x->dev, "%s mic det adc: %d\n", __func__, adc);

		if (adc > cod3035x->mic_adc_range)
			jackdet->mic_det = true;
		else
			jackdet->mic_det = false;

		dev_dbg(cod3035x->dev, "%s Mic det: %d\n", __func__, jackdet->mic_det);

		jackdet->adc_val = adc;
	} else {
		dev_dbg(cod3035x->dev, "%s JACK OUT\n", __func__);

		/* jack/mic out */
		jackdet->mic_det = false;
		jackdet->adc_val = -EINVAL;
	}

	/* Send the jack detect event to the audio framework */
	if (jackdet->jack_det && jackdet->mic_det)
		switch_set_state(&cod3035x->sdev, 1);	/* 4 Pole */
	else if (jackdet->jack_det)
		switch_set_state(&cod3035x->sdev, 2);	/* 3 Pole */
	else
		switch_set_state(&cod3035x->sdev, 0);

	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, false);

	if (jackdet->jack_det && jackdet->mic_det) {
		snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);
		snd_soc_write(codec, COD3035X_94_JACK_CTR, 0x30);
		dev_dbg(cod3035x->dev, "%s 4 Pole Jack-In\n", __func__);
	} else if (jackdet->jack_det) {
		snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);
		snd_soc_write(codec, COD3035X_94_JACK_CTR, 0x20);
		dev_dbg(cod3035x->dev, "%s 3 Pole Jack-In\n", __func__);
	} else {
		snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);
		snd_soc_write(codec, COD3035X_94_JACK_CTR, 0x00);
		dev_dbg(cod3035x->dev, "%s JACK OUT\n", __func__);
	}

	if (cod3035x->is_suspend)
	    regcache_cache_only(cod3035x->regmap, true);

	dev_dbg(cod3035x->codec->dev, "Jack %s, Mic %s\n",
			jackdet->jack_det ? "inserted" : "removed",
			jackdet->mic_det ? "inserted" : "removed");

	mutex_unlock(&cod3035x->jackdet_lock);
}

#define ADC_TRACE_NUM		5
#define ADC_TRACE_NUM2		2
#define ADC_READ_DELAY_US	500
#define ADC_READ_DELAY_MS	1
#define ADC_DEVI_THRESHOLD	18000

#define BUTTON_PRESS 1
#define BUTTON_RELEASE 0

static int get_adc_avg(int *adc_values)
{
	int i;
	int adc_sum = 0;

	for (i = 0; i < ADC_TRACE_NUM; i++)
		adc_sum += adc_values[i];

	adc_sum = adc_sum / ADC_TRACE_NUM;
	return adc_sum;
}

static int get_adc_devi(int avg, int *adc_values)
{
	int i;
	int devi = 0, diff;

	for (i = 0; i < ADC_TRACE_NUM; i++) {
		diff = adc_values[i]-avg;
		devi += (diff*diff);
	}
	return devi;
}

static void cod3035x_buttons_work(struct work_struct *work)
{
	struct cod3035x_priv *cod3035x =
		container_of(work, struct cod3035x_priv, buttons_work);
	struct cod3035x_jack_det *jd = &cod3035x->jack_det;
	struct jack_buttons_zone *btn_zones = cod3035x->jack_buttons_zones;
	int num_buttons_zones = ARRAY_SIZE(cod3035x->jack_buttons_zones);
	int adc_values[ADC_TRACE_NUM];
	int current_button_state;
	int adc;
	int i, avg, devi;
	int adc_final_values[ADC_TRACE_NUM2];
	int j;
	int adc_final = 0;
	int adc_max = 0;

	if (!jd->jack_det) {
		dev_err(cod3035x->dev, "Skip button events for jack_out\n");
		if (jd->privious_button_state == BUTTON_PRESS) {
			jd->button_det = false;
			input_report_key(cod3035x->input, jd->button_code, 0);
			input_sync(cod3035x->input);
			cod3035x_process_button_ev(cod3035x->codec, jd->button_code, 0);
			dev_dbg(cod3035x->dev, ":key %d released when jack_out\n", jd->button_code);
		}
		return;
	}

	if (!jd->mic_det) {
		dev_err(cod3035x->dev, "Skip button events for 3-pole jack\n");
		return;
	}

	/* set delay for read correct adc value */
	mdelay(10);

	for (j = 0; j < ADC_TRACE_NUM2; j++) {
		/* read GPADC for button */
		for (i = 0; i < ADC_TRACE_NUM; i++) {
			adc = cod3035x_adc_get_value(cod3035x);
			adc_values[i] = adc;
			udelay(ADC_READ_DELAY_US);
		}

		/*
		 * check avg/devi value is proper
		 * if not read adc after 5 ms
		 */
		avg = get_adc_avg(adc_values);
		devi = get_adc_devi(avg, adc_values);
		dev_dbg(cod3035x->dev,
				":button adc avg: %d, devi: %d\n", avg, devi);

		if (devi > ADC_DEVI_THRESHOLD) {
			queue_work(cod3035x->buttons_wq,
					&cod3035x->buttons_work);

			for (i = 0; i < ADC_TRACE_NUM;) {
				dev_err(cod3035x->dev,
						":retry button_work :  %d %d %d %d %d\n",
						adc_values[i + 0],
						adc_values[i + 1],
						adc_values[i + 2],
						adc_values[i + 3],
						adc_values[i + 4]);

				i += 5;
			}
			return;
		}
		adc_final_values[j] = avg;

		if (avg > adc_max)
			adc_max = avg;
		mdelay(ADC_READ_DELAY_MS);
	}
	adc_final = adc_max;

	/* check button press/release */
	if (adc_final > cod3035x->btn_release_value) {
		dev_dbg(cod3035x->dev,
				"Button Released! adc_fanal: %d, btn_value: %d\n",
				adc_final,
				cod3035x->btn_release_value);

		current_button_state = BUTTON_RELEASE;
	} else {
		dev_dbg(cod3035x->dev,
				"Button Pressed! adc_final: %d, btn_value: %d\n",
				adc_final,
				cod3035x->btn_release_value);

		current_button_state = BUTTON_PRESS;
	}

	if (jd->privious_button_state == current_button_state) {
		dev_dbg(cod3035x->dev, "status are same\n");
		return;
	}

	jd->privious_button_state = current_button_state;

	adc = adc_final;
	jd->adc_val = adc_final;

	for (i = 0; i < 4; i++)
		dev_dbg(cod3035x->dev,
				"[DEBUG]: adc(%d), buttons: code(%d), low(%d), high(%d)\n",
				adc,
				cod3035x->jack_buttons_zones[i].code,
				cod3035x->jack_buttons_zones[i].adc_low,
				cod3035x->jack_buttons_zones[i].adc_high);

	/* determine which button press or release */
	if (current_button_state == BUTTON_PRESS) {
		for (i = 0; i < num_buttons_zones; i++)
			if (adc >= btn_zones[i].adc_low &&
					adc <= btn_zones[i].adc_high) {
				jd->button_code = btn_zones[i].code;
				input_report_key(cod3035x->input,
						jd->button_code,
						1);

				input_sync(cod3035x->input);
				jd->button_det = true;
				cod3035x_process_button_ev(cod3035x->codec,
						jd->button_code,
						1);

				dev_dbg(cod3035x->dev, ":key %d is pressed, adc %d\n",
						btn_zones[i].code, adc);
				return;
			}

		dev_dbg(cod3035x->dev, ":key skipped. ADC %d\n", adc);
	} else {
		snd_soc_update_bits(cod3035x->codec, COD3035X_12_PD_AD2,
				PDB_MIC_BST3_MASK, 0);
		msleep(40);
		snd_soc_update_bits(cod3035x->codec, COD3035X_12_PD_AD2,
				PDB_MIC_BST3_MASK, PDB_MIC_BST3_MASK);

		jd->button_det = false;
		input_report_key(cod3035x->input, jd->button_code, 0);
		input_sync(cod3035x->input);
		cod3035x_process_button_ev(cod3035x->codec,
				jd->button_code,
				0);

		dev_dbg(cod3035x->dev, ":key %d released\n", jd->button_code);
	}

	return;
}


int cod3035x_jack_mic_register(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);
	int i, ret;

	cod3035x->sdev.name = "h2w";

	ret = switch_dev_register(&cod3035x->sdev);
	if (ret < 0)
		dev_err(codec->dev, "Switch registration failed\n");

	cod3035x->input = devm_input_allocate_device(codec->dev);
	if (!cod3035x->input) {
		dev_err(codec->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	/* Not handling Headset events for now.Headset event handling
	 * registered as Input device, causing some conflict with Keyboard Input
	 * device.So, temporarily not handling Headset event, it will be enabled
	 * after proper fix.
	 */
	cod3035x->input->name = "Codec3035 Headset Events";
	cod3035x->input->phys = dev_name(codec->dev);
	cod3035x->input->id.bustype = BUS_I2C;

	cod3035x->input->evbit[0] = BIT_MASK(EV_KEY);
	for (i = 0; i < 4; i++)
		set_bit(cod3035x->jack_buttons_zones[i].code, cod3035x->input->keybit);
	cod3035x->input->dev.parent = codec->dev;
	input_set_drvdata(cod3035x->input, codec);

	ret = input_register_device(cod3035x->input);
	if (ret != 0) {
		cod3035x->input = NULL;
		dev_err(codec->dev, "Failed to register 3035 input device\n");
	}

#ifdef CONFIG_PM
	pm_runtime_get_sync(codec->dev);
#else
	cod3035x_enable(codec->dev);
#endif

#ifdef CONFIG_PM
	pm_runtime_put_sync(codec->dev);
#else
	cod3035x_disable(codec->dev);
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(cod3035x_jack_mic_register);

static const struct snd_soc_dai_ops cod3035x_dai_ops = {
	.set_fmt = cod3035x_dai_set_fmt,
	.startup = cod3035x_dai_startup,
	.shutdown = cod3035x_dai_shutdown,
	.hw_params = cod3035x_dai_hw_params,
};

#define COD3035X_RATES		SNDRV_PCM_RATE_8000_192000

#define COD3035X_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |		\
		SNDRV_PCM_FMTBIT_S20_3LE |	\
		SNDRV_PCM_FMTBIT_S24_LE |	\
		SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver cod3035x_dai[] = {
	{
		.name = "cod3035x-aif",
		.id = 1,
		.playback = {
			.stream_name = "AIF Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = COD3035X_RATES,
			.formats = COD3035X_FORMATS,
		},
		.capture = {
			.stream_name = "AIF Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = COD3035X_RATES,
			.formats = COD3035X_FORMATS,
		},
		.ops = &cod3035x_dai_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "cod3035x-aif2",
		.id = 2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = COD3035X_RATES,
			.formats = COD3035X_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = COD3035X_RATES,
			.formats = COD3035X_FORMATS,
		},
		.ops = &cod3035x_dai_ops,
		.symmetric_rates = 1,
	},
};

static int cod3035x_regulators_enable(struct snd_soc_codec *codec)
{
	int ret;
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	cod3035x->regulator_count++;
	ret = regulator_enable(cod3035x->vdd);
	ret = regulator_enable(cod3035x->vdd2);
	dev_dbg(codec->dev, "%s regulator: %d\n",
			__func__, cod3035x->regulator_count);

	return ret;
}

static void cod3035x_regulators_disable(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	cod3035x->regulator_count--;
	regulator_disable(cod3035x->vdd);
	regulator_disable(cod3035x->vdd2);
	dev_dbg(codec->dev, "%s regulator: %d\n",
			__func__, cod3035x->regulator_count);
}

static void cod3035x_reset_io_selector_bits(struct snd_soc_codec *codec)
{
	/* Reset output selector bits */
	snd_soc_update_bits(codec, COD3035X_77_CHOP_DA,
			EN_HP_CHOP_MASK | EN_EP_CHOP_MASK, 0x0);
}

/*
 * Configure the mic1 and mic2 bias voltages with default value or the value
 * received from the device tree.
 * Also configure the internal LDO voltage.
 */
static void cod3035x_configure_mic_bias(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	/* Configure Mic1 Bias Voltage */
	snd_soc_update_bits(codec, COD3035X_19_CTRL_REF,
			CTRV_MCB1_MASK,
			(cod3035x->mic_bias1_voltage << CTRV_MCB1_SHIFT));

	/* Configure Mic2 Bias Voltage */
	snd_soc_update_bits(codec, COD3035X_83_CTR_MCB,
			CTRV_MCB2_MASK,
			(cod3035x->mic_bias2_voltage << CTRV_MCB2_SHIFT));

	/* Configure Mic Bias LDO Voltage */
	snd_soc_update_bits(codec, COD3035X_83_CTR_MCB,
			CTRV_MCB_LDO_MASK,
			(cod3035x->mic_bias_ldo_voltage << CTRV_MCB_LDO_SHIFT));
}

/**
 * cod3035x_codec_initialize : To be called if f/w update fails
 *
 * In case the firmware is not present or corrupt, we should still be able to
 * run the codec with decent parameters. This values are updated as per the
 * latest stable firmware.
 *
 * The values provided in this function are hard-coded register values, and we
 * need not update these values as per bit-fields.
 */
static void cod3035x_codec_initialize(void *context)
{
	struct snd_soc_codec *codec = (struct snd_soc_codec *)context;

	dev_dbg(codec->dev, "%s called, setting defaults\n", __func__);

#ifdef CONFIG_PM
	pm_runtime_get_sync(codec->dev);
#else
	cod3035x_enable(codec->dev);
#endif

	snd_soc_write(codec, COD3035X_05_IRQ1M, 0x00);
	snd_soc_write(codec, COD3035X_06_IRQ2M, 0x00);
	snd_soc_write(codec, COD3035X_07_IRQ3M, 0x53);
	snd_soc_write(codec, COD3035X_08_IRQ4M, 0x53);
	snd_soc_write(codec, COD3035X_84_CTR_POP1, 0xBE);
	snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x42);
	snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);
	snd_soc_write(codec, COD3035X_94_JACK_CTR, 0x00);
	snd_soc_write(codec, COD3035X_8E_CTR_DLY1, 0xC0);
	snd_soc_write(codec, COD3035X_8F_CTR_DLY2, 0xC0);
	snd_soc_write(codec, COD3035X_8D_CTR_DBNC3, 0xA1);
	snd_soc_write(codec, COD3035X_3D_OVP_1, 0xF5);
	snd_soc_write(codec, COD3035X_9A_TEST3, 0x01);
	snd_soc_write(codec, COD3035X_44_IF1_FORMAT4, 0xFF);

	/* HP amplifier reference current to 8uA*/
	snd_soc_write(codec, COD3035X_B0_AUTO_HP1, 0xA0);
	snd_soc_write(codec, COD3035X_B4_AUTO_HP5, 0x01);
	snd_soc_write(codec, COD3035X_B5_AUTO_HP6, 0xFC);
	snd_soc_write(codec, COD3035X_B6_AUTO_HP7, 0xFF);
	snd_soc_write(codec, COD3035X_B7_AUTO_HP8, 0xB7);
	snd_soc_write(codec, COD3035X_B8_AUTO_HP9, 0x55);
	snd_soc_write(codec, COD3035X_BA_AUTO_HP11, 0x02);
	snd_soc_write(codec, COD3035X_BB_AUTO_HP12, 0x66);
	snd_soc_write(codec, COD3035X_BE_ODSEL2, 0x60);

	/* Disable AVC mute function */
	snd_soc_write(codec, COD3035X_54_AVC1, 0x8F);
	snd_soc_write(codec, COD3035X_55_AVC2, 0x78);

	/* Disable DRC function */
	snd_soc_write(codec, COD3035X_57_AVC4, 0x00);

	/* Preset register for AVC */
	snd_soc_write(codec, COD3035X_E1_PRESET_AVC, 0xF1);
	snd_soc_write(codec, COD3035X_E3_PRESET_AVC, 0x00);
	snd_soc_write(codec, COD3035X_E5_PRESET_AVC, 0x00);
	snd_soc_write(codec, COD3035X_E7_PRESET_AVC, 0x00);
	snd_soc_write(codec, COD3035X_E8_PRESET_AVC, 0x00);
	snd_soc_write(codec, COD3035X_E9_PRESET_AVC, 0x00);
	snd_soc_write(codec, COD3035X_EA_PRESET_AVC, 0x01);
	snd_soc_write(codec, COD3035X_F2_PRESET_AVC, 0x03);
	snd_soc_write(codec, COD3035X_F3_PRESET_AVC, 0x1F);
	snd_soc_write(codec, COD3035X_F4_PRESET_AVC, 0x55);
	snd_soc_write(codec, COD3035X_5A_AVC7, 0x44);
	snd_soc_write(codec, COD3035X_F5_PRESET_AVC, 0x1A);
	snd_soc_write(codec, COD3035X_F6_PRESET_AVC, 0xC6);
	snd_soc_write(codec, COD3035X_1B_SDM_STR, 0x01);

	snd_soc_write(codec, COD3035X_80_PDB_ACC1, 0x02);
	msleep(100);
	snd_soc_write(codec, COD3035X_80_PDB_ACC1, 0x03);

	/* MIC Boost LPF On */
	snd_soc_write(codec, COD3035X_7C_LPF_AD, 0x1F);

	/* Default value, enabling HPF and setting freq at 100Hz */
	snd_soc_write(codec, COD3035X_46_ADC1, 0x0c);
	snd_soc_write(codec, COD3035X_4B_ADC2, 0x0c);

	snd_soc_update_bits(codec, COD3035X_D0_CTRL_IREF1,
			CTMI_VCM_MASK | CTMI_MIX_MASK,
			(CTMI_VCM_4U << CTMI_VCM_SHIFT) | CTMI_MIX_2U);

	snd_soc_update_bits(codec, COD3035X_D1_CTRL_IREF2,
			CTMI_INT1_MASK, CTMI_INT1_4U);

	snd_soc_update_bits(codec, COD3035X_D2_CTRL_IREF3,
			CTMI_MIC2_MASK | CTMI_MIC1_MASK,
			(CTMI_MIC2_2U << CTMI_MIC2_SHIFT) | CTMI_MIC1_2U);

	snd_soc_update_bits(codec, COD3035X_D3_CTRL_IREF4,
			CTMI_MIC_BUFF_MASK | CTMI_MIC3_MASK,
			(CTMI_MIC_BUFF_2U << CTMI_MIC_BUFF_SHIFT) | CTMI_MIC3_2U);

	snd_soc_write(codec, COD3035X_47_AVOLL1, 0x18);
	snd_soc_write(codec, COD3035X_48_AVOLR1, 0x18);
	/* Boost 20 dB, Gain 0 dB for MIC1/MIC2/MIC3 */
	snd_soc_write(codec, COD3035X_20_VOL_AD1, 0x54);
	snd_soc_write(codec, COD3035X_21_VOL_AD2, 0x54);
	snd_soc_write(codec, COD3035X_22_VOL_AD3, 0x54);
	snd_soc_write(codec, COD3035X_22_VOL_AD3, 0x54);

	/* default HP Volume setting */
	snd_soc_write(codec, COD3035X_30_VOL_HPL, 0x0A);
	snd_soc_write(codec, COD3035X_31_VOL_HPR, 0x0A);
	snd_soc_write(codec, COD3035X_51_DVOLL, 0x54);
	snd_soc_write(codec, COD3035X_52_DVOLR, 0x54);
	snd_soc_write(codec, COD3035X_10_PD_REF, 0x10);

	/* Change receiver REF current */
	snd_soc_update_bits(codec, COD3035X_DC_CTRL_EPS,
			CTMI_EP_A_MASK, (CTMI_EP_A_4_UA << CTMI_EP_A_SHIFT));

	/* Configure mic bias voltage */
	cod3035x_configure_mic_bias(codec);

	/* All boot time hardware access is done. Put the device to sleep. */
#ifdef CONFIG_PM
	pm_runtime_put_sync(codec->dev);
#else
	cod3035x_disable(codec->dev);
#endif
}

/**
 * cod3035x_post_fw_update_success: To be called after f/w update
 *
 * The firmware may be enabling some of the path and power registers which are
 * used during path enablement. We need to keep the values of these registers
 * consistent so that the functionality of the codec driver doesn't change
 * because of the firmware.
 */

static void cod3035x_regmap_sync(struct device *dev)
{
	struct cod3035x_priv *cod3035x = dev_get_drvdata(dev);
	unsigned char reg[COD3035X_MAX_REGISTER] = {0,};
	int i;

	/* Read from Cache */
	for (i = 0; i < COD3035X_REGCACHE_SYNC_END_REG; i++)
		if (cod3035x_readable_register(dev, i) &&
				(!cod3035x_volatile_register(dev, i)))
			reg[i] = (unsigned char)
				snd_soc_read(cod3035x->codec, i);

	regcache_cache_bypass(cod3035x->regmap, true);

	snd_soc_write(cod3035x->codec, COD3035X_40_DIGITAL_POWER,
			reg[COD3035X_40_DIGITAL_POWER]);

	/* Update HW */
	for (i = 0; i < COD3035X_REGCACHE_SYNC_END_REG ; i++)
		if (cod3035x_writeable_register(dev, i) &&
				(!cod3035x_volatile_register(dev, i)))
			snd_soc_write(cod3035x->codec, i, reg[i]);

	regcache_cache_bypass(cod3035x->regmap, false);
}

static void cod3035x_reg_restore(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, COD3035X_80_PDB_ACC1,
			PDB_JD_CLK_EN_MASK, PDB_JD_CLK_EN_MASK);

	/* Give 15ms delay before storing the otp values */
	usleep_range(15000, 15000 + 1000);

	if (!cod3035x->is_probe_done) {
		cod3035x_regmap_sync(codec->dev);
		cod3035x_reset_io_selector_bits(codec);
	} else {
		cod3035x_regmap_sync(codec->dev);
	}
}

static void cod3035x_i2c_parse_dt(struct cod3035x_priv *cod3035x)
{
	/* todo .. Need to add DT parsing for 3035 */
	struct device *dev = cod3035x->dev;
	struct device_node *np = dev->of_node;
	unsigned int bias_v_conf;
	int mic_range, mic_delay, btn_rel_val;
	int water_finish_chk_min, water_threshold_min, water_threshold_max;
	int gdet_mode, ret;
	struct of_phandle_args args;
	int jack_imp_tuning;
	int i = 0;

	cod3035x->int_gpio = of_get_gpio(np, 0);

	if (cod3035x->int_gpio < 0)
		dev_err(dev, "(*)Error in getting Codec-3035 Interrupt gpio\n");

	/* Default Bias Voltages */
	cod3035x->mic_bias1_voltage = MIC_BIAS1_VO_3_0V;
	cod3035x->mic_bias2_voltage = MIC_BIAS2_VO_3_0V;
	cod3035x->mic_bias_ldo_voltage = MIC_BIAS_LDO_VO_3_3V;

	ret = of_property_read_u32(dev->of_node,
			"mic-bias1-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS1_VO_2_8V) &&
				(bias_v_conf <= MIC_BIAS1_VO_3_0V)))
		cod3035x->mic_bias1_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias1-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	ret = of_property_read_u32(dev->of_node,
			"mic-bias2-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS2_VO_2_8V) &&
				(bias_v_conf <= MIC_BIAS2_VO_3_0V)))
		cod3035x->mic_bias2_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias2-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	ret = of_property_read_u32(dev->of_node, "mic-adc-range", &mic_range);
	if (!ret)
		cod3035x->mic_adc_range = mic_range;
	else
		cod3035x->mic_adc_range = 1120;

	ret = of_property_read_u32(dev->of_node, "mic-det-delay", &mic_delay);
	if (!ret)
		cod3035x->mic_det_delay = mic_delay;
	else
		cod3035x->mic_det_delay = COD3035X_MIC_DET_DELAY;

	ret = of_property_read_u32(dev->of_node,
			"btn-release-value", &btn_rel_val);
	if (!ret)
		cod3035x->btn_release_value = btn_rel_val;
	else
		cod3035x->btn_release_value = 1100;

	ret = of_property_read_u32(dev->of_node, "water-threshold-min",
			&water_threshold_min);
	if (!ret)
		cod3035x->water_threshold_adc_min = water_threshold_min;
	else
		cod3035x->water_threshold_adc_min = COD3035X_WATER_DET_THRESHOLD_MIN;

	ret = of_property_read_u32(dev->of_node, "water-finish-chk-min",
			&water_finish_chk_min);
	if (!ret)
		cod3035x->water_finish_chk_adc_min = water_finish_chk_min;
	else
		cod3035x->water_finish_chk_adc_min = COD3035X_GDET_FINISH_CHK_MIN;

	ret = of_property_read_u32(dev->of_node, "water-threshold-max",
			&water_threshold_max);
	if (!ret)
		cod3035x->water_threshold_adc_max = water_threshold_max;
	else
		cod3035x->water_threshold_adc_max = COD3035X_WATER_DET_THRESHOLD_MAX;

	ret = of_property_read_u32(dev->of_node, "jack-imp-tuning", &jack_imp_tuning);
	if (!ret)
		cod3035x->jack_imp_gap = jack_imp_tuning;
	else
		cod3035x->jack_imp_gap = 0;

	ret = of_property_read_u32(dev->of_node,
			"mic-bias-ldo-voltage", &bias_v_conf);
	if ((!ret) && ((bias_v_conf >= MIC_BIAS_LDO_VO_2_8V) &&
				(bias_v_conf <= MIC_BIAS_LDO_VO_3_3V)))
		cod3035x->mic_bias_ldo_voltage = bias_v_conf;
	else
		dev_dbg(dev, "Property 'mic-bias-ldo-voltage' %s",
				ret ? "not found" : "invalid value (use:1-3)");

	dev_dbg(dev, "Bias voltage values: bias1 = %d, bias2= %d, ldo = %d\n",
			cod3035x->mic_bias1_voltage,
			cod3035x->mic_bias2_voltage,
			cod3035x->mic_bias_ldo_voltage);

	if (of_find_property(dev->of_node,
				"update-firmware", NULL))
		cod3035x->update_fw = true;
	else
		cod3035x->update_fw = false;

	if (of_find_property(dev->of_node, "use-btn-adc-mode", NULL) != NULL)
		cod3035x->use_btn_adc_mode = true;

	if (of_find_property(dev->of_node, "use-det-gdet-adc-mode", NULL) != NULL) {
		cod3035x->use_det_gdet_adc_mode = true;
		ret = of_property_read_u32(dev->of_node, "use-det-gdet-adc-mode", &gdet_mode);
		if (!ret)
			cod3035x->use_det_gdet_adc_mode = gdet_mode;
		else
			cod3035x->use_det_gdet_adc_mode = false;
		dev_dbg(dev, "%s : cod3035x->use_det_gdet_adc_mode: %d\n",
				__func__, cod3035x->use_det_gdet_adc_mode);
	}

	dev_err(dev, "Using %s for button detection\n",
			cod3035x->use_btn_adc_mode ? "GPADC" : "internal h/w");

	if (cod3035x->use_btn_adc_mode) {
		/* Parsing but-zones, a maximum of 4 buttons are supported */
		for (i = 0; i < 4; i++) {
			if (of_parse_phandle_with_args(dev->of_node,
						"but-zones-list",
						"#list-but-cells", i, &args))
				break;

			cod3035x->jack_buttons_zones[i].code = args.args[0];
			cod3035x->jack_buttons_zones[i].adc_low = args.args[1];
			cod3035x->jack_buttons_zones[i].adc_high = args.args[2];
		}
		/* initialize button status */
		cod3035x->jack_det.privious_button_state = BUTTON_RELEASE;

		for (i = 0; i < 4; i++)
			dev_err(dev, "[DEBUG]: buttons: code(%d), low(%d), high(%d)\n",
					cod3035x->jack_buttons_zones[i].code,
					cod3035x->jack_buttons_zones[i].adc_low,
					cod3035x->jack_buttons_zones[i].adc_high);
	}
}

struct codec_notifier_struct {
	struct cod3035x_priv *cod3035x;
};
static struct codec_notifier_struct codec_notifier_t;

static int cod3035x_notifier_handler(struct notifier_block *nb,
		unsigned long insert,
		void *data)
{
	struct codec_notifier_struct *codec_notifier_data = data;
	struct cod3035x_priv *cod3035x = codec_notifier_data->cod3035x;
	struct snd_soc_codec *codec = cod3035x->codec;
	struct cod3035x_jack_det *jackdet = &cod3035x->jack_det;
	unsigned int stat1, pend1, pend2, pend3, pend4;
	unsigned int param1, param2, param3, param4, param5;
	unsigned int temp1, temp2;

	mutex_lock(&cod3035x->key_lock);

	pend1 = cod3035x->irq_val[0];
	pend2 = cod3035x->irq_val[1];
	pend3 = cod3035x->irq_val[2];
	pend4 = cod3035x->irq_val[3];
	stat1 = cod3035x->irq_val[4];
	param1 = cod3035x->irq_val[5];
	param2 = cod3035x->irq_val[6];
	param3 = cod3035x->irq_val[7];
	param4 = cod3035x->irq_val[8];
	param5 = cod3035x->irq_val[9];

	dev_dbg(cod3035x->dev,
			"[DEBUG] %s , line %d 01: %02x, 02:%02x, 03:%02x, 04:%02x, stat1:%02x\n",
			__func__, __LINE__, pend1, pend2, pend3, pend4, stat1);

	/* surge out */
	if(pend4 & IRQ4_OVPR_MONI_F || pend4 & IRQ4_OVPL_MONI_F) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s IRQ4_OVPLR_MONI_F, line %d\n", __func__, __LINE__);
		jackdet->surge_det = false;
		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	if (jackdet->surge_det) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s jackdet->surge_det, line %d\n", __func__, __LINE__);
		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* surge in */
	if(pend3 & IRQ3_OVPR_MONI_R || pend3 & IRQ3_OVPL_MONI_R) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s surge in, line %d\n", __func__, __LINE__);
		jackdet->surge_det = true;
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		/* Jack in */
		snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x20);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* AP Check state*/
	if ((pend1 & IRQ1_ST_APCHECK_R) || (pend4 & IRQ4_ST_MOIST_F)) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s AP Check state, line  %d\n", __func__,__LINE__);
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		temp1 = snd_soc_read(codec, 0xAF);
		temp2 = snd_soc_read(codec, 0xAD);
		dev_dbg(cod3035x->dev, "[DEBUG] %s 0xAF:%02x, 0xAD:%02x, line %d\n", __func__, temp1, temp2, __LINE__);

		snd_soc_write(cod3035x->codec, COD3035X_93_CTR_DLY6, 0x00);
		snd_soc_write(cod3035x->codec, COD3035X_99_TEST2, 0x00);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		queue_work(cod3035x->gdet_adc_wq, &cod3035x->gdet_adc_work);
		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* Jack interrupt in water */
	if (pend1 & IRQ1_ST_WTJACKIN_R) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s Jack interrupt in water, line %d\n", __func__,__LINE__);
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		temp1 = snd_soc_read(codec, 0xAF);
		temp2 = snd_soc_read(codec, 0xAD);
		dev_dbg(cod3035x->dev, "[DEBUG] %s 0xAF:%02x, 0xAD:%02x, line %d\n", __func__, temp1, temp2, __LINE__);

		snd_soc_write(cod3035x->codec, COD3035X_93_CTR_DLY6, 0x00);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* Water out interrupt */
	if (pend1 & IRQ1_ST_WTJACKOUT_R) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s Water out interrupt, line %d\n", __func__,__LINE__);
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		temp1 = snd_soc_read(codec, 0xAF);
		temp2 = snd_soc_read(codec, 0xAD);
		dev_dbg(cod3035x->dev, "[DEBUG] %s 0xAF:%02x, 0xAD:%02x, line %d\n", __func__, temp1, temp2, __LINE__);

		snd_soc_write(cod3035x->codec, COD3035X_93_CTR_DLY6, 0x00);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* Jack out interrupt */
	if (pend1 & IRQ1_ST_JACKOUT_R) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s Jack out interrupt, line %d\n", __func__,__LINE__);
		jackdet->jack_det = false;
		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, false);

		/* IRQ masking off */
		snd_soc_write(codec, COD3035X_05_IRQ1M, 0x00);
		snd_soc_write(codec, COD3035X_06_IRQ2M, 0x00);
		/* Jack out */
		snd_soc_write(codec, COD3035X_93_CTR_DLY6, 0x00);
		/* Impedance value reset */
		snd_soc_write(codec, COD3035X_A6_IMP_CNT11, 0x42);

		if (cod3035x->is_suspend)
			regcache_cache_only(cod3035x->regmap, true);

		cancel_work_sync(&cod3035x->jack_det_work);
		queue_work(cod3035x->jack_det_wq, &cod3035x->jack_det_work);

		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* Impedance check interrupt */
	if (pend3 & IRQ3_IMP_CHECK_DONE_R) {
		cod3035x_jack_imp_cal(cod3035x, param1, param2, param3, param4, param5);

		mutex_unlock(&cod3035x->key_lock);
		goto out;
	}

	/* Button interrupt */
	if ((pend1 & IRQ1_BTN_DET_R) || (pend2 & IRQ2_BTN_DET_F)) {
		dev_dbg(cod3035x->dev, "[DEBUG] %s Button interrupt, line %d\n", __func__,__LINE__);
		if (cod3035x->use_btn_adc_mode) {
			/* start button work */
			queue_work(cod3035x->buttons_wq, &cod3035x->buttons_work);
		} else {
			pr_err("[DEBUG] %s , line %d\n", __func__, __LINE__);
			/* need to implement button detection */
		}
	}

	mutex_unlock(&cod3035x->key_lock);

out:

	return IRQ_HANDLED;
}
static BLOCKING_NOTIFIER_HEAD(cod3035x_notifier);

int cod3035x_register_notifier(struct notifier_block *n,
		struct cod3035x_priv *cod3035x)
{
	int ret;

	codec_notifier_t.cod3035x = cod3035x;
	ret = blocking_notifier_chain_register(&cod3035x_notifier, n);
	if (ret < 0)
		pr_err("[DEBUG] %s(%d)\n", __func__, __LINE__);
	return ret;
}


/* Notifier registration.
 * MFD driver get interrupts from PMIC and MFD filters the interrups.
 * if the interrup belongs to codec,
 * then it notify the interrupt to the codec.
 * The notifier is the way to communicate btw them
 *
 * The notifier contains
 * >> the irq1, irq2, irq3, irq4 and status registers information of codec.
 */
void cod3035x_call_notifier(int irq1, int irq2, int irq3, int irq4, int status1,
		int param1, int param2, int param3, int param4, int param5)
{
	struct cod3035x_priv *cod3035x = codec_notifier_t.cod3035x;

	dev_dbg(cod3035x->dev,
			"[DEBUG] %s(%d)  0x1: %02x 0x2: %02x 0x3: %02x 0x4: %02x\n",
			__func__, __LINE__, irq1, irq2, irq3, irq4);

	cod3035x->irq_val[0] = irq1;
	cod3035x->irq_val[1] = irq2;
	cod3035x->irq_val[2] = irq3;
	cod3035x->irq_val[3] = irq4;
	cod3035x->irq_val[4] = status1;
	cod3035x->irq_val[5] = param1;
	cod3035x->irq_val[6] = param2;
	cod3035x->irq_val[7] = param3;
	cod3035x->irq_val[8] = param4;
	cod3035x->irq_val[9] = param5;

	blocking_notifier_call_chain(&cod3035x_notifier, 0, &codec_notifier_t);
}
EXPORT_SYMBOL(cod3035x_call_notifier);
struct notifier_block codec_notifier;

/* To support PBA function test */
#include "../../sound/soc/samsung/jack_cod3035.c"

static int cod3035x_codec_probe(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "3035x CODEC_PROBE: (*) %s\n", __func__);
	cod3035x->codec = codec;

	cod3035x->vdd = devm_regulator_get(codec->dev, "vdd_ldo36");
	if (IS_ERR(cod3035x->vdd)) {
		dev_warn(codec->dev, "failed to get regulator vdd\n");
		return PTR_ERR(cod3035x->vdd);
	}

	cod3035x->vdd2 = devm_regulator_get(codec->dev, "vdd_ldo37");
	if (IS_ERR(cod3035x->vdd2)) {
		dev_warn(codec->dev, "failed to get regulator vdd2\n");
		return PTR_ERR(cod3035x->vdd2);
	}

#ifdef CONFIG_PM
	pm_runtime_get_sync(codec->dev);
#else
	cod3035x_enable(codec->dev);
#endif

	cod3035x->is_probe_done = true;

	/* Initialize work queue for button handling */
	INIT_WORK(&cod3035x->buttons_work, cod3035x_buttons_work);
	cod3035x->buttons_wq = create_singlethread_workqueue("buttons_wq");
	if (cod3035x->buttons_wq == NULL) {
		dev_err(codec->dev, "Failed to create buttons_wq\n");
		return -ENOMEM;
	}

	/* Initiallize work queue for jack detect handling */
	INIT_WORK(&cod3035x->jack_det_work, cod3035x_jack_det_work);
	cod3035x->jack_det_wq = create_singlethread_workqueue("jack_det_wq");
	if (cod3035x->jack_det_wq == NULL) {
		dev_err(codec->dev, "Failed to create jack_det_wq\n");
		return -ENOMEM;
	}

	/* Initialize work queue for water detect handling */
	INIT_WORK(&cod3035x->gdet_adc_work, cod3035x_gdet_adc_work);
	cod3035x->gdet_adc_wq = create_singlethread_workqueue("gdet_adc_wq");
	if (cod3035x->gdet_adc_wq == NULL) {
		dev_err(codec->dev, "Failed to create gdet_adc_wq\n");
		return -ENOMEM;
	}

	INIT_WORK(&cod3035x->adc_mute_work, cod3035x_adc_mute_work);
		cod3035x->adc_mute_wq = create_singlethread_workqueue("adc_mute_wq");
	if (cod3035x->adc_mute_wq == NULL) {
		dev_err(codec->dev, "Failed to create adc_mute_wq\n");
		return -ENOMEM;
	}

	cod3035x_adc_start(cod3035x);

	cod3035x->aifrate = COD3035X_SAMPLE_RATE_48KHZ;

	cod3035x_i2c_parse_dt(cod3035x);

	cod3035x->jack_det.adc_val = -EINVAL;
	cod3035x->jack_det.gdet_adc_val = -EINVAL;

	mutex_init(&cod3035x->jackdet_lock);
	mutex_init(&cod3035x->key_lock);
	mutex_init(&cod3035x->adc_mute_lock);

	/*
	 * interrupt pin should be shared with pmic.
	 * so codec driver use notifier because of knowing
	 * the interrupt status from mfd.
	 */
	codec_notifier.notifier_call = cod3035x_notifier_handler,
		cod3035x_register_notifier(&codec_notifier, cod3035x);

	set_codec_notifier_flag();

	msleep(20);

	cod3035x_codec_initialize(codec);

	/* it should be modify to move machine driver */
	cod3035x_jack_mic_register(codec);

	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "HPOUTLN");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "EPOUTN");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "IN1L");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "IN2L");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "IN3L");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "IN4L");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "AIF Playback");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "AIF Capture");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "AIF2 Playback");
	snd_soc_dapm_ignore_suspend(snd_soc_codec_get_dapm(codec), "AIF2 Capture");
	snd_soc_dapm_sync(snd_soc_codec_get_dapm(codec));

	/* To support PBA function test */
	create_jack_devices(cod3035x);

#ifdef CONFIG_PM
	pm_runtime_put_sync(codec->dev);
#else
	cod3035x_disable(codec->dev);
#endif
	return 0;
}

static int cod3035x_codec_remove(struct snd_soc_codec *codec)
{
	struct cod3035x_priv *cod3035x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "(*) %s called\n", __func__);
	cancel_delayed_work_sync(&cod3035x->key_work);
	if (cod3035x->int_gpio) {
		free_irq(gpio_to_irq(cod3035x->int_gpio), cod3035x);
		gpio_free(cod3035x->int_gpio);
	}

	if ((cod3035x->use_det_gdet_adc_mode == 2) && cod3035x->adc_pin) {
		devm_gpio_free(cod3035x->codec->dev, cod3035x->adc_pin);
	}

	cod3035x_regulators_disable(codec);
	destroy_workqueue(cod3035x->buttons_wq);
	destroy_workqueue(cod3035x->jack_det_wq);
	destroy_workqueue(cod3035x->gdet_adc_wq);
	cod3035x_adc_stop(cod3035x);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_cod3035x = {
	.probe = cod3035x_codec_probe,
	.remove = cod3035x_codec_remove,
	.controls = cod3035x_snd_controls,
	.num_controls = ARRAY_SIZE(cod3035x_snd_controls),
	.dapm_widgets = cod3035x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cod3035x_dapm_widgets),
	.dapm_routes = cod3035x_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(cod3035x_dapm_routes),
	.ignore_pmdown_time = true,
	.idle_bias_off = true,
};

static int cod3035x_i2c_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	struct cod3035x_priv *cod3035x;
	struct pinctrl *pinctrl;
	int ret;

	cod3035x = kzalloc(sizeof(struct cod3035x_priv), GFP_KERNEL);
	if (cod3035x == NULL)
		return -ENOMEM;
	cod3035x->dev = &i2c->dev;
	cod3035x->i2c_addr = i2c->addr;
	cod3035x->use_external_jd = false;
	cod3035x->is_probe_done = false;
	cod3035x->use_btn_adc_mode = false;
	cod3035x->regulator_count = 0;

	cod3035x->regmap = devm_regmap_init_i2c(i2c, &cod3035x_regmap);
	if (IS_ERR(cod3035x->regmap)) {
		dev_err(&i2c->dev, "Failed to allocate regmap: %li\n",
				PTR_ERR(cod3035x->regmap));
		ret = -ENOMEM;
		goto err;
	}

	regcache_mark_dirty(cod3035x->regmap);

	pinctrl = devm_pinctrl_get(&i2c->dev);
	if (IS_ERR(pinctrl)) {
		dev_warn(&i2c->dev, "did not get pins for codec: %li\n",
				PTR_ERR(pinctrl));
	} else {
		cod3035x->pinctrl = pinctrl;
		dev_err(&i2c->dev, "cod3035x_i2c_probe pinctrl\n");
	}

	i2c_set_clientdata(i2c, cod3035x);

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_cod3035x,
			cod3035x_dai, ARRAY_SIZE(cod3035x_dai));
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to register codec: %d\n", ret);
		goto err;
	}
#ifdef CONFIG_PM
	pm_runtime_enable(cod3035x->dev);
#endif

	return ret;

err:
	kfree(cod3035x);
	return ret;
}

static int cod3035x_i2c_remove(struct i2c_client *i2c)
{
	struct cod3035x_priv *cod3035x = dev_get_drvdata(&i2c->dev);

	snd_soc_unregister_codec(&i2c->dev);
	kfree(cod3035x);
	return 0;
}

static int cod3035x_enable(struct device *dev)
{
	struct cod3035x_priv *cod3035x = dev_get_drvdata(dev);

	dev_dbg(dev, "(*) %s\n", __func__);

	abox_enable_mclk(true);

	cod3035x_regulators_enable(cod3035x->codec);

	cod3035x->is_suspend = false;

	/* Disable cache_only feature and sync the cache with h/w */
	regcache_cache_only(cod3035x->regmap, false);
	cod3035x_reg_restore(cod3035x->codec);

	return 0;
}

static int cod3035x_disable(struct device *dev)
{
	struct cod3035x_priv *cod3035x = dev_get_drvdata(dev);

	dev_dbg(dev, "(*) %s\n", __func__);

	cod3035x->is_suspend = true;

	/* As device is going to suspend-state, limit the writes to cache */
	regcache_cache_only(cod3035x->regmap, true);

	cod3035x_regulators_disable(cod3035x->codec);

	abox_enable_mclk(false);

	return 0;
}

static int cod3035x_sys_suspend(struct device *dev)
{
#ifndef CONFIG_PM
	if (abox_is_on()) {
		dev_dbg(dev, "(*)Don't suspend codec3035x, cp functioning\n");
		return 0;
	}
	dev_dbg(dev, "(*) %s\n", __func__);
	cod3035x_disable(dev);
#endif

	return 0;
}

static int cod3035x_sys_resume(struct device *dev)
{
#ifndef CONFIG_PM
	struct cod3035x_priv *cod3035x = dev_get_drvdata(dev);

	if (!cod3035x->is_suspend) {
		dev_dbg(dev, "(*)codec3035x not resuming, cp functioning\n");
		return 0;
	}
	dev_dbg(dev, "(*) %s\n", __func__);
	cod3035x_enable(dev);
#endif

	return 0;
}

#ifdef CONFIG_PM
static int cod3035x_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "(*) %s\n", __func__);
	cod3035x_enable(dev);

	return 0;
}

static int cod3035x_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "(*) %s\n", __func__);
	cod3035x_disable(dev);

	return 0;
}
#endif

static const struct dev_pm_ops cod3035x_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(
			cod3035x_sys_suspend,
			cod3035x_sys_resume
			)
#ifdef CONFIG_PM
		SET_RUNTIME_PM_OPS(
				cod3035x_runtime_suspend,
				cod3035x_runtime_resume,
				NULL
				)
#endif
};

static const struct i2c_device_id cod3035x_i2c_id[] = {
	{ "cod3035x", 3035 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cod3035x_i2c_id);

const struct of_device_id cod3035x_of_match[] = {
	{ .compatible = "codec,cod3035x", },
	{},
};

static struct i2c_driver cod3035x_i2c_driver = {
	.driver = {
		.name = "cod3035x",
		.owner = THIS_MODULE,
		.pm = &cod3035x_pm,
		.of_match_table = of_match_ptr(cod3035x_of_match),
	},
	.probe = cod3035x_i2c_probe,
	.remove = cod3035x_i2c_remove,
	.id_table = cod3035x_i2c_id,
};

module_i2c_driver(cod3035x_i2c_driver);

MODULE_DESCRIPTION("ASoC COD3035X driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:COD3035X-codec");