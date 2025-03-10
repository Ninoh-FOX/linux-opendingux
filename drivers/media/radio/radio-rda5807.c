// SPDX-License-Identifier: GPL-2.0
/*
 * radio-rda5807.c - Driver for using the RDA5807 FM tuner chip via I2C
 *
 * Copyright (c) 2011 Maarten ter Huurne <maarten@treewalker.org>
 * Copyright (c) 2021 Paul Cercueil <paul@crapouillou.net>
 *
 * Many thanks to Jérôme Veres for his command line radio application that
 * demonstrates how the chip can be controlled via I2C.
 *
 * Also thanks to Marcos Paulo de Souza for several patches to this driver.
 *
 * The RDA5807 has three ways of accessing registers:
 * - I2C address 0x10: sequential access, RDA5800 style
 * - I2C address 0x11: random access
 * - I2C address 0x60: sequential access, TEA5767 compatible
 *
 * This driver only supports random access to the registers.
 */


#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>


enum rda5807_reg {
	RDA5807_REG_CHIPID		= 0x00,
	RDA5807_REG_CTRL		= 0x02,
	RDA5807_REG_CHAN		= 0x03,
	RDA5807_REG_IOCFG		= 0x04,
	RDA5807_REG_INPUT		= 0x05,
	RDA5807_REG_BAND		= 0x07,
	RDA5807_REG_SEEKRES		= 0x0A,
	RDA5807_REG_SIGNAL		= 0x0B,
};

#define RDA5807_CTRL_DHIZ		BIT(15)
#define RDA5807_CTRL_DMUTE		BIT(14)
#define RDA5807_CTRL_MONO		BIT(13)
#define RDA5807_CTRL_BASS		BIT(12)
#define RDA5807_CTRL_SEEKUP		BIT(9)
#define RDA5807_CTRL_SEEK		BIT(8)
#define RDA5807_CTRL_SKMODE		BIT(7)
#define RDA5807_CTRL_CLKMODE		GENMASK(6, 4)
#define RDA5807_CTRL_SOFTRESET		BIT(1)
#define RDA5807_CTRL_ENABLE		BIT(0)

#define RDA5807_CHAN_WRCHAN		GENMASK(15, 6)
#define RDA5807_CHAN_TUNE		BIT(4)
#define RDA5807_CHAN_BAND		GENMASK(3, 2)
#define RDA5807_CHAN_SPACE		GENMASK(1, 0)

#define RDA5807_IOCFG_DEEMPHASIS	BIT(11)
#define RDA5807_IOCFG_I2S_EN		BIT(6)

#define RDA5807_INPUT_LNA_PORT		GENMASK(7, 6)
#define RDA5807_INPUT_LNA_ICSEL		GENMASK(5, 4)
#define RDA5807_INPUT_VOLUME		GENMASK(3, 0)

#define RDA5807_BAND_65M_BAND		BIT(9)

#define RDA5807_SEEKRES_COMPLETE	BIT(14)
#define RDA5807_SEEKRES_FAIL		BIT(13)
#define RDA5807_SEEKRES_STEREO		BIT(10)
#define RDA5807_SEEKRES_READCHAN	GENMASK(9, 0)

#define RDA5807_SIGNAL_RSSI		GENMASK(15, 9)


#define RDA5807_AUTOSUSPEND_DELAY_MS	5000


struct rda5807_driver {
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device video_dev;
	struct v4l2_device v4l2_dev;

	struct device *dev;
	struct regmap *map;
	struct regulator *supply;

	const struct v4l2_frequency_band *band;

	bool unmuted;
};

enum rda5807_bands {
	RDA5807_BAND_WORLDWIDE,
	RDA5807_BAND_EAST_EUROPE,
	RDA5807_BAND_UNKNOWN,
};

