#ifndef RAM_H_
#define RAM_H_

int ram_init(void);
int ram_read(uint32_t addr, void *buf, int len);
int ram_write(uint32_t addr, void *buf, int len, bool end);
int ram_copyfromflash(uint32_t flashAddr, uint32_t len, uint32_t ramAddr);
int ram_poweroff();

#endif /* RAM_H */