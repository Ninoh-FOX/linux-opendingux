// SPDX-License-Identifier: GPL-2.0-only

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_probe_helper.h>

#define ITE_VENDOR_ID			0xca00
#define IT6610_PRODUCT_ID		0x0611

#define HOTPLUG_DEBOUNCE_MS		100

#define REG_VENDOR_ID(x)		(0x00 + (x))
#define REG_DEVICE_ID(x)		(0x02 + (x))
#define REG_RESET			0x04
#define REG_CONTROL			0x05
#define REG_IRQ_STATUS(x)		(0x06 + (x))
#define REG_IRQ_MASK(x)			(0x09 + (x))
#define REG_IRQ_CLEAR(x)		(0x0c + (x))
#define REG_STATUS			0x0e
#define REG_BANK_SEL			0x0f

#define REG_PRODUCT_ID_PID_MASK		0xfff

enum it6610_state {
	UNKNOWN = 0,
	NO_RECEIVER,
	RECEIVER_READY,
	ACTIVE,
};

struct it6610 {
	struct drm_bridge		bridge;
	struct drm_connector		connector;

	struct device			*dev;
	struct regmap			*map;
	struct gpio_desc		*reset_gpio;
	struct regulator		*supply;

	enum it6610_state		state;
	struct delayed_work		work;
};

static inline struct it6610 *drm_bridge_to_it6610(struct drm_bridge *bridge)
{
	return container_of(bridge, struct it6610, bridge);
}

static inline struct it6610 *drm_conn_to_it6610(struct drm_connector *conn)
{
	return container_of(conn, struct it6610, connector);
}

static inline struct it6610 *work_to_it6610(struct work_struct *work)
{
	return container_of(work, struct it6610, work.work);
}

