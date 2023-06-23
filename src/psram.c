/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_log.h"
#include "psram.h"

#define flash_offset	0x200000
#define TAG "RAM"

int psram_init() {
	return 0;
}
int psram_read(uint32_t addr, void *buf, int len) {
	// ESP_LOGI(TAG, "READ(addr: %lu, len: %d)", addr, len);
	esp_flash_read(NULL, buf, addr + flash_offset, len);
	return 0;
}
int psram_write(uint32_t addr, void *buf, int len) {
	// ESP_LOGI(TAG, "WRITE(addr: %lu, len: %d)", addr, len);
	esp_flash_write(NULL, buf, addr + flash_offset, len);
	return 0;
}