static const struct v4l2_frequency_band rda5807_bands[] = {
	[RDA5807_BAND_WORLDWIDE] = {
		.index = RDA5807_BAND_WORLDWIDE,
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_LOW |
			V4L2_TUNER_CAP_FREQ_BANDS,
		.rangelow = 1216000,   /* 76.0 MHz */
		.rangehigh = 1728000,  /* 108.0 MHz */
		.modulation = V4L2_BAND_MODULATION_FM,
	},
	[RDA5807_BAND_EAST_EUROPE] = {
		.index = RDA5807_BAND_EAST_EUROPE,
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_LOW |
			V4L2_TUNER_CAP_FREQ_BANDS,
		.rangelow = 1040000,   /* 65.0 MHz */
		.rangehigh = 1216000,  /* 76.0 MHz */
		.modulation = V4L2_BAND_MODULATION_FM,
	},
	[RDA5807_BAND_UNKNOWN] = {
		.index = RDA5807_BAND_UNKNOWN,
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_LOW |
			V4L2_TUNER_CAP_FREQ_BANDS,
		.rangelow = 800000,   /* 50.0 MHz */
		.rangehigh = 1040000,  /* 65.0 MHz */
		.modulation = V4L2_BAND_MODULATION_FM,
	},
};

static const struct v4l2_frequency_band *rda5807_get_band(unsigned long min,
							  unsigned long max)
{
	const struct v4l2_frequency_band *band;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rda5807_bands); i++) {
		band = &rda5807_bands[i];

		if (band->rangelow <= min && band->rangehigh >= max)
			return band;
	}

	return NULL;
}

static int rda5807_set_band(struct rda5807_driver *radio,
			    const struct v4l2_frequency_band *band)
{
	u16 val;
	int err;

	if (band->index == RDA5807_BAND_EAST_EUROPE)
		err = regmap_set_bits(radio->map, RDA5807_REG_BAND,
				      RDA5807_BAND_65M_BAND);
	else
		err = regmap_clear_bits(radio->map, RDA5807_REG_BAND,
					RDA5807_BAND_65M_BAND);
	if (err)
		return err;

	if (band->index == RDA5807_BAND_WORLDWIDE)
		val = FIELD_PREP(RDA5807_CHAN_BAND, 2);
	else
		val = FIELD_PREP(RDA5807_CHAN_BAND, 3);

	err = regmap_update_bits(radio->map, RDA5807_REG_CHAN,
				 RDA5807_CHAN_BAND, val);
	if (err)
		return err;

	radio->band = band;
	return 0;
}

static int rda5807_set_mute(struct rda5807_driver *radio, int muted)
{
	u16 val = muted ? 0 : RDA5807_CTRL_DMUTE /* disable mute */;

	dev_dbg(radio->dev, "set mute to %d\n", muted);

	return regmap_update_bits(radio->map, RDA5807_REG_CTRL,
				  RDA5807_CTRL_DMUTE, val);
}

static int rda5807_set_volume(struct rda5807_driver *radio, int volume)
{
	dev_dbg(radio->dev, "set volume to %d\n", volume);

	return regmap_update_bits(radio->map, RDA5807_REG_INPUT,
				  RDA5807_INPUT_VOLUME,
				  FIELD_PREP(RDA5807_INPUT_VOLUME, volume));
}

static int rda5807_set_deemphasis(struct rda5807_driver *radio,
				  enum v4l2_deemphasis deemp)
{
	int err;

	if (deemp == V4L2_DEEMPHASIS_50_uS)
		err = regmap_set_bits(radio->map, RDA5807_REG_IOCFG,
				      RDA5807_IOCFG_DEEMPHASIS);
	else
		err = regmap_clear_bits(radio->map, RDA5807_REG_IOCFG,
					RDA5807_IOCFG_DEEMPHASIS);

	dev_dbg(radio->dev, "set deemphasis to %d\n", deemp);
	return err;
}

static inline struct rda5807_driver *ctrl_to_radio(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct rda5807_driver, ctrl_handler);
}

