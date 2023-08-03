/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * Use some code of mini-rv32ima.c from https://github.com/cnlohr/mini-rv32ima
 * Copyright 2022 Charles Lohr
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_timer.h"
// #include "hal/usb_serial_jtag_ll.h"

#include "cache.h"
#include "psram.h"

const char *TAG = "uc-rv32";
static uint32_t ram_amt = 16 * 1024 * 1024;

static uint64_t GetTimeMicroseconds();
static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy );
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);
static void MiniSleep();
static int IsKBHit();
static int ReadKBByte();

// This is the functionality we want to override in the emulator.
//  think of this as the way the emulator's processor is connected to the outside world.
#define MINIRV32WARN(x...) ESP_LOGE(TAG, x);
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) { if (retval > 0) {  retval = HandleException(ir, retval); } }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#define MINIRV32_CUSTOM_MEMORY_BUS
static void MINIRV32_STORE4(uint32_t ofs, uint32_t val)
{
	cache_write(ofs, &val, 4);
	// printf("st4: %lu from %lu\n", val, ofs);
}

static void MINIRV32_STORE2(uint32_t ofs, uint16_t val)
{
	cache_write(ofs, &val, 2);
	// printf("st2: %d from %lu\n", val, ofs);
}

static void MINIRV32_STORE1(uint32_t ofs, uint8_t val)
{
	cache_write(ofs, &val, 1);
	// printf("st1: %u from %lu\n", val, ofs);
}

static uint32_t MINIRV32_LOAD4(uint32_t ofs)
{
	uint32_t val;
	cache_read(ofs, &val, 4);
	// printf("ld4: %lu from %lu\n", val, ofs);
	return val;
}

static uint16_t MINIRV32_LOAD2(uint32_t ofs)
{
	uint16_t val;
	cache_read(ofs, &val, 2);
	// printf("ld2: %d from %lu\n", val, ofs);
	return val;
}

static uint8_t MINIRV32_LOAD1(uint32_t ofs)
{
	uint8_t val;
	cache_read(ofs, &val, 1);
	// printf("ld1: %u from %lu\n", val, ofs);
	return val;
}

uint32_t inst__load(uint32_t ofs) {
	uint32_t val;
	cache_read(ofs, &val, 4);
	// printf("Read inst: %lu from %lu\n", val, ofs);
	return val;
	
	// return 
}

#include "emulator.h"

static void DumpState(struct MiniRV32IMAState *core)
{
	unsigned int pc = core->pc;
	unsigned int *regs = (unsigned int *)core->regs;
	uint64_t thit, taccessed;

	cache_get_stat(&thit, &taccessed);
	ESP_LOGI(TAG, "hit: %llu accessed: %llu\n", thit, taccessed);
	ESP_LOGI(TAG, "PC: %08x ", pc);
	ESP_LOGI(TAG, "Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
		regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
		regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15] );
	ESP_LOGI(TAG, "a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
		regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23],
		regs[24], regs[25], regs[26], regs[27], regs[28], regs[29], regs[30], regs[31] );
}

static struct MiniRV32IMAState core;

#define dtb_start	0x3ff000
#define dtb_end		0x3ff5c0
#define kernel_start	0x200000
#define kernel_end	0x3c922c

