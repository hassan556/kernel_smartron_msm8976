/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/regmap.h>
#include "wsa881x-analog.h"
#include "../msm/msm-audio-pinctrl.h"

#define SPK_GAIN_12DB 4

/*
 * Private data Structure for wsa881x. All parameters related to
 * WSA881X codec needs to be defined here.
 */
struct wsa881x_pdata {
	struct regmap *regmap[MAX_WSA881X_DEVICE];
	struct i2c_client *client[MAX_WSA881X_DEVICE];
	struct snd_soc_codec *codec;

	/* track wsa881x status during probe */
	u8 status;
	bool boost_enable;
	bool visense_enable;
	int spk_pa_gain;
	struct i2c_msg xfer_msg[2];
	struct mutex xfer_lock;
	bool regmap_flag;
};

enum wsa881x_status {
	WSA881X_STATUS_PROBING,
	WSA881X_STATUS_I2C,
};

struct wsa881x_pdata wsa_pdata[MAX_WSA881X_DEVICE];

static enum wsa881x_status wsa881x_status = -1;

static int wsa881x_populate_dt_pdata(struct device *dev);

static int delay_array_msec[] = {10, 20, 30, 40, 50};

static int wsa881x_i2c_addr = -1;

static const char * const wsa881x_spk_pa_gain_text[] = {
"POS_13P5_DB", "POS_12_DB", "POS_10P5_DB", "POS_9_DB", "POS_7P5_DB",
"POS_6_DB", "POS_4P5_DB", "POS_3_DB", "POS_1P5_DB", "POS_0_DB"};

static const struct soc_enum wsa881x_spk_pa_gain_enum[] = {
		SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(wsa881x_spk_pa_gain_text),
				    wsa881x_spk_pa_gain_text),
};

static int wsa881x_spk_pa_gain_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = (snd_soc_read(codec,
						WSA881X_SPKR_DRV_GAIN) >> 4);

	dev_dbg(codec->dev, "%s: spk_pa_gain = %ld\n", __func__,
				ucontrol->value.integer.value[0]);

	return 0;
}

static int wsa881x_spk_pa_gain_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);

	if (ucontrol->value.integer.value[0] < 0 ||
		ucontrol->value.integer.value[0] > 0xC) {
		dev_err(codec->dev, "%s: Unsupported gain val %ld\n",
			 __func__, ucontrol->value.integer.value[0]);
		return -EINVAL;
	}
	wsa881x->spk_pa_gain = ucontrol->value.integer.value[0];
	dev_dbg(codec->dev, "%s: ucontrol->value.integer.value[0] = %ld\n",
			 __func__, ucontrol->value.integer.value[0]);

	return 0;
}

static int get_i2c_wsa881x_device_index(u16 reg)
{
	u16 mask = 0x0f00;
	int value = 0;

	value = ((reg & mask) >> 8) & 0x000f;

	switch (value) {
	case 0:
		return 0;
	case 1:
		return 1;
	default:
		break;
	}
	return -EINVAL;
}