static int rda5807_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rda5807_driver *radio = ctrl_to_radio(ctrl);
	int err;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		if (radio->unmuted == !ctrl->val)
			break;

		if (ctrl->val) {
			pm_runtime_mark_last_busy(radio->dev);
			err = pm_runtime_put_autosuspend(radio->dev);
			if (err < 0)
				return err;
		} else {
			err = pm_runtime_get_sync(radio->dev);
			if (err < 0) {
				pm_runtime_put_noidle(radio->dev);
				return err;
			}
		}

		err = rda5807_set_mute(radio, ctrl->val);
		if (err)
			return err;

		radio->unmuted = !ctrl->val;
		break;
	case V4L2_CID_AUDIO_VOLUME:
		return rda5807_set_volume(radio, ctrl->val);
	case V4L2_CID_TUNE_DEEMPHASIS:
		return rda5807_set_deemphasis(radio, ctrl->val);
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops rda5807_ctrl_ops = {
	.s_ctrl = rda5807_s_ctrl,
};

static int rda5807_vidioc_querycap(struct file *file, void *fh,
				   struct v4l2_capability *cap)
{
	*cap = (struct v4l2_capability) {
		.driver		= "rda5807",
		.card		= "RDA5807 FM receiver",
		.bus_info	= "I2C",
		.device_caps	= V4L2_CAP_RADIO | V4L2_CAP_TUNER
						 | V4L2_CAP_HW_FREQ_SEEK,
	};
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int rda5807_vidioc_g_audio(struct file *file, void *fh,
				  struct v4l2_audio *a)
{
	if (a->index != 0)
		return -EINVAL;

	*a = (struct v4l2_audio) {
		.name = "Radio",
		.capability = V4L2_AUDCAP_STEREO,
		.mode = 0,
	};