static const unsigned char default64mbdtb[] = {
0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x05, 0x00,
0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0xe3, 0x00, 0x00, 0x04, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x02,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1b, 0x72, 0x69, 0x73, 0x63,
0x76, 0x2d, 0x6d, 0x69, 0x6e, 0x69, 0x6d, 0x61, 0x6c, 0x2d, 0x6e, 0x6f, 0x6d, 0x6d, 0x75, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x26, 0x72, 0x69, 0x73, 0x63,
0x76, 0x2d, 0x6d, 0x69, 0x6e, 0x69, 0x6d, 0x61, 0x6c, 0x2d, 0x6e, 0x6f, 0x6d, 0x6d, 0x75, 0x2c,
0x71, 0x65, 0x6d, 0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x63, 0x68, 0x6f, 0x73,
0x65, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0x2c,
0x65, 0x61, 0x72, 0x6c, 0x79, 0x63, 0x6f, 0x6e, 0x3d, 0x75, 0x61, 0x72, 0x74, 0x38, 0x32, 0x35,
0x30, 0x2c, 0x6d, 0x6d, 0x69, 0x6f, 0x2c, 0x30, 0x78, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
0x30, 0x2c, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x20, 0x63, 0x6f, 0x6e, 0x73, 0x6f, 0x6c,
0x65, 0x3d, 0x68, 0x76, 0x63, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x40, 0x38, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x35, 0x6d, 0x65, 0x6d, 0x6f,
0x72, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x41,
0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xc0, 0x00,
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x63, 0x70, 0x75, 0x73, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x45, 0x00, 0x0f, 0x42, 0x40,
0x00, 0x00, 0x00, 0x01, 0x63, 0x70, 0x75, 0x40, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x35, 0x63, 0x70, 0x75, 0x00, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x60, 0x6f, 0x6b, 0x61, 0x79, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1b, 0x72, 0x69, 0x73, 0x63,
0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x67,
0x72, 0x76, 0x33, 0x32, 0x69, 0x6d, 0x61, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b,
0x00, 0x00, 0x00, 0x71, 0x72, 0x69, 0x73, 0x63, 0x76, 0x2c, 0x6e, 0x6f, 0x6e, 0x65, 0x00, 0x00,
0x00, 0x00, 0x00, 0x01, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x72, 0x75, 0x70, 0x74, 0x2d, 0x63, 0x6f,
0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x6c, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0f,
0x00, 0x00, 0x00, 0x1b, 0x72, 0x69, 0x73, 0x63, 0x76, 0x2c, 0x63, 0x70, 0x75, 0x2d, 0x69, 0x6e,
0x74, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x58,
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
0x63, 0x70, 0x75, 0x2d, 0x6d, 0x61, 0x70, 0x00, 0x00, 0x00, 0x00, 0x01, 0x63, 0x6c, 0x75, 0x73,
0x74, 0x65, 0x72, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x63, 0x6f, 0x72, 0x65,
0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xa0,
0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x73, 0x6f, 0x63, 0x00, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x1b, 0x73, 0x69, 0x6d, 0x70, 0x6c, 0x65, 0x2d, 0x62,
0x75, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa4,
0x00, 0x00, 0x00, 0x01, 0x75, 0x61, 0x72, 0x74, 0x40, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xab,
0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x41,
0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1b, 0x6e, 0x73, 0x31, 0x36,
0x38, 0x35, 0x30, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x70, 0x6f, 0x77, 0x65,
0x72, 0x6f, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
0x00, 0x00, 0x00, 0xbb, 0x00, 0x00, 0x55, 0x55, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
0x00, 0x00, 0x00, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10,
0x00, 0x00, 0x00, 0x1b, 0x73, 0x79, 0x73, 0x63, 0x6f, 0x6e, 0x2d, 0x70, 0x6f, 0x77, 0x65, 0x72,
0x6f, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x72, 0x65, 0x62, 0x6f,
0x6f, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xbb,
0x00, 0x00, 0x77, 0x77, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xc1,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xc8,
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x1b,
0x73, 0x79, 0x73, 0x63, 0x6f, 0x6e, 0x2d, 0x72, 0x65, 0x62, 0x6f, 0x6f, 0x74, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x73, 0x79, 0x73, 0x63, 0x6f, 0x6e, 0x40, 0x31,
0x31, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10,
0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x1b,
0x73, 0x79, 0x73, 0x63, 0x6f, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
0x63, 0x6c, 0x69, 0x6e, 0x74, 0x40, 0x31, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xcf, 0x00, 0x00, 0x00, 0x02,
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03,
0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x1b,
0x00, 0x00, 0x00, 0x1b, 0x73, 0x69, 0x66, 0x69, 0x76, 0x65, 0x2c, 0x63, 0x6c, 0x69, 0x6e, 0x74,
0x30, 0x00, 0x72, 0x69, 0x73, 0x63, 0x76, 0x2c, 0x63, 0x6c, 0x69, 0x6e, 0x74, 0x30, 0x00, 0x00,
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x09,
0x23, 0x61, 0x64, 0x64, 0x72, 0x65, 0x73, 0x73, 0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x23,
0x73, 0x69, 0x7a, 0x65, 0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x63, 0x6f, 0x6d, 0x70, 0x61,
0x74, 0x69, 0x62, 0x6c, 0x65, 0x00, 0x6d, 0x6f, 0x64, 0x65, 0x6c, 0x00, 0x62, 0x6f, 0x6f, 0x74,
0x61, 0x72, 0x67, 0x73, 0x00, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x5f, 0x74, 0x79, 0x70, 0x65,
0x00, 0x72, 0x65, 0x67, 0x00, 0x74, 0x69, 0x6d, 0x65, 0x62, 0x61, 0x73, 0x65, 0x2d, 0x66, 0x72,
0x65, 0x71, 0x75, 0x65, 0x6e, 0x63, 0x79, 0x00, 0x70, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x00,
0x73, 0x74, 0x61, 0x74, 0x75, 0x73, 0x00, 0x72, 0x69, 0x73, 0x63, 0x76, 0x2c, 0x69, 0x73, 0x61,
0x00, 0x6d, 0x6d, 0x75, 0x2d, 0x74, 0x79, 0x70, 0x65, 0x00, 0x23, 0x69, 0x6e, 0x74, 0x65, 0x72,
0x72, 0x75, 0x70, 0x74, 0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x69, 0x6e, 0x74, 0x65, 0x72,
0x72, 0x75, 0x70, 0x74, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x6c, 0x65, 0x72, 0x00,
0x63, 0x70, 0x75, 0x00, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x73, 0x00, 0x63, 0x6c, 0x6f, 0x63, 0x6b,
0x2d, 0x66, 0x72, 0x65, 0x71, 0x75, 0x65, 0x6e, 0x63, 0x79, 0x00, 0x76, 0x61, 0x6c, 0x75, 0x65,
0x00, 0x6f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x00, 0x72, 0x65, 0x67, 0x6d, 0x61, 0x70, 0x00, 0x69,
0x6e, 0x74, 0x65, 0x72, 0x72, 0x75, 0x70, 0x74, 0x73, 0x2d, 0x65, 0x78, 0x74, 0x65, 0x6e, 0x64,
0x65, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// void loadDataIntoRAM(const unsigned char *d, uint32_t addr, uint32_t size)
// {
//     while (size--)
//         accessPSRAM(addr++, 1, true, d++);
// }

void app_main(void)
{
	ESP_LOGI(TAG, "RAM init\n");

	int ret = ram_init();
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to init RAM! Status code: %d\n", ret);
		return;
	}

	ESP_LOGI(TAG, "RAM init successful\n");
restart:
	ram_copylinuxdisktoram(0);
	ESP_LOGI(TAG, "Loaded Kernel into RAM!");
	uint32_t dtb_ptr = ram_amt - sizeof(default64mbdtb);
    uint32_t validram = dtb_ptr;
    ram_write(dtb_ptr, default64mbdtb, sizeof(default64mbdtb));

	// ram_copyfromflash(dtb_start, (dtb_end - dtb_start), 8388608 - (dtb_end - dtb_start));

	uint32_t dtbRamValue = (validram >> 24) | (((validram >> 16) & 0xff) << 8) | (((validram >> 8) & 0xff) << 16) | ((validram & 0xff) << 24);
    MINIRV32_STORE4(dtb_ptr + 0x13c, dtbRamValue);

	ESP_LOGI(TAG, "Loaded Kernel Image and DTB into RAM! dtb_ptr: %lu, dtbRamValue: %lu", dtb_ptr, dtbRamValue);

	core.pc = MINIRV32_RAM_IMAGE_OFFSET;
	core.regs[10] = 0x00; //hart ID
	core.regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0;
	core.extraflags |= 3; // Machine-mode.
	// os_update_cpu_frequency(160);



	// Image is loaded.
	uint64_t lastTime = 0;
	uint32_t time_divisor = 1024;
	// uint32_t time_divisor = 1;
	int instrs_per_flip = 1024;
	ESP_LOGI(TAG, "RV32IMA starting\n");
	while (1) {
		int ret;
		uint64_t *this_ccount = ((uint64_t*)&core.cyclel);
		uint32_t elapsedUs = *this_ccount / time_divisor - lastTime;

		lastTime = elapsedUs;
		ret = MiniRV32IMAStep(&core, NULL, 0, elapsedUs, instrs_per_flip);
		switch (ret) {
		case 0:
			break;
		case 1:
			MiniSleep();
			*this_ccount += instrs_per_flip;
			ESP_LOGI(TAG, "Sleep...");
			break;
		case 3:
			ESP_LOGI(TAG, "Got return 3!");
			break;
		case 0x7777:
			goto restart;
		case 0x5555:
			ESP_LOGI(TAG, "POWEROFF@0x%"PRIu32"%"PRIu32"", core.cycleh, core.cyclel);
			return;
		default:
			ESP_LOGI(TAG, "Unknown failure (%d)! Restarting..", ret);
			goto restart;
			break;
		}
	}

	DumpState(&core);
	ram_poweroff();
}
//////////////////////////////////////////////////////////////////////////
// Platform-specific functionality
//////////////////////////////////////////////////////////////////////////
static void MiniSleep(void)
{
	vTaskDelay(pdMS_TO_TICKS(10));
}