static int wsa881x_i2c_write_device(struct wsa881x_pdata *wsa881x,
			unsigned int reg, unsigned int val)
{
	int i = 0, rc = 0;
	int wsa881x_index;
	struct i2c_msg *msg;
	int ret = 0;
	int bytes = 1;
	u8 reg_addr = 0;
	u8 data[bytes + 1];

	wsa881x_index = get_i2c_wsa881x_device_index(reg);
	if (wsa881x_index < 0) {
		pr_err("%s:invalid register to write\n", __func__);
		return -EINVAL;
	}
	if (wsa881x->regmap_flag) {
		/* sleep of 5ms recommended every read/write by HW team */
		usleep_range(5000, 5010);
		rc = regmap_write(wsa881x->regmap[wsa881x_index], reg, val);
		for (i = 0; rc && i < ARRAY_SIZE(delay_array_msec); i++) {
			pr_debug("Failed writing reg=%u - retry(%d)\n", reg, i);
			/* retry after delay of increasing order */
			msleep(delay_array_msec[i]);
			rc = regmap_write(wsa881x->regmap[wsa881x_index],
								reg, val);
		}
		if (rc)
			pr_err("Failed writing reg=%u rc=%d\n", reg, rc);
		else
			pr_debug("write sucess register = %x val = %x\n",
							reg, val);
	} else {
		/* sleep of 5ms recommended every read/write by HW team */
		usleep_range(5000, 5010);
		reg_addr = (u8)reg;
		msg = &wsa881x->xfer_msg[0];
		msg->addr = wsa881x->client[wsa881x_index]->addr;
		msg->len = bytes + 1;
		msg->flags = 0;
		data[0] = reg;
		data[1] = (u8)val;
		msg->buf = data;
		ret = i2c_transfer(wsa881x->client[wsa881x_index]->adapter,
						wsa881x->xfer_msg, 1);
		/* Try again if the write fails */
		if (ret != 1) {
			ret = i2c_transfer(
					wsa881x->client[wsa881x_index]->adapter,
							wsa881x->xfer_msg, 1);
			if (ret != 1) {
				pr_err("failed to write the device\n");
				return ret;
			}
		}
		pr_debug("write sucess register = %x val = %x\n", reg, data[1]);
	}
	return rc;
}

static int wsa881x_i2c_read_device(struct wsa881x_pdata *wsa881x,
				unsigned int reg)
{
	int wsa881x_index;
	int i = 0, rc = 0;
	unsigned int val;
	struct i2c_msg *msg;
	int ret = 0;
	u8 reg_addr = 0;
	u8 dest[5];

	wsa881x_index = get_i2c_wsa881x_device_index(reg);
	if (wsa881x_index < 0) {
		pr_err("%s:invalid register to read\n", __func__);
		return -EINVAL;
	}
	if (wsa881x->regmap_flag) {
		/* sleep of 5ms recommended every read/write by HW team */
		usleep_range(5000, 5010);
		rc = regmap_read(wsa881x->regmap[wsa881x_index], reg, &val);
		for (i = 0; rc && i < ARRAY_SIZE(delay_array_msec); i++) {
			pr_debug("Failed reading reg=%u - retry(%d)\n", reg, i);
			/* retry after delay of increasing order */
			msleep(delay_array_msec[i]);
			rc = regmap_read(wsa881x->regmap[wsa881x_index],
						reg, &val);
		}
		if (rc) {
			pr_err("Failed reading reg=%u rc=%d\n", reg, rc);
			return rc;
		} else {
			pr_debug("read sucess register = %x val = %x\n",
							reg, val);
		}
	} else {
		/* sleep of 5ms recommended every read/write by HW team */
		usleep_range(5000, 5010);
		reg_addr = (u8)reg;
		msg = &wsa881x->xfer_msg[0];
		msg->addr = wsa881x->client[wsa881x_index]->addr;
		msg->len = 1;
		msg->flags = 0;
		msg->buf = &reg_addr;

		msg = &wsa881x->xfer_msg[1];
		msg->addr = wsa881x->client[wsa881x_index]->addr;
		msg->len = 1;
		msg->flags = I2C_M_RD;
		msg->buf = dest;
		ret = i2c_transfer(wsa881x->client[wsa881x_index]->adapter,
					wsa881x->xfer_msg, 2);

		/* Try again if read fails first time */
		if (ret != 2) {
			ret = i2c_transfer(
				wsa881x->client[wsa881x_index]->adapter,
						wsa881x->xfer_msg, 2);
			if (ret != 2) {
				pr_err("failed to read wsa register:%d\n",
								reg);
				return ret;
			}
		}
		val = dest[0];
	}
	return val;
}