	return 0;
}

static int rda5807_vidioc_g_tuner(struct file *file, void *fh,
				  struct v4l2_tuner *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	unsigned int seekres, signal;
	u32 rxsubchans;
	int err, active;

	if (a->index != 0)
		return -EINVAL;

	active = pm_runtime_get_if_in_use(radio->dev);
	if (active < 0)
		return active;

	if (active == 0) {
		signal = 0;
		rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
	} else {
		err = regmap_read(radio->map, RDA5807_REG_SEEKRES, &seekres);
		if (err < 0)
			goto out_runtime_pm_put;

		if (seekres & RDA5807_SEEKRES_COMPLETE &&
		    !(seekres & RDA5807_SEEKRES_FAIL))
			/* mono/stereo known */
			rxsubchans = seekres & RDA5807_SEEKRES_STEREO
				? V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO;
		else
			/* mono/stereo unknown */
			rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;

		err = regmap_read(radio->map, RDA5807_REG_SIGNAL, &signal);
		if (err < 0)
			goto out_runtime_pm_put;
	}

	*a = (struct v4l2_tuner) {
		.name = "FM",
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO,
		/* unit is 1/16 kHz */
		.rangelow   = 50000 * 16,
		.rangehigh  = 108000 * 16,
		.rxsubchans = rxsubchans,
		/* TODO: Implement forced mono (RDA5807_CTRL_MONO). */
		.audmode = V4L2_TUNER_MODE_STEREO,
		.signal = signal & RDA5807_SIGNAL_RSSI,
		.afc = 0, /* automatic frequency control */
	};

	err = 0;
out_runtime_pm_put:
	if (active > 0) {
		pm_runtime_mark_last_busy(radio->dev);
		pm_runtime_put_autosuspend(radio->dev);
	}
	return err;
}

static int rda5807_vidioc_g_frequency(struct file *file, void *fh,
				      struct v4l2_frequency *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	unsigned int val;
	int err;

	if (a->tuner != 0)
		return -EINVAL;
	if (!radio->band)
		return -EINVAL;

	err = regmap_read(radio->map, RDA5807_REG_SEEKRES, &val);
	if (err < 0)
		return err;

	a->frequency = 400 * (val & RDA5807_SEEKRES_READCHAN) + radio->band->rangelow;
	return 0;
}

static int rda5807_vidioc_s_frequency(struct file *file, void *fh,
				      const struct v4l2_frequency *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	const struct v4l2_frequency_band *band;
	u16 mask = 0;
	u16 val = 0;
	int err, active;

	if (a->tuner != 0)
		return -EINVAL;
	if (a->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	band = rda5807_get_band(a->frequency, a->frequency);
	if (!band)
		return -ERANGE;

	dev_dbg(radio->dev, "set freq to %u kHz\n", a->frequency / 16);

	err = rda5807_set_band(radio, band);
	if (err)
		return err;

	/* select 25 kHz channel spacing */
	mask |= RDA5807_CHAN_SPACE;
	val  |= FIELD_PREP(RDA5807_CHAN_SPACE, 0x3);

	/* select frequency */
	mask |= RDA5807_CHAN_WRCHAN;
	val  |= FIELD_PREP(RDA5807_CHAN_WRCHAN, (a->frequency + 200) / 400);

	err = regmap_update_bits(radio->map, RDA5807_REG_CHAN, mask, val);
	if (err)
		return err;

	active = pm_runtime_get_if_in_use(radio->dev);
	if (active <= 0)
		return active;

	/* start tune operation */
	err = regmap_write_bits(radio->map, RDA5807_REG_CHAN,
				RDA5807_CHAN_TUNE, RDA5807_CHAN_TUNE);

	pm_runtime_mark_last_busy(radio->dev);
	pm_runtime_put_autosuspend(radio->dev);

	return err;
}

static int rda5807_vidioc_s_hw_freq_seek(struct file *file, void *fh,
					 const struct v4l2_hw_freq_seek *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	const struct v4l2_frequency_band *band;
	unsigned int val = RDA5807_CTRL_SEEK;
	unsigned int freq, spacing, increment;
	int err;

	if (a->tuner != 0)
		return -EINVAL;
	if (a->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	switch (a->spacing) {
	case 25000:
		spacing = 0x3;
		break;
	case 50000:
		spacing = 0x2;
		break;
	case 100000:
		spacing = 0x0;
		break;
	case 200000:
		spacing = 0x1;
		break;
	default:
		return -EINVAL;
	}

	band = rda5807_get_band(a->rangelow, a->rangehigh);
	if (!band)
		return -ERANGE;

	err = pm_runtime_get_sync(radio->dev);
	if (err < 0) {
		dev_err(radio->dev, "Unable to runtime get: %d\n", err);
		pm_runtime_put_noidle(radio->dev);
		return err;
	}

	/* Configure channel spacing */
	err = regmap_update_bits(radio->map, RDA5807_REG_CHAN,
				 RDA5807_CHAN_SPACE,
				 FIELD_PREP(RDA5807_CHAN_SPACE, spacing));
	if (err < 0)
		goto out_runtime_pm_put;

	err = rda5807_set_band(radio, band);
	if (err < 0)
		goto out_runtime_pm_put;

	/* seek up or down? */
	if (a->seek_upward)
		val |= RDA5807_CTRL_SEEKUP;

	/* wrap around at band limit? */
	if (!a->wrap_around)
		val |= RDA5807_CTRL_SKMODE;

	/* Send seek command */
	err = regmap_update_bits(radio->map, RDA5807_REG_CTRL,
				 RDA5807_CTRL_SEEKUP |
				 RDA5807_CTRL_SKMODE |
				 RDA5807_CTRL_SEEK, val);
	if (err < 0)
		goto out_runtime_pm_put;

	increment = a->spacing * 16 / 1000;

	for (freq = a->rangelow; freq <= a->rangehigh; freq += increment) {
		/*
		 * The programming guide says we should wait for 35 ms for each
		 * frequency tested.
		 */
		msleep(35);

		err = regmap_read(radio->map, RDA5807_REG_SEEKRES, &val);
		if (err < 0)
			goto out_runtime_pm_put;

		/* Seek done? */
		if (val & RDA5807_SEEKRES_COMPLETE)
			break;
	}

	err = regmap_clear_bits(radio->map, RDA5807_REG_CTRL,
				RDA5807_CTRL_SEEK);
	if (err)
		goto out_runtime_pm_put;

	if (freq > a->rangehigh)
		err = -ETIMEDOUT;

out_runtime_pm_put:
	pm_runtime_mark_last_busy(radio->dev);
	pm_runtime_put_autosuspend(radio->dev);
	return err;
}

static int rda5807_vidioc_enum_freq_bands(struct file *file, void *priv,
					  struct v4l2_frequency_band *band)
{
	if (band->index >= ARRAY_SIZE(rda5807_bands))
		return -EINVAL;

	*band = rda5807_bands[band->index];
	return 0;
}

static const struct v4l2_ioctl_ops rda5807_ioctl_ops = {
	.vidioc_querycap	= rda5807_vidioc_querycap,
	.vidioc_g_audio		= rda5807_vidioc_g_audio,
	.vidioc_g_tuner		= rda5807_vidioc_g_tuner,
	.vidioc_g_frequency	= rda5807_vidioc_g_frequency,
	.vidioc_s_frequency	= rda5807_vidioc_s_frequency,
	.vidioc_s_hw_freq_seek  = rda5807_vidioc_s_hw_freq_seek,
	.vidioc_enum_freq_bands	= rda5807_vidioc_enum_freq_bands,
};

static const u16 rda5807_lna_current[] = { 1800, 2100, 2500, 3000 };

static int rda5807_setup(struct rda5807_driver *radio)
{
	struct device *dev = radio->dev;
	u16 lna = 0, iocfg = 0, ctrl = 0;
	u32 lna_current = 2500;
	size_t i;
	int err;

	/* Configure chip inputs. */

	if (device_property_read_bool(dev, "rda,lnan"))
		lna |= 0x1;
	if (device_property_read_bool(dev, "rda,lnap"))
		lna |= 0x2;
	if (!lna)
		dev_warn(dev, "Both LNA inputs disabled\n");

	device_property_read_u32(dev, "rda,lna-microamp", &lna_current);
	for (i = 0; i < ARRAY_SIZE(rda5807_lna_current); i++)
		if (rda5807_lna_current[i] == lna_current)
			break;
	if (i == ARRAY_SIZE(rda5807_lna_current)) {
		dev_err(dev, "Invalid LNA current value\n");
		return -EINVAL;
	}

	err = regmap_update_bits(radio->map, RDA5807_REG_INPUT,
				 RDA5807_INPUT_LNA_ICSEL | RDA5807_INPUT_LNA_PORT,
				 FIELD_PREP(RDA5807_INPUT_LNA_ICSEL, i) |
				 FIELD_PREP(RDA5807_INPUT_LNA_PORT, lna));
	if (err)
		return err;

	/* Configure chip outputs. */

	if (device_property_read_bool(dev, "rda,i2s-out"))
		iocfg |= RDA5807_IOCFG_I2S_EN;

	if (device_property_read_bool(dev, "rda,analog-out"))
		ctrl |= RDA5807_CTRL_DHIZ;

	err = regmap_write(radio->map, RDA5807_REG_IOCFG, iocfg);
	if (err)
		return err;

	err = regmap_write(radio->map, RDA5807_REG_CTRL, ctrl);
	if (err)
		return err;

	return 0;
}

static int rda5807_enable_regulator(struct rda5807_driver *radio)
{
	int err;

	err = regulator_enable(radio->supply);
	if (err)
		return err;

	/* A little sleep is needed before the registers can be accessed */
	msleep(20);

	return 0;
}

static const struct v4l2_file_operations rda5807_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
};

static const struct regmap_range rda5807_no_write_ranges[] = {
	{ RDA5807_REG_CHIPID, RDA5807_REG_CHIPID },
	{ RDA5807_REG_SEEKRES, RDA5807_REG_SIGNAL },
};

static const struct regmap_access_table rda5807_write_table = {
	.no_ranges = rda5807_no_write_ranges,
	.n_no_ranges = ARRAY_SIZE(rda5807_no_write_ranges),
};

static const struct regmap_range rda5807_volatile_ranges[] = {
	{ RDA5807_REG_SEEKRES, RDA5807_REG_SIGNAL },
};


static const struct regmap_access_table rda5807_volatile_table = {
	.yes_ranges = rda5807_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(rda5807_volatile_ranges),
};

static const struct reg_default rda5807_reg_defaults[] = {
	{ RDA5807_REG_CHIPID, 0x5804 },
	{ RDA5807_REG_CTRL, 0x0 },
	{ RDA5807_REG_CHAN, 0x4fc0 },
	{ RDA5807_REG_IOCFG, 0x0400 },
	{ RDA5807_REG_INPUT, 0x888b },
	{ RDA5807_REG_BAND, 0x5ec6 },
};

static const struct regmap_config rda5807_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = RDA5807_REG_SIGNAL,

	.wr_table = &rda5807_write_table,
	.volatile_table = &rda5807_volatile_table,

	.reg_defaults = rda5807_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(rda5807_reg_defaults),

	.cache_type = REGCACHE_FLAT,
};

static int rda5807_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct rda5807_driver *radio;
	unsigned int chipid;
	int err;

