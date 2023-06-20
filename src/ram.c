#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "ram.h"
#include "esp_flash.h"

#include "emulator.h"

sdmmc_card_t *card;
FILE* ramdisk;

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

int mode = 0;

int ram_init() {
	esp_err_t ret;
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = true,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024
	};
	const char mount_point[] = "/sd";
	ESP_LOGI(TAG, "Initializing SD card...");
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = PIN_NUM_MOSI,
		.miso_io_num = PIN_NUM_MISO,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	ret = spi_bus_initialize(host.slot, &bus_cfg, 1);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to initialize bus.");
		return -1;
	}
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = PIN_NUM_CS;
	slot_config.host_id = host.slot;

	ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount filesystem. "
					 "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		} else {
			ESP_LOGE(TAG, "Failed to initialize the card (%s). "
					 "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
		}
		return -1;
	}
	return 0;
}

int ram_read(uint32_t addr, void *buf, int len) {
	// if(mode != 2) {
	if(ramdisk != NULL) fclose(ramdisk);
	ramdisk = fopen("/sd/ramdisk.bin", "r+");
	if (ramdisk == NULL) {
		ESP_LOGE(TAG, "Failed to open file as Read-Write!");
		while(1);
	}
	// }
	fseek(ramdisk, addr, SEEK_SET);
	fread(buf, 1, len, ramdisk);
	// if(mode != 2)
	// 	mode = 2;
	fclose(ramdisk);
	return 0;
}

int ram_write(uint32_t addr, void *buf, int len, bool end) {
	if(mode != 1) {
		if(ramdisk != NULL) fclose(ramdisk);
		ramdisk = fopen("/sd/ramdisk.bin", "r+");
		if (ramdisk == NULL) {
			ESP_LOGE(TAG, "Failed to open file as Read-Write!");
			while(1);
		}
	}
	fseek(ramdisk, addr, SEEK_SET);
	fwrite(buf, 1, len, ramdisk);
	if(mode != 1)
		mode = 1;
	if(end) {
		fclose(ramdisk);
		mode = 0;
	}
	return 0;
}

int ram_copyfromflash(uint32_t flashAddr, uint32_t len, uint32_t ramAddr) {
	char dmabuf[64];
	for(int i = 0; i < len; i += 64) {
		esp_flash_read(NULL, dmabuf, flashAddr + i, 64);
		ram_write(ramAddr + i, dmabuf, 64, (i + 64 == len));
	}
	return 0;
}

int ram_poweroff() {
    ESP_LOGI(TAG, "Ram POWER OFF");
	fclose(ramdisk);
	return 0;
}
