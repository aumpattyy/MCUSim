/*
 * Copyright (c) 2017, 2018,
 * Dmitry Salychev <darkness.bsd@gmail.com>,
 * Alexander Salychev <ppsalex@rambler.ru> et al.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <time.h>
#include "mcusim/avr/sim/sim.h"
#include "mcusim/avr/sim/vcd_dump.h"

#define TERA			1000000000000.0
#define MAX_CLK_PRINTS		50

static void print_reg16(char *buf, uint32_t len, uint8_t hr, uint8_t lr);
static void print_reg(char *buf, unsigned int len, unsigned char r);
static void print_regbit(char *buf, unsigned int len, unsigned char r,
                         short bit);

FILE *MSIM_VCDOpenDump(void *vmcu, const char *dumpname)
{
	FILE *f;
	time_t timer;
	char buf[32];
	struct tm *tm_info;
	unsigned int i, regs;
	struct MSIM_AVR *mcu;
	struct MSIM_VCDRegister *reg;
	struct MSIM_VCDRegister *reg_low;
	uint8_t rh, rl;
	uint32_t reg_val;

	mcu = (struct MSIM_AVR *)vmcu;
	regs = sizeof mcu->vcdd->bit/sizeof mcu->vcdd->bit[0];

	f = fopen(dumpname, "w");
	if (!f) {
		return NULL;
	}

	time(&timer);
	tm_info = localtime(&timer);
	strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", tm_info);

	/* Printing VCD header */
	fprintf(f, "$date\n\t%s\n$end\n", buf);
	fprintf(f, "$version\n\tGenerated by MCUSim %s\n$end\n", MSIM_VERSION);
	fprintf(f, "$comment\n\tDump of a simulated %s\n$end\n", mcu->name);
	fprintf(f, "$timescale\n\t%lu ps\n$end\n",
	        (unsigned long)(((1.0/(double)mcu->freq)*TERA)/2.0));
	fprintf(f, "$scope\n\tmodule %s\n$end\n", mcu->name);

	/* Declare VCD variables to dump */
	fprintf(f, "$var reg 1 CLK_IO CLK_IO $end\n");
	for (i = 0; i < regs; i++) {
		if (mcu->vcdd->bit[i].regi < 0) {
			break;
		}
		reg = &mcu->vcdd->regs[mcu->vcdd->bit[i].regi];

		/* Are we going to dump a register bit only? */
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			fprintf(f, "$var reg 16 %s %s $end\n",
			        mcu->vcdd->bit[i].name,
			        mcu->vcdd->bit[i].name);
		} else if (mcu->vcdd->bit[i].n < 0) {
			fprintf(f, "$var reg 8 %s %s $end\n",
			        reg->name, reg->name);
		} else {
			fprintf(f, "$var reg 1 %s%d %s%d $end\n",
			        reg->name, mcu->vcdd->bit[i].n,
			        reg->name, mcu->vcdd->bit[i].n);
		}
	}
	fprintf(f, "$upscope $end\n");
	fprintf(f, "$enddefinitions $end\n");

	/* Dumping initial register values to VCD file */
	fprintf(f, "$dumpvars\n");
	fprintf(f, "b0 CLK_IO\n");
	for (i = 0; i < regs; i++) {
		if (mcu->vcdd->bit[i].regi < 0) {
			break;
		}

		reg = &mcu->vcdd->regs[mcu->vcdd->bit[i].regi];
		reg_val = *reg->addr;
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			reg_low = &mcu->vcdd->regs[mcu->vcdd->bit[i].reg_lowi];
			rh = *reg->addr;
			rl = *reg_low->addr;
			reg_val = ((uint16_t)(rh<<8)&0xFF00U)|
			          (uint16_t)(rl&0x00FFU);
		}
		if (!reg->addr) {
			fprintf(stderr, "[!]: Register with known address "
			        "(placed in data memory) can be dumped "
			        "only: regname=\"\"%16s\n", reg->name);
			continue;
		}

		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			print_reg16(buf, sizeof buf, rh, rl);
			fprintf(f, "b%s %s\n", buf, mcu->vcdd->bit[i].name);
		} else if (mcu->vcdd->bit[i].n < 0) {
			print_reg(buf, sizeof buf, *reg->addr);
			fprintf(f, "b%s %s\n", buf, reg->name);
		} else {
			print_regbit(buf, sizeof buf, *reg->addr,
			             mcu->vcdd->bit[i].n);
			fprintf(f, "b%s %s%d\n", buf, reg->name,
			        mcu->vcdd->bit[i].n);
		}
	}
	fprintf(f, "$end\n");

	return f;
}