	radio = devm_kzalloc(dev, sizeof(*radio), GFP_KERNEL);
	if (!radio)
		return -ENOMEM;

	radio->dev = dev;

	radio->map = devm_regmap_init_i2c(client, &rda5807_regmap_config);
	if (IS_ERR(radio->map)) {
		dev_err(dev, "Failed to create regmap\n");
		return PTR_ERR(radio->map);
	}

	radio->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(radio->supply)) {
		dev_err(dev, "Failed to get power supply\n");
		return PTR_ERR(radio->supply);
	}

	err = rda5807_enable_regulator(radio);
	if (err) {
		dev_err(dev, "Failed to enable regulator\n");
		return err;
	}

	/* Disable the regmap cache temporarily to force reading the chip ID */
	regcache_cache_bypass(radio->map, true);
	err = regmap_read(radio->map, RDA5807_REG_CHIPID, &chipid);
	regcache_cache_bypass(radio->map, false);

	regulator_disable(radio->supply);
	if (err < 0) {
		dev_err(dev, "Failed to read chip ID\n");
		return err;
	}

	if ((chipid & 0xFF00) != 0x5800) {
		dev_err(dev, "Chip ID mismatch: expected 58xx, got %04X\n",
			chipid);
		return -ENODEV;
	}

	dev_info(dev, "Found FM radio receiver\n");

	pm_runtime_set_autosuspend_delay(dev, RDA5807_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	/* Only use regmap cache until the chip is brought up */
	regcache_cache_only(radio->map, true);
	regcache_mark_dirty(radio->map);

	err = rda5807_setup(radio);
	if (err) {
		dev_err(dev, "Failed to setup registers\n");
		return err;
	}

	/* Initialize controls. */
	v4l2_ctrl_handler_init(&radio->ctrl_handler, 3);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_VOLUME, 0, 15, 1, 8);

	v4l2_ctrl_new_std_menu(&radio->ctrl_handler, &rda5807_ctrl_ops,
			       V4L2_CID_TUNE_DEEMPHASIS,
			       V4L2_DEEMPHASIS_75_uS,
			       BIT(V4L2_DEEMPHASIS_DISABLED),
			       V4L2_DEEMPHASIS_50_uS);
	err = radio->ctrl_handler.error;
	if (err) {
		dev_err(dev, "Failed to init controls handler\n");
		goto err_ctrl_free;
	}

	err = v4l2_device_register(dev, &radio->v4l2_dev);
	if (err < 0) {
		dev_err(dev, "Failed to register v4l2 device\n");
		goto err_ctrl_free;
	}

	radio->video_dev = (struct video_device) {
		.name = "RDA5807 FM receiver",
		.v4l2_dev = &radio->v4l2_dev,
		.ctrl_handler = &radio->ctrl_handler,
		.fops = &rda5807_fops,
		.ioctl_ops = &rda5807_ioctl_ops,
		.release = video_device_release_empty,
		.device_caps = V4L2_CAP_RADIO | V4L2_CAP_TUNER
					      | V4L2_CAP_HW_FREQ_SEEK,
	};

	i2c_set_clientdata(client, radio);
	video_set_drvdata(&radio->video_dev, radio);

	err = v4l2_ctrl_handler_setup(&radio->ctrl_handler);
	if (err < 0) {
		dev_err(dev, "Failed to set default control values\n");
		goto err_video_unreg;
	}

	err = video_register_device(&radio->video_dev, VFL_TYPE_RADIO, -1);
	if (err < 0) {
		dev_err(dev, "Failed to register video device\n");
		goto err_ctrl_free;
	}

	return 0;