static irqreturn_t it6610_irq(int irq, void *data)
{
	struct it6610 *priv = data;
	u8 status[3];
	u8 clear[3] = { 0, 0, 0x0D };
	int err;

	/* Read status of all interrupts */
	err = regmap_bulk_read(priv->map, REG_IRQ_STATUS(0),
			       status, sizeof(status));
	if (err < 0) {
		dev_err(priv->dev, "Error reading interrupt status: %d\n", err);
		goto err_disable_irqs;
	}

	/* Handle active interrupts */
	if (status[0] & 0x01) {
		dev_dbg(priv->dev, "HPD interrupt\n");
		clear[0] |= 0x01;
	}
	if (status[0] & 0x02) {
		dev_dbg(priv->dev, "RxSEN interrupt\n");
		clear[0] |= 0x02;
	}
	if (status[2] & 0x10) {
		dev_dbg(priv->dev, "VidStable interrupt\n");
		clear[1] |= 0x40;
	}

	/* Wake up worker */
	mod_delayed_work(system_wq, &priv->work,
			 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	/* Clear interrupts */
	if (clear[0] || clear[1]) {
		err = regmap_bulk_write(priv->map, REG_IRQ_CLEAR(0),
					clear, sizeof(clear));
		if (err < 0) {
			dev_err(priv->dev, "Error clearing IRQ: %d\n", err);
			goto err_disable_irqs;
		}

		return IRQ_HANDLED;
	}

err_disable_irqs:
	/*
	 * If we cannot acknowledge the interrupt, do damage control by
	 * disabling the IRQ.
	 */
	dev_err(priv->dev, "Disabling IRQ\n");
	disable_irq(irq);
	return IRQ_NONE;
}

static void it6610_hw_reset(struct it6610 *priv)
{
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(300);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(10);
}

static int it6610_sw_reset(struct it6610 *priv)
{
	int ret;

	ret = regmap_write(priv->map, REG_RESET, 0x3C);
	if (ret < 0) {
		dev_err(priv->dev, "Error performing soft reset: %d\n", ret);
		return ret;
	}

	return 0;
}

static int it6610_read_ids(struct it6610 *priv)
{
	unsigned int vid, pid;
	u8 val_buf[4];
	int err;

	err = regmap_bulk_read(priv->map, REG_VENDOR_ID(0),
			       val_buf, sizeof(val_buf));
	if (err) {
		dev_err(priv->dev, "Unable to read VID: %d\n", err);
		return err;
	}

	vid = val_buf[0] | (val_buf[1] << 8);
	pid = val_buf[2] | ((val_buf[3] & 0x0F) << 8);

	if (vid != ITE_VENDOR_ID ||
	    (pid & REG_PRODUCT_ID_PID_MASK) != IT6610_PRODUCT_ID) {
		dev_err(priv->dev, "Unsupported i2c device\n");
		dev_err(priv->dev, "VID=0x%04x, PID=0x%04x\n", vid, pid);
		return -ENODEV;
	}

	return 0;
}

static int it6610_get_modes(struct drm_connector *conn)
{
	int err;

	/* No EDID, fallback on the XGA standard modes */
	err = drm_add_modes_noedid(conn, 1920, 1200);

	drm_add_modes_noedid(conn, 320, 240);

	/* And prefer a mode pretty much anything can handle */
	drm_set_preferred_mode(conn, 1280, 720);

	return err;
}

#if 0
static struct drm_encoder *it6610_best_encoder(struct drm_connector *conn)
{
	struct it6610 *priv = drm_conn_to_it6610(conn);

	return priv->bridge.encoder;
}
#endif

static const struct drm_connector_helper_funcs it6610_con_helper_funcs = {
	.get_modes		= it6610_get_modes,
#if 0
	.best_encoder		= it6610_best_encoder,
#endif
};

static enum drm_connector_status
it6610_connector_detect(struct drm_connector *conn, bool force)
{
	struct it6610 *priv = drm_conn_to_it6610(conn);
	unsigned int status;
	int err;

	err = regmap_read(priv->map, REG_STATUS, &status);
	if (err) {
		dev_err(priv->dev, "Unable to read connection status\n");
		return connector_status_unknown;
	}

	if (status & 0x60)
		return connector_status_connected;
	else
		return connector_status_disconnected;
}

static const struct drm_connector_funcs it6610_con_funcs = {
	.detect			= it6610_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int it6610_attach(struct drm_bridge *bridge,
			 enum drm_bridge_attach_flags flags)
{
	struct it6610 *priv = drm_bridge_to_it6610(bridge);
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	int err;

	if (!bridge->encoder) {
		dev_err(priv->dev, "Missing encoder\n");
		return -ENODEV;
	}

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		drm_connector_helper_add(&priv->connector,
					 &it6610_con_helper_funcs);

		err = drm_connector_init(bridge->dev, &priv->connector,
					 &it6610_con_funcs,
					 DRM_MODE_CONNECTOR_HDMIA);
		if (err) {
			dev_err(priv->dev, "Unable to init connector: %d\n",
				err);
			return err;
		}

		priv->connector.polled = DRM_CONNECTOR_POLL_HPD;

		drm_display_info_set_bus_formats(&priv->connector.display_info,
						 &bus_format, 1);

		drm_connector_attach_encoder(&priv->connector, bridge->encoder);
	}

	return 0;
}

static void it6610_enable(struct drm_bridge *bridge)
{
	struct it6610 *priv = drm_bridge_to_it6610(bridge);

	dev_warn(priv->dev, "HDMI enable\n");

	/* Enable video clock. */
	regmap_write(priv->map, REG_RESET, 0x15);

	/* Enable analog front end. */
	regmap_write(priv->map, 0x061, 0x00);

	/* Disable AV mute. */
	regmap_write(priv->map, 0x0C1, 0x00);
}

static void it6610_disable(struct drm_bridge *bridge)
{
	struct it6610 *priv = drm_bridge_to_it6610(bridge);

	dev_warn(priv->dev, "HDMI disable\n");

	/* Disable video clock. */
	regmap_write(priv->map, REG_RESET, 0x1D);

	/* Enable AV mute. */
	regmap_write(priv->map, 0x0C1, 0x01);

	/* Disable analog front end. */
	regmap_write(priv->map, 0x061, 0x10);
}

static void it6610_mode_set(struct drm_bridge *bridge,
			    const struct drm_display_mode *mode,
			    const struct drm_display_mode *adjusted_mode)
{
	struct it6610 *priv = drm_bridge_to_it6610(bridge);
	unsigned int vds, vde, hds, hde;

	/* Configure TMDS for clock rates below 80 MHz. */
	static const u8 tmds_val[4] = { 0x19, 0x03, 0x1E, 0x00 };
	u8 timing_val[0x10];

	vds = mode->vtotal - mode->vsync_start;
	vde = vds + mode->vdisplay;

	hds = mode->htotal - mode->hsync_start;
	hde = hds + mode->hdisplay;

	/*
	 * Regenerate DE from hsync and vsync.
	 * Although the DE signal we pass is usable, we have one extra display
	 * line in our setup as a trick to get interrupts at the right moment.
	 * That line should be omitted from the HDMI output to avoid problems
	 * with timings.
	 */

	timing_val[0x0] = 0x01;
	timing_val[0x1] = 0,
	timing_val[0x2] = (hds - 2) & 0x0FF,
	timing_val[0x3] = (hde - 2) & 0x0FF,
	timing_val[0x4] = ((hde - 2) & 0xF00) >> 4 | ((hds - 2) & 0xF00) >> 8,
	timing_val[0x5] = 0,
	timing_val[0x6] = 0,
	timing_val[0x7] = 0,
	timing_val[0x8] = 0,
	timing_val[0x9] = 0,
	timing_val[0xa] = (vds - 1) & 0x0FF,
	timing_val[0xb] = (vde - 1) & 0x0FF,
	timing_val[0xc] = ((vde - 1) & 0xF00) >> 4 | ((vds - 1) & 0xF00) >> 8,
	timing_val[0xd] = 0xFF,
	timing_val[0xe] = 0xFF,
	timing_val[0xf] = 0xFF,

	regmap_bulk_write(priv->map, 0x62, tmds_val, sizeof(tmds_val));
	regmap_bulk_write(priv->map, 0x90, timing_val, sizeof(timing_val));
}

static const struct drm_bridge_funcs it6610_bridge_funcs = {
	.attach			= it6610_attach,
	.enable			= it6610_enable,
	.disable		= it6610_disable,
	.mode_set		= it6610_mode_set,
};

static void it6610_work_func(struct work_struct *work)
{
	struct it6610 *priv = work_to_it6610(work);

	if (priv->bridge.dev)
		drm_kms_helper_hotplug_event(priv->bridge.dev);
}

static const struct drm_bridge_timings it6610_bridge_timings = {
	.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE
			 | DRM_BUS_FLAG_DE_HIGH,
	.setup_time_ps = 1200,
	.hold_time_ps = 1300,
};

static const struct regmap_range_cfg it6610_regmap_banks[] = {
	{
		.name = "it6610",
		.range_min = 0x00,
		.range_max = 0x1ff,
		.selector_reg = REG_BANK_SEL,
		.selector_mask = 0x1,
		.selector_shift = 0,
		.window_start = 0x00,
		.window_len = 0x100,
	},
};

static const struct regmap_config it6610_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0x1ff,
	.ranges		= it6610_regmap_banks,
	.num_ranges	= ARRAY_SIZE(it6610_regmap_banks),
};

