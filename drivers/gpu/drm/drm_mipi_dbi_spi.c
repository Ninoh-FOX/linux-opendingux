// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIPI Display Bus Interface (DBI) SPI support
 *
 * Copyright 2016 Noralf Trønnes
 * Copyright 2020 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_mipi_dsi.h>

#include <video/mipi_display.h>

struct dbi_spi {
	struct mipi_dsi_host host;
	struct mipi_dsi_host_ops host_ops;

	struct spi_device *spi;
	struct gpio_desc *dc;
	struct mutex cmdlock;

	unsigned int current_bus_type;

	/**
	 * @tx_buf9: Buffer used for Option 1 9-bit conversion
	 */
	void *tx_buf9;

	/**
	 * @tx_buf9_len: Size of tx_buf9.
	 */
	size_t tx_buf9_len;
};

static inline struct dbi_spi *host_to_dbi_spi(struct mipi_dsi_host *host)
{
	return container_of(host, struct dbi_spi, host);
}

static ssize_t dbi_spi1_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct dbi_spi *dbi = host_to_dbi_spi(host);
	struct spi_device *spi = dbi->spi;
	struct spi_transfer tr = {
		.bits_per_word = 9,
	};
	const u8 *src8 = msg->tx_buf;
	struct spi_message m;
	size_t max_chunk, chunk;
	size_t len = msg->tx_len;
	bool cmd_byte = true;
	unsigned int i;
	u16 *dst16;
	int ret;

	tr.speed_hz = mipi_dbi_spi_cmd_max_speed(spi, len);
	dst16 = dbi->tx_buf9;

	max_chunk = min(dbi->tx_buf9_len / 2, len);

	spi_message_init_with_transfers(&m, &tr, 1);
	tr.tx_buf = dst16;

	while (len) {
		chunk = min(len, max_chunk);

		for (i = 0; i < chunk; i++) {
			dst16[i] = *src8++;

			/* Bit 8: 0 means command, 1 means data */
			if (!cmd_byte)
				dst16[i] |= BIT(8);

			cmd_byte = false;
		}

		tr.len = chunk * 2;
		len -= chunk;

		ret = spi_sync(spi, &m);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t dbi_spi3_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct dbi_spi *dbi = host_to_dbi_spi(host);
	struct spi_device *spi = dbi->spi;
	const u8 *buf = msg->tx_buf;
	unsigned int bpw = 8;
	u32 speed_hz;
	ssize_t ret;

	/* for now we only support sending messages, not receiving */
	if (msg->rx_len)
		return -EINVAL;

	gpiod_set_value_cansleep(dbi->dc, 0);

	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, 1);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, buf, 1);
	if (ret || msg->tx_len == 1)
		return ret;

	if (buf[0] == MIPI_DCS_WRITE_MEMORY_START)
		bpw = 16;

	gpiod_set_value_cansleep(dbi->dc, 1);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, msg->tx_len - 1);

	ret = mipi_dbi_spi_transfer(spi, speed_hz, bpw,
				    &buf[1], msg->tx_len - 1);
	if (ret)
		return ret;

	return msg->tx_len;
}

static ssize_t dbi_spi_transfer(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct dbi_spi *dbi = host_to_dbi_spi(host);

	switch (dbi->current_bus_type) {
	case MIPI_DCS_BUS_TYPE_DBI_SPI_C1:
		return dbi_spi1_transfer(host, msg);
	case MIPI_DCS_BUS_TYPE_DBI_SPI_C3:
		return dbi_spi3_transfer(host, msg);
	default:
		dev_err(&dbi->spi->dev, "Unknown transfer type\n");
		return -EINVAL;
	}
}

static int dbi_spi_attach(struct mipi_dsi_host *host,
			  struct mipi_dsi_device *dsi)
{
	struct dbi_spi *dbi = host_to_dbi_spi(host);

	dbi->current_bus_type = dsi->bus_type;

	if (dbi->current_bus_type == MIPI_DCS_BUS_TYPE_DBI_SPI_C1) {
		dbi->tx_buf9_len = SZ_16K;

		dbi->tx_buf9 = kmalloc(dbi->tx_buf9_len, GFP_KERNEL);
		if (!dbi->tx_buf9)
			return -ENOMEM;
	}

	return 0;
}

static int dbi_spi_detach(struct mipi_dsi_host *host,
			  struct mipi_dsi_device *dsi)
{
	struct dbi_spi *dbi = host_to_dbi_spi(host);

	kfree(dbi->tx_buf9);
	dbi->tx_buf9_len = 0;

	return 0; /* Nothing to do */
}

static void dbi_spi_host_unregister(void *d)
{
	mipi_dsi_host_unregister(d);
}

int drm_mipi_dbi_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dsi_device_info info = { };
	struct mipi_dsi_device *dsi;
	struct dbi_spi *dbi;
	int ret;

	dbi = devm_kzalloc(dev, sizeof(*dbi), GFP_KERNEL);
	if (!dbi)
		return -ENOMEM;

	dbi->host.dev = dev;
	dbi->host.ops = &dbi->host_ops;
	dbi->spi = spi;
	spi_set_drvdata(spi, dbi);

	dbi->dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dbi->dc)) {
		dev_err(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dbi->dc);
	}

	if (spi_is_bpw_supported(spi, 9))
		dbi->host.bus_types |= MIPI_DCS_BUS_TYPE_DBI_SPI_C1;
	if (dbi->dc)
		dbi->host.bus_types |= MIPI_DCS_BUS_TYPE_DBI_SPI_C3;

	if (!dbi->host.bus_types) {
		dev_err(dev, "Neither Type1 nor Type3 are supported\n");
		return -EINVAL;
	}

	dbi->host_ops.transfer = dbi_spi_transfer;
	dbi->host_ops.attach = dbi_spi_attach;
	dbi->host_ops.detach = dbi_spi_detach;

	mutex_init(&dbi->cmdlock);

	ret = mipi_dsi_host_register(&dbi->host);
	if (ret) {
		dev_err(dev, "Unable to register DSI host\n");
		return ret;
	}

	ret = devm_add_action_or_reset(dev, dbi_spi_host_unregister,
				       &dbi->host);
	if (ret)
		return ret;

	/*
	 * Register our own node as a MIPI DSI device.
	 * This ensures that the panel driver will be probed.
	 */
	info.channel = 0;
	info.node = of_node_get(dev->of_node);

	dsi = mipi_dsi_device_register_full(&dbi->host, &info);
	if (IS_ERR(dsi)) {
		dev_err(dev, "Failed to add DSI device\n");
		return PTR_ERR(dsi);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(drm_mipi_dbi_spi_probe);

MODULE_DESCRIPTION("DBI SPI bus driver");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_LICENSE("GPL");