static unsigned int wsa881x_i2c_read(struct snd_soc_codec *codec,
				unsigned int reg)
{
	int wsa881x_index;

	if (codec == NULL) {
		pr_err("%s: invalid codec read\n", __func__);
		return -EINVAL;
	}
	wsa881x_index = get_i2c_wsa881x_device_index(reg);
	if (wsa881x_index < 0) {
		pr_err("%s:invalid register to read\n", __func__);
		return -EINVAL;
	}
	return wsa881x_i2c_read_device(&wsa_pdata[wsa881x_index], reg);
}

static int wsa881x_i2c_write(struct snd_soc_codec *codec, unsigned int reg,
			unsigned int val)
{
	int wsa881x_index;

	if (codec == NULL) {
		pr_err("%s: invalid codec write\n", __func__);
		return -EINVAL;
	}
	wsa881x_index = get_i2c_wsa881x_device_index(reg);
	if (wsa881x_index < 0) {
		pr_err("%s:invalid register to read\n", __func__);
		return -EINVAL;
	}
	return wsa881x_i2c_write_device(&wsa_pdata[wsa881x_index], reg, val);
}

static int wsa881x_i2c_get_client_index(struct i2c_client *client,
					int *wsa881x_index)
{
	int ret = 0;

	switch (client->addr) {
	case WSA881X_I2C_SPK0_SLAVE0_ADDR:
	case WSA881X_I2C_SPK0_SLAVE1_ADDR:
		*wsa881x_index = WSA881X_I2C_SPK0_SLAVE0;
	break;
	case WSA881X_I2C_SPK1_SLAVE0_ADDR:
	case WSA881X_I2C_SPK1_SLAVE1_ADDR:
		*wsa881x_index = WSA881X_I2C_SPK1_SLAVE0;
	break;
	default:
		ret = -EINVAL;
	break;
	}
	return ret;
}

static int wsa881x_boost_ctrl(struct snd_soc_codec *codec, bool enable)
{
	pr_debug("%s: enable:%d\n", __func__, enable);
	if (enable) {
		snd_soc_update_bits(codec, WSA881X_CDC_ANA_CLK_CTL, 0x04, 0x04);
		snd_soc_update_bits(codec, WSA881X_CDC_ANA_CLK_CTL, 0x03, 0x01);
		snd_soc_update_bits(codec, WSA881X_BOOST_EN_CTL, 0x99, 0x99);
		/* For WSA8810, start-up time is 450us as per qcrg sequence */
		usleep_range(450, 460);
	} else {
		/* ENSURE: Class-D amp is shutdown. CLK is still on */
		snd_soc_update_bits(codec, WSA881X_BOOST_EN_CTL, 0x80, 0x00);
	}
	return 0;
}

static int wsa881x_visense_txfe_ctrl(struct snd_soc_codec *codec, bool enable,
				     u8 isense1_gain, u8 isense2_gain,
				     u8 vsense_gain)
{
	u8 value = 0;

	pr_debug("%s: enable:%d\n", __func__, enable);

	if (enable) {
		value = ((isense2_gain << 6) || (isense1_gain << 4) ||
			(vsense_gain << 3));
		snd_soc_update_bits(codec, WSA881X_SPKR_PROT_FE_GAIN,
					0xF8, value);
		snd_soc_update_bits(codec, WSA881X_SPKR_PROT_FE_GAIN,
					0x01, 0x01);
	} else {
		snd_soc_update_bits(codec, WSA881X_SPKR_PROT_FE_GAIN,
					0x01, 0x00);
	}
	return 0;
}