void MSIM_VCDDumpFrame(FILE *f, void *vmcu, unsigned long tick,
                       unsigned char fall)
{
	static unsigned int clk_prints_left = 0;
	unsigned int i, regs;
	char buf[32], print_tick, new_value;
	struct MSIM_AVR *mcu;
	struct MSIM_VCDRegister *reg;
	struct MSIM_VCDRegister *reg_low;
	uint8_t rh, rl;
	uint32_t reg_val;

	mcu = (struct MSIM_AVR *)vmcu;
	regs = sizeof mcu->vcdd->bit/sizeof mcu->vcdd->bit[0];
	print_tick = 1;
	new_value = 0;

	/* Do we have at least one register which value has changed? */
	for (i = 0; i < regs; i++) {
		short n;		/* Bit index of a register */

		/* No register changes on fall should be */
		if (fall) {
			break;
		}
		/* First N registers to be dumped only */
		if (mcu->vcdd->bit[i].regi < 0) {
			break;
		}
		reg = &mcu->vcdd->regs[mcu->vcdd->bit[i].regi];
		reg_val = *reg->addr;
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			reg_low = &mcu->vcdd->regs[mcu->vcdd->bit[i].reg_lowi];
			rh = *reg->addr;
			rl = *reg_low->addr;
			reg_val = ((uint16_t)(rh<<8)&0xFF00U)|
			          (uint16_t)(rl&0x00FFU);
		}

		/* Has register value been changed? */
		n = mcu->vcdd->bit[i].n;
		if ((n < 0) && (reg_val != reg->oldv)) {
			new_value = 1;
			break;
		}
		if (n >= 0 && (((*reg->addr >> n)&1) !=
		                ((reg->oldv >> n)&1))) {
			new_value = 1;
			break;
		}
	}

	/* There is no register which value changed. Should we print a
	 * clock pulse in this case? */
	if (new_value == 0) {
		if (!clk_prints_left) {
			return;
		}
		fprintf(f, "#%lu\n", tick);
		fprintf(f, "b%d CLK_IO\n", !fall ? 1 : 0);
		if (fall) {
			clk_prints_left--;
		}
		return;
	}

	/* We've at least one register which value changed.
	 * Let's print it. */
	for (i = 0; i < regs; i++) {
		/* First N registers to be dumped only */
		if (mcu->vcdd->bit[i].regi < 0) {
			break;
		}

		reg = &mcu->vcdd->regs[mcu->vcdd->bit[i].regi];
		reg_val = *reg->addr;
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			reg_low = &mcu->vcdd->regs[mcu->vcdd->bit[i].reg_lowi];
			rh = *reg->addr;
			rl = *reg_low->addr;
			reg_val = ((uint16_t)(rh<<8)&0xFF00U)|
			          (uint16_t)(rl&0x00FFU);
		}
		/* Hasn't it been changed? */
		if (reg_val == reg->oldv) {
			continue;
		}

		/* Print current tick and main clock only once */
		if (print_tick) {
			print_tick = 0;
			fprintf(f, "#%lu\n", tick);
			fprintf(f, "b1 CLK_IO\n");
			clk_prints_left = MAX_CLK_PRINTS;
		}

		/* Print selected register */
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			print_reg16(buf, sizeof buf, rh, rl);
			fprintf(f, "b%s %s\n", buf, mcu->vcdd->bit[i].name);
		} else if (mcu->vcdd->bit[i].n < 0) {
			print_reg(buf, sizeof buf, *reg->addr);
			fprintf(f, "b%s %s\n", buf, reg->name);
		} else {
			print_regbit(buf, sizeof buf, *reg->addr,
			             mcu->vcdd->bit[i].n);
			fprintf(f, "b%s %s%d\n", buf, reg->name,
			        mcu->vcdd->bit[i].n);
		}
	}

	/* Update "old" values of the registers */
	for (i = 0; i < regs; i++) {
		/* First N registers to be updated only */
		if (mcu->vcdd->bit[i].regi < 0) {
			break;
		}

		reg = &mcu->vcdd->regs[mcu->vcdd->bit[i].regi];
		reg_val = *reg->addr;
		if (mcu->vcdd->bit[i].reg_lowi >= 0) {
			reg_low = &mcu->vcdd->regs[mcu->vcdd->bit[i].reg_lowi];
			rh = *reg->addr;
			rl = *reg_low->addr;
			reg_val = ((uint16_t)(rh<<8)&0xFF00U)|
			          (uint16_t)(rl&0x00FFU);
		}

		/* Hasn't it been changed? */
		if (reg_val == reg->oldv) {
			continue;
		} else {
			reg->oldv = reg_val;
		}
	}
}

static void print_reg16(char *buf, uint32_t len, uint8_t hr, uint8_t lr)
{
	uint32_t i;
	uint32_t j = 0;
	uint32_t reg;
	size_t rbits;

	j = 0;
	rbits = 16;
	reg = ((uint16_t)(hr<<8)&0xFF00U)|(uint16_t)(lr&0x00FFU);

	if (len < (rbits+1)) {
		buf[0] = 0;
		return;
	}
	for (i = 0; i < rbits; i++) {
		if ((reg >> (rbits-1-i)) & 1) {
			buf[j++] = '1';
		} else {
			buf[j++] = '0';
		}
	}
	buf[j] = 0;
}

static void print_reg(char *buf, unsigned int len, unsigned char r)
{
	uint32_t i;
	uint32_t j;
	size_t rbits;

	j = 0;
	rbits = 8;

	if (len < (rbits+1)) {
		buf[0] = 0;
		return;
	}
	for (i = 0; i < rbits; i++) {
		if ((r >> (rbits-1-i)) & 1) {
			buf[j++] = '1';
		} else {
			buf[j++] = '0';
		}
	}
	buf[j] = 0;
}

static void print_regbit(char *buf, unsigned int len, unsigned char r,
                         short bit)
{
	if (len < 2) {
		buf[0] = 0;
		return;
	}
	buf[0] = ((r >> bit) & 1) ? '1' : '0';
	buf[1] = 0;
}