static int it6610_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct it6610 *priv;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	i2c_set_clientdata(client, priv);

	INIT_DELAYED_WORK(&priv->work, it6610_work_func);

	priv->map = devm_regmap_init_i2c(client, &it6610_regmap_config);
	if (IS_ERR(priv->map)) {
		dev_err(dev, "Failed to init regmap\n");
		return PTR_ERR(priv->map);
	}

	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio)) {
		dev_err(dev, "Failed to get reset gpio\n");
		return PTR_ERR(priv->reset_gpio);
	}

	err = devm_request_threaded_irq(dev, client->irq, NULL, it6610_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"it6610-irq", priv);
	if (err) {
		dev_err(dev, "Failed to request irq\n");
		return err;
	}

	priv->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(priv->supply)) {
		dev_err(dev, "Failed to get power supply\n");
		return PTR_ERR(priv->supply);
	}

	err = regulator_enable(priv->supply);
	if (err) {
		dev_err(dev, "Failed to enable power supply\n");
		return err;
	}

	it6610_hw_reset(priv);

	err = it6610_read_ids(priv);
	if (err)
		goto err_regulator_disable;

	err = it6610_sw_reset(priv);
	if (err)
		goto err_regulator_disable;

	/* Unmask interrupts: hot plug, receiver sense, video stable. */

	err = regmap_write(priv->map, REG_IRQ_MASK(0), 0xFC);
	if (err) {
		dev_err(dev, "Unable to enable interrupt: %d\n", err);
		goto err_regulator_disable;
	}

	err = regmap_write(priv->map, REG_IRQ_MASK(2), 0xF7);
	if (err) {
		dev_err(dev, "Unable to enable interrupt: %d\n", err);
		goto err_regulator_disable;
	}

	/* Determine initial state. */
	schedule_delayed_work(&priv->work, 0);

	priv->bridge.funcs = &it6610_bridge_funcs;
	priv->bridge.of_node = dev->of_node;
	priv->bridge.timings = &it6610_bridge_timings;

	drm_bridge_add(&priv->bridge);

	dev_info(dev, "Probed\n");

	return 0;

err_regulator_disable:
	regulator_disable(priv->supply);
	return err;
}

static int it6610_remove(struct i2c_client *client)
{
	struct it6610 *priv = i2c_get_clientdata(client);

	regulator_disable(priv->supply);

	return 0;
}

static const struct of_device_id it6610_match[] = {
	{ .compatible = "ite,it6610" },
	{},
};
MODULE_DEVICE_TABLE(of, it6610_match);

static const struct i2c_device_id it6610_ids[] = {
	{ "it6610", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, it6610_ids);

static struct i2c_driver it6610_driver = {
	.probe_new = it6610_probe,
	.remove = it6610_remove,
	.driver = {
		.name = "it6610-hdmi",
		.of_match_table = it6610_match,
	},
	.id_table = it6610_ids,
};
module_i2c_driver(it6610_driver);

MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("ITE IT6610 HDMI bridge driver");
MODULE_LICENSE("GPL");