static int wsa881x_visense_adc_ctrl(struct snd_soc_codec *codec, bool enable)
{
	pr_debug("%s: enable:%d\n", __func__, enable);
	if (enable) {
		snd_soc_update_bits(codec, WSA881X_ADC_SEL_IBIAS, 0x07, 0x04);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_SEL_IBIAS,
							0x80, 0x80);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_SEL_IBIAS,
							0x70, 0x60);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_SEL_IBIAS,
							0x0C, 0x08);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_SEL_IBIAS,
							0x03, 0x02);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_MODU_V, 0x80, 0x80);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_MODU_I, 0x80, 0x80);
	} else {
		/* Ensure: Speaker Protection has been stopped */
		snd_soc_update_bits(codec, WSA881X_ADC_EN_MODU_V, 0x80, 0x00);
		snd_soc_update_bits(codec, WSA881X_ADC_EN_MODU_I, 0x80, 0x00);
	}

	return 0;
}

static int wsa881x_rdac_ctrl(struct snd_soc_codec *codec, bool enable)
{
	pr_debug("%s: enable:%d\n", __func__, enable);
	if (enable) {
		snd_soc_update_bits(codec, WSA881X_CDC_RX_CTL, 0x80, 0x00);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x20, 0x20);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x20, 0x00);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x40, 0x40);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x80, 0x80);
	} else {
		/* Ensure class-D amp is off */
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x80, 0x00);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x40, 0x00);
	}
	return 0;
}

static int wsa881x_spkr_pa_ctrl(struct snd_soc_codec *codec, bool enable)
{
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: enable:%d\n", __func__, enable);
	if (enable) {
		/*
		 * Ensure: Boost is enabled and stable, Analog input is up
		 * and outputting silence
		 */
		snd_soc_update_bits(codec, WSA881X_SPKR_DRV_GAIN, 0x08, 0x08);
		snd_soc_update_bits(codec, WSA881X_SPKR_DAC_CTL, 0x20, 0x20);
		snd_soc_update_bits(codec, WSA881X_SPKR_DRV_EN, 0x01, 0x01);
		snd_soc_update_bits(codec, WSA881X_SPKR_OCP_CTL, 0x80, 0x00);
		/* Here gain value is in descending order, check gain table */
		if (wsa881x->spk_pa_gain > SPK_GAIN_12DB)
			snd_soc_update_bits(codec, WSA881X_SPKR_DRV_GAIN,
							0xF0, 0xC0);
		else
			snd_soc_update_bits(codec, WSA881X_SPKR_DRV_GAIN,
							0xF0, 0x40);
		snd_soc_update_bits(codec, WSA881X_SPKR_DRV_EN, 0x80, 0x80);
		usleep_range(1000, 1010);
		snd_soc_update_bits(codec, WSA881X_SPKR_DRV_GAIN, 0xF0,
						(wsa881x->spk_pa_gain << 4));
	} else {
		/*
		 * Ensure: Boost is still on, Stream from Analog input and
		 * Speaker Protection has been stopped and input is at 0V
		 */
		snd_soc_update_bits(codec, WSA881X_SPKR_DRV_EN, 0x80, 0x00);
	}
	return 0;
}

static int wsa881x_bandgap_ctrl(struct snd_soc_codec *codec, bool enable)
{
	pr_debug("%s: enable:%d\n", __func__, enable);
	if (enable) {
		snd_soc_update_bits(codec, WSA881X_TEMP_OP, 0x08, 0x08);
		usleep_range(400, 410);
	} else {
		snd_soc_update_bits(codec, WSA881X_TEMP_OP, 0x08, 0x00);
	}
	return 0;
}

static int wsa881x_temp_sensor_ctrl(struct snd_soc_codec *codec, bool enable)
{
	dev_dbg(codec->dev, "%s: enable:%d\n", __func__, enable);
	snd_soc_update_bits(codec, WSA881X_TEMP_OP, (0x01 << 2),
				(enable << 2));
	return 0;
}

static int wsa881x_get_boost(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = wsa881x->boost_enable;
	return 0;
}

static int wsa881x_set_boost(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);
	int value = ucontrol->value.integer.value[0];

	dev_dbg(codec->dev, "%s: Boost enable current %d, new %d\n",
		 __func__, wsa881x->boost_enable, value);
	wsa881x->boost_enable = value;
	return 0;
}