static uint64_t GetTimeMicroseconds()
{
	return esp_timer_get_time();
}

char charin;

static int ReadKBByte(void)
{
	// uint8_t rxchar;
	// int rread;

	// rread = usb_serial_jtag_ll_read_rxfifo(&rxchar, 1);

	if (charin > 0 && charin < 255) {
		charin = 0;
		return charin;
	}else
		return -1;
}

static int IsKBHit(void)
{
	charin = getc(stdin);
	// printf("%d", charin);
    return charin > 0 && charin < 255;
	// return usb_serial_jtag_ll_rxfifo_data_available();
}

static uint32_t HandleException(uint32_t ir, uint32_t code)
{
	// Weird opcode emitted by duktape on exit.
	if (code == 3) {
		// Could handle other opcodes here.
	}
	return code;
}

static uint32_t HandleControlStore(uint32_t addy, uint32_t val)
{
	//UART 8250 / 16550 Data Buffer
	if (addy == 0x10000000) {
        // ESP_LOGI(TAG, "%s", (char*) &val);
		printf("%s", (char*)&val);
		// while (usb_serial_jtag_ll_write_txfifo((uint8_t *)&val, 1) < 1) ;
		// usb_serial_jtag_ll_txfifo_flush();
	}
	return 0;
}

static uint32_t HandleControlLoad(uint32_t addy)
{
	// Emulating a 8250 / 16550 UART
	if (addy == 0x10000005)
		return 0x60 | IsKBHit();
	else if (addy == 0x10000000 && IsKBHit())
		return ReadKBByte();
	return 0;
}

static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value)
{
	uint32_t ptrstart, ptrend;

	switch (csrno) {
	case 0x136:
		printf("%d", (int)value);
		break;
	case 0x137:
		printf("%08x", (int)value);
		break;
	case 0x138:
		//Print "string"
		ptrstart = value - MINIRV32_RAM_IMAGE_OFFSET;
		ptrend = ptrstart;
		if (ptrstart >= ram_amt)
			ESP_LOGE(TAG, "DEBUG PASSED INVALID PTR (%"PRIu32")\n", value);
		while (ptrend < ram_amt) {
			uint8_t c = MINIRV32_LOAD1(ptrend);
			if (c == 0)
				break;
			printf("%c", c);
			ptrend++;
		}
		break;
	case 0x139:
		printf("%c", (uint8_t) value);
		break;
	default:
		break;
	}
}

static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno)
{
	if (csrno == 0x140) {
		if (!IsKBHit())
			return -1;
		return ReadKBByte();
	}
	return 0;
}