err_video_unreg:
	video_unregister_device(&radio->video_dev);
err_ctrl_free:
	v4l2_ctrl_handler_free(&radio->ctrl_handler);
	video_device_release_empty(&radio->video_dev);

	return err;
}

static int rda5807_i2c_remove(struct i2c_client *client)
{
	struct rda5807_driver *radio = i2c_get_clientdata(client);
	struct device *dev = &client->dev;

	pm_runtime_disable(dev);
	pm_runtime_force_suspend(dev);
	pm_runtime_dont_use_autosuspend(dev);

	video_unregister_device(&radio->video_dev);
	v4l2_ctrl_handler_free(&radio->ctrl_handler);
	video_device_release_empty(&radio->video_dev);

	return 0;
}

static int rda5807_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);
	int err;

	err = regmap_clear_bits(radio->map, RDA5807_REG_CTRL,
				RDA5807_CTRL_ENABLE);
	if (err)
		return err;

	regcache_cache_only(radio->map, true);
	regcache_mark_dirty(radio->map);

	err = regulator_disable(radio->supply);
	if (err)
		return err;

	dev_dbg(radio->dev, "Disabled\n");

	return 0;
}

static int rda5807_reset_chip(struct rda5807_driver *radio)
{
	int err;

	err = regmap_write_bits(radio->map, RDA5807_REG_CTRL,
				RDA5807_CTRL_SOFTRESET, RDA5807_CTRL_SOFTRESET);
	if (err)
		return err;

	usleep_range(1000, 10000);

	return regmap_write_bits(radio->map, RDA5807_REG_CTRL,
				 RDA5807_CTRL_SOFTRESET, 0);
}

