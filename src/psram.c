#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "psram.h"
#include "esp_flash.h"
#include <sys/stat.h>

#include "emulator.h"

sdmmc_card_t *card;
FILE* ramdisk;
FILE* linux;

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

int mode = -1;

int ram_init() {
	esp_err_t ret;
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = true,
		.max_files = 2,
		.allocation_unit_size = 16 * 1024
	};
	const char mount_point[] = "/sd";
	ESP_LOGI("RAM", "Initializing SD card...");
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.max_freq_khz = 80*1000*1000;
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = PIN_NUM_MOSI,
		.miso_io_num = PIN_NUM_MISO,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 32768,
	};
	ret = spi_bus_initialize(host.slot, &bus_cfg, 1);
	if (ret != ESP_OK) {
		ESP_LOGE("RAM", "Failed to initialize bus.");
		return -1;
	}
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = PIN_NUM_CS;
	slot_config.host_id = host.slot;
	// slot_config.clock_speed_hz = 80*1000*1000;

	ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE("RAM", "Failed to mount filesystem. "
					 "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		} else {
			ESP_LOGE("RAM", "Failed to initialize the card (%s). "
					 "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
		}
		return -1;
	}

	if(ramdisk != NULL) fclose(ramdisk);
	ramdisk = fopen("/sd/ramdisk.bin", "w+");
	if (ramdisk == NULL) {
		ESP_LOGE("RAM", "Failed to open file as Read-Write!");
		while(1);
	}

	if(linux != NULL) fclose(linux);
	linux = fopen("/sd/Linux", "r");
	if (linux == NULL) {
		ESP_LOGE("RAM", "Failed to open linux file as Read!");
		while(1);
	}

	return 0;
}

int inst_read(uint32_t addr, void *buf, int len) {
	fseek(linux, addr, SEEK_SET);
	fread(buf, 1, len, linux);
	return 0;
}

int ram_read(uint32_t addr, void *buf, int len) {
	fseek(ramdisk, addr, SEEK_SET);
	fread(buf, 1, len, ramdisk);
	return 0;
}

int ram_copylinuxdisktoram(uint32_t ramAddr) {
	// len = linux->_offset;
	struct stat sbuf;
	fstat(fileno(linux), &sbuf);
	ESP_LOGI("RAM", "File size: %lu", sbuf.st_size);
	uint32_t len = sbuf.st_size;
	char dat[512];
	for(uint32_t i = 0; i < len; i += 512) {
		inst_read(i, &dat, 512);
		ram_write(i, &dat, 512, false);
	}
	fclose(linux);
	return 0;
}

int ram_write(uint32_t addr, void *buf, int len, bool end) {
	fseek(ramdisk, addr, SEEK_SET);
	fwrite(buf, 1, len, ramdisk);
	fflush(ramdisk);
	return 0;
}

int ram_copyfromflash(uint32_t flashAddr, uint32_t len, uint32_t ramAddr) {
	char dmabuf[64];
	for(int i = 0; i < len; i += 64) {
		esp_flash_read(NULL, dmabuf, flashAddr + i, 64);
		ram_write(ramAddr + i, dmabuf, 64, false);
	}
	return 0;
}

int ram_poweroff() {
    ESP_LOGI("RAM", "Ram POWER OFF");
	fclose(ramdisk);
	// fclose(linux);
	return 0;
}