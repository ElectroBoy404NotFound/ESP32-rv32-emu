#ifndef RAM_H_
#define RAM_H_

#include <string.h>
#include <unistd.h>
#include <stdbool.h>

int ram_init(void);
int ram_read(uint32_t addr, void *buf, int len);
int ram_write(uint32_t addr, void *buf, int len);
int ram_copyfromflash(uint32_t flashAddr, uint32_t len, uint32_t ramAddr);
int ram_copylinuxdisktoram(uint32_t ramAddr);
int ram_poweroff();

int inst_read(uint32_t addr, void *buf, int len);

#endif /* RAM_H */