static int wsa881x_get_visense(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = wsa881x->visense_enable;
	return 0;
}

static int wsa881x_set_visense(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);
	int value = ucontrol->value.integer.value[0];

	dev_dbg(codec->dev, "%s: VIsense enable current %d, new %d\n",
		 __func__, wsa881x->visense_enable, value);
	wsa881x->visense_enable = value;
	return 0;
}

static const struct snd_kcontrol_new wsa881x_snd_controls[] = {
	SOC_SINGLE_EXT("BOOST Switch", SND_SOC_NOPM, 0, 1, 0,
		wsa881x_get_boost, wsa881x_set_boost),

	SOC_SINGLE_EXT("VISENSE Switch", SND_SOC_NOPM, 0, 1, 0,
		wsa881x_get_visense, wsa881x_set_visense),

	SOC_ENUM_EXT("WSA_SPK PA Gain", wsa881x_spk_pa_gain_enum[0],
		wsa881x_spk_pa_gain_get, wsa881x_spk_pa_gain_put),
};

static const char * const rdac_text[] = {
	"ZERO", "Switch",
};

static const struct soc_enum rdac_enum =
	SOC_ENUM_SINGLE(0, 0, ARRAY_SIZE(rdac_text), rdac_text);

static const struct snd_kcontrol_new rdac_mux[] = {
	SOC_DAPM_ENUM_VIRT("RDAC", rdac_enum)
};

static int wsa881x_rdac_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wsa881x_pdata *wsa881x = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s: %s %d boost %d visense %d\n",
		 __func__, w->name, event,
		wsa881x->boost_enable, wsa881x->visense_enable);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, WSA881X_CDC_RST_CTL, 0x02, 0x02);
		snd_soc_update_bits(codec, WSA881X_CDC_RST_CTL, 0x01, 0x01);
		snd_soc_update_bits(codec, WSA881X_CLOCK_CONFIG, 0x01, 0x01);
		snd_soc_update_bits(codec, WSA881X_CDC_ANA_CLK_CTL, 0x01, 0x01);
		wsa881x_bandgap_ctrl(codec, true);
		usleep_range(450, 460);
		snd_soc_write(codec, WSA881X_SPKR_DRV_DBG, 0x11);
		if (wsa881x->boost_enable)
			wsa881x_boost_ctrl(codec, true);
		break;
	case SND_SOC_DAPM_POST_PMU:
		wsa881x_rdac_ctrl(codec, true);
		if (wsa881x->visense_enable) {
			wsa881x_visense_txfe_ctrl(codec, true,
						0x00, 0x01, 0x00);
			wsa881x_visense_adc_ctrl(codec, true);
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (wsa881x->visense_enable) {
			wsa881x_visense_txfe_ctrl(codec, false,
						0x00, 0x01, 0x00);
			wsa881x_visense_adc_ctrl(codec, false);
		}
		wsa881x_rdac_ctrl(codec, false);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (wsa881x->boost_enable)
			wsa881x_boost_ctrl(codec, false);
		wsa881x_bandgap_ctrl(codec, false);
		break;
	default:
		pr_err("%s: invalid event:%d\n", __func__, event);
		return -EINVAL;
	}
	return 0;
}

static int wsa881x_spkr_pa_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	pr_debug("%s: %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		wsa881x_spkr_pa_ctrl(codec, true);
		wsa881x_temp_sensor_ctrl(codec, true);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		wsa881x_spkr_pa_ctrl(codec, false);
		break;
	default:
		pr_err("%s: invalid event:%d\n", __func__, event);
		return -EINVAL;
	}
	return 0;
}


static const struct snd_soc_dapm_widget wsa881x_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("WSA_IN"),

	SND_SOC_DAPM_DAC_E("RDAC Analog", NULL, SND_SOC_NOPM, 0, 0,
		wsa881x_rdac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_VIRT_MUX("WSA_RDAC", SND_SOC_NOPM, 0, 0,
		rdac_mux),

	SND_SOC_DAPM_PGA_E("WSA_SPKR PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
			wsa881x_spkr_pa_event,
			SND_SOC_DAPM_POST_PMU |	SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_OUTPUT("WSA_SPKR"),
};

static const struct snd_soc_dapm_route wsa881x_audio_map[] = {
	{"WSA_RDAC", "Switch", "WSA_IN"},
	{"RDAC Analog", NULL, "WSA_RDAC"},
	{"WSA_SPKR PGA", NULL, "RDAC Analog"},
	{"WSA_SPKR", NULL, "WSA_SPKR PGA"},
};


static int wsa881x_probe(struct snd_soc_codec *codec)
{
	return 0;
}

static int wsa881x_remove(struct snd_soc_codec *codec)
{
	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_wsa881x = {
	.probe	= wsa881x_probe,
	.remove	= wsa881x_remove,

	.read = wsa881x_i2c_read,
	.write = wsa881x_i2c_write,

	.reg_cache_size = WSA881X_CACHE_SIZE,
	.reg_cache_default = wsa881x_reg_defaults,
	.reg_word_size = 1,

	.controls = wsa881x_snd_controls,
	.num_controls = ARRAY_SIZE(wsa881x_snd_controls),
	.dapm_widgets = wsa881x_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(wsa881x_dapm_widgets),
	.dapm_routes = wsa881x_audio_map,
	.num_dapm_routes = ARRAY_SIZE(wsa881x_audio_map),
};

static int wsa881x_reset(struct wsa881x_pdata *pdata, bool enable)
{
	int ret = 0;
	/*
	 * shutdown the GPIOs WSA_EN, WSA_MCLK, regulators
	 * and restore defaults in soc cache when shutdown.
	 * Enable regulators, GPIOs WSA_MCLK, WSA_EN when powerup.
	 */
	if (enable) {
		ret = msm_gpioset_activate(CLIENT_WSA_BONGO_1, "wsa_clk");
		if (ret) {
			pr_err("%s: gpio set cannot be activated %s\n",
				__func__, "wsa_clk");
			return ret;
		}
		ret = msm_gpioset_activate(CLIENT_WSA_BONGO_1, "wsa_reset");
		if (ret) {
			pr_err("%s: gpio set cannot be activated %s\n",
				__func__, "wsa_reset");
			return ret;
		}
	} else {
		ret = msm_gpioset_suspend(CLIENT_WSA_BONGO_1, "wsa_reset");
		if (ret) {
			pr_err("%s: gpio set cannot be activated %s\n",
				__func__, "wsa_reset");
			return ret;
		}
		ret = msm_gpioset_suspend(CLIENT_WSA_BONGO_1, "wsa_clk");
		if (ret) {
			pr_err("%s: gpio set cannot be activated %s\n",
				__func__, "wsa_clk");
			return ret;
		}
		/*TODO: restore defaults to cache*/
	}
	return ret;
}

int wsa881x_get_client_index(void)
{
	return wsa881x_i2c_addr;
}
EXPORT_SYMBOL(wsa881x_get_client_index);

static int check_wsa881x_presence(struct i2c_client *client)
{
	int ret = 0;
	int wsa881x_index = 0;

	ret = wsa881x_i2c_get_client_index(client, &wsa881x_index);
	if (ret != 0) {
		dev_err(&client->dev, "%s: I2C get codec I2C\n"
			"client failed\n", __func__);
		return ret;
	}
	ret = wsa881x_i2c_read_device(&wsa_pdata[wsa881x_index],
					WSA881X_CDC_RST_CTL);
	if (ret < 0) {
		dev_err(&client->dev, "failed to read wsa881x with addr %x\n",
				client->addr);
		return ret;
	}
	ret = wsa881x_i2c_write_device(&wsa_pdata[wsa881x_index],
					WSA881X_CDC_RST_CTL, 0x01);
	if (ret < 0) {
		dev_err(&client->dev, "failed write addr %x reg:0x5 val:0x1\n",
					client->addr);
		return ret;
	}
	/* allow 20ms before trigger next write to verify bongo presence */
	msleep(20);
	ret = wsa881x_i2c_write_device(&wsa_pdata[wsa881x_index],
					WSA881X_CDC_RST_CTL, 0x00);
	if (ret < 0) {
		dev_err(&client->dev, "failed write addr %x reg:0x5 val:0x0\n",
					client->addr);
		return ret;
	}
	return ret;
}

static int wsa881x_populate_dt_pdata(struct device *dev)
{
	int ret = 0;

	/* reading the gpio configurations from dtsi file */
	ret = msm_gpioset_initialize(CLIENT_WSA_BONGO_1, dev);
	if (ret < 0)
		dev_err(dev,
			"%s: error reading dtsi files%d\n", __func__, ret);
	return ret;
}

static int wsa881x_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int ret = 0;
	int wsa881x_index = 0;
	struct wsa881x_pdata *pdata = NULL;

	ret = wsa881x_i2c_get_client_index(client, &wsa881x_index);
	if (ret != 0) {
		dev_err(&client->dev, "%s: I2C get codec I2C\n"
			"client failed\n", __func__);
		return ret;
	}
	if (wsa881x_status == WSA881X_STATUS_I2C) {
		dev_dbg(&client->dev, "%s:probe for other slaves\n"
			"devices of codec I2C slave Addr = %x\n",
			__func__, client->addr);
		pdata = &wsa_pdata[wsa881x_index];
		dev_dbg(&client->dev, "%s:wsa_idx = %d SLAVE = %d\n",
				__func__, wsa881x_index, WSA881X_ANALOG_SLAVE);
		pdata->regmap[WSA881X_ANALOG_SLAVE] =
				devm_regmap_init_i2c(
					client,
				&wsa881x_regmap_config[WSA881X_ANALOG_SLAVE]);
		regcache_cache_bypass(pdata->regmap[WSA881X_ANALOG_SLAVE],
					true);
		if (IS_ERR(pdata->regmap[WSA881X_ANALOG_SLAVE])) {
			ret = PTR_ERR(pdata->regmap[WSA881X_ANALOG_SLAVE]);
			dev_err(&client->dev,
				"%s: regmap_init failed %d\n",
					__func__, ret);
		}
		client->dev.platform_data = pdata;
		i2c_set_clientdata(client, pdata);
		pdata->client[WSA881X_ANALOG_SLAVE] = client;
		return ret;
	} else if (wsa881x_status == WSA881X_STATUS_PROBING) {
		pdata = &wsa_pdata[wsa881x_index];
		if (client->dev.of_node) {
			dev_dbg(&client->dev, "%s:Platform data\n"
				"from device tree\n", __func__);
			ret = wsa881x_populate_dt_pdata(&client->dev);
			if (ret < 0) {
				dev_err(&client->dev,
				"%s: Fail to obtain pdata from device tree\n",
					 __func__);
				ret = -EINVAL;
				goto err;
			}
			client->dev.platform_data = pdata;
		} else {
			dev_dbg(&client->dev, "%s:Platform data from\n"
				"board file\n", __func__);
			pdata = client->dev.platform_data;
		}
		if (!pdata) {
			dev_dbg(&client->dev, "no platform data?\n");
			ret = -EINVAL;
			goto err;
		}
		i2c_set_clientdata(client, pdata);
		dev_set_drvdata(&client->dev, client);

		pdata->regmap[WSA881X_DIGITAL_SLAVE] =
				devm_regmap_init_i2c(
					client,
				&wsa881x_regmap_config[WSA881X_DIGITAL_SLAVE]);
		regcache_cache_bypass(pdata->regmap[WSA881X_DIGITAL_SLAVE],
					true);
		if (IS_ERR(pdata->regmap[WSA881X_DIGITAL_SLAVE])) {
			ret = PTR_ERR(pdata->regmap[WSA881X_DIGITAL_SLAVE]);
			dev_err(&client->dev, "%s: regmap_init failed %d\n",
				__func__, ret);
			goto err;
		}
		/* bus reset sequence */
		ret = wsa881x_reset(pdata, true);
		if (ret < 0) {
			dev_err(&client->dev, "%s: WSA enable Failed %d\n",
				__func__, ret);
			goto err;
		}
		pdata->client[WSA881X_DIGITAL_SLAVE] = client;
		pdata->regmap_flag = true;
		ret = check_wsa881x_presence(client);
		if (ret < 0) {
			dev_err(&client->dev,
				"failed to ping wsa with addr:%x, ret = %d\n",
						client->addr, ret);
			goto err;
		} else {
			if (client->addr == WSA881X_I2C_SPK0_SLAVE0_ADDR)
				wsa881x_i2c_addr = WSA881X_I2C_SPK0_SLAVE0_ADDR;
			else if (client->addr == WSA881X_I2C_SPK1_SLAVE0_ADDR)
				wsa881x_i2c_addr = WSA881X_I2C_SPK1_SLAVE0_ADDR;
		}
		ret = snd_soc_register_codec(&client->dev,
					&soc_codec_dev_wsa881x,
					     NULL, 0);
		if (ret < 0)
			goto err;
		wsa881x_status = WSA881X_STATUS_I2C;
	}
	return 0;
err:
	return ret;
}

static int wsa881x_i2c_remove(struct i2c_client *client)
{
	struct wsa881x_pdata *wsa881x = i2c_get_clientdata(client);

	snd_soc_unregister_codec(&client->dev);
	i2c_set_clientdata(client, NULL);
	kfree(wsa881x);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int wsa881x_i2c_suspend(struct device *dev)
{
	pr_debug("%s: system suspend\n", __func__);
	return 0;
}

static int wsa881x_i2c_resume(struct device *dev)
{
	pr_debug("%s: system resume\n", __func__);
	return 0;
}

static const struct dev_pm_ops wsa881x_i2c_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(wsa881x_i2c_suspend, wsa881x_i2c_resume)
};
#endif /* CONFIG_PM_SLEEP */

static const struct i2c_device_id wsa881x_i2c_id[] = {
	{"wsa881x-i2c-dev", WSA881X_I2C_SPK0_SLAVE0_ADDR},
	{"wsa881x-i2c-dev", WSA881X_I2C_SPK0_SLAVE1_ADDR},
	{"wsa881x-i2c-dev", WSA881X_I2C_SPK1_SLAVE0_ADDR},
	{"wsa881x-i2c-dev", WSA881X_I2C_SPK1_SLAVE1_ADDR},
	{}
};

MODULE_DEVICE_TABLE(i2c, wsa881x_i2c_id);


static struct of_device_id msm_match_table[] = {
	{.compatible = "qcom,wsa881x-i2c-codec"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_match_table);

static struct i2c_driver wsa881x_codec_driver = {
	.driver = {
		.name = "wsa881x-i2c-codec",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM_SLEEP
		.pm = &wsa881x_i2c_pm_ops,
#endif
		.of_match_table = msm_match_table,
	},
	.id_table = wsa881x_i2c_id,
	.probe = wsa881x_i2c_probe,
	.remove = wsa881x_i2c_remove,
};

static int __init wsa881x_codec_init(void)
{
	wsa881x_status = WSA881X_STATUS_PROBING;
	return i2c_add_driver(&wsa881x_codec_driver);
}
module_init(wsa881x_codec_init);

static void __exit wsa881x_codec_exit(void)
{
	i2c_del_driver(&wsa881x_codec_driver);
}

module_exit(wsa881x_codec_exit);

MODULE_DESCRIPTION("WSA881x Codec driver");
MODULE_LICENSE("GPL v2");