/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 */

#ifndef __DRM_MIPI_DBI_SPI_H
#define __DRM_MIPI_DBI_SPI_H

#include <drm/drm_mipi_dsi.h>
#include <linux/spi/spi.h>

int drm_mipi_dbi_spi_probe(struct spi_device *spi);

#define module_mipi_dbi_spi_driver(__mipi_dbi_driver, __mipi_dbi_match_table) \
	static struct spi_driver __mipi_dbi_driver##_spi __maybe_unused = { \
		.driver = { \
			.name = #__mipi_dbi_driver "_spi", \
			.of_match_table = __mipi_dbi_match_table, \
		}, \
		.probe = drm_mipi_dbi_spi_probe, \
	}; \
	static int __mipi_dbi_driver##_module_init(void) \
	{ \
		if (IS_ENABLED(CONFIG_DRM_MIPI_DBI_SPI)) { \
			int err = spi_register_driver(&__mipi_dbi_driver##_spi); \
			if (err) \
				return err; \
		} \
		return mipi_dsi_driver_register(&__mipi_dbi_driver); \
	} \
	static void __mipi_dbi_driver##_module_exit(void) \
	{ \
		mipi_dsi_driver_unregister(&__mipi_dbi_driver); \
		if (IS_ENABLED(CONFIG_DRM_MIPI_DBI_SPI)) \
			spi_unregister_driver(&__mipi_dbi_driver##_spi); \
	} \
	module_init(__mipi_dbi_driver##_module_init); \
	module_exit(__mipi_dbi_driver##_module_exit)

#endif /* __DRM_MIPI_DBI_SPI_H */