static int rda5807_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);
	int err;

	err = rda5807_enable_regulator(radio);
	if (err)
		return err;

	regcache_cache_only(radio->map, false);

	err = rda5807_reset_chip(radio);
	if (err)
		return err;

	/* Restore cached registers to hardware */
	err = regcache_sync(radio->map);
	if (err) {
		dev_err(dev, "Failed to restore regs: %d\n", err);
		goto err_regulator_disable;
	}

	err = regmap_set_bits(radio->map, RDA5807_REG_CTRL,
			      RDA5807_CTRL_ENABLE);
	if (err) {
		dev_err(dev, "Failed to enable radio: %d\n", err);
		goto err_regulator_disable;
	}

	err = regmap_write_bits(radio->map, RDA5807_REG_CHAN,
				RDA5807_CHAN_TUNE, RDA5807_CHAN_TUNE);
	if (err) {
		dev_err(dev, "Failed to tune radio: %d\n", err);
		goto err_radio_disable;
	}

	dev_dbg(radio->dev, "Enabled\n");

	return 0;

err_radio_disable:
	regmap_clear_bits(radio->map, RDA5807_REG_CTRL,
			  RDA5807_CTRL_ENABLE);
err_regulator_disable:
	regulator_disable(radio->supply);
	return err;
}

static UNIVERSAL_DEV_PM_OPS(rda5807_pm_ops, rda5807_suspend, rda5807_resume, NULL);

static const struct of_device_id rda5807_dt_ids[] = {
	{ .compatible = "rda,rda5807" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rda5807_dt_ids);

static const struct i2c_device_id rda5807_id[] = {
	{ "rda5807", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, rda5807_id);

static struct i2c_driver rda5807_i2c_driver = {
	.driver = {
		.name = "radio-rda5807",
		.of_match_table = of_match_ptr(rda5807_dt_ids),
		.pm = &rda5807_pm_ops,
	},
	.probe = rda5807_i2c_probe,
	.remove = rda5807_i2c_remove,
	.id_table = rda5807_id,
};
module_i2c_driver(rda5807_i2c_driver);

MODULE_AUTHOR("Maarten ter Huurne <maarten@treewalker.org>");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("RDA5807 FM tuner driver");
MODULE_LICENSE("GPL");
