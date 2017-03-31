/*
 * mcusim - Interactive simulator for microcontrollers.
 * Copyright (C) 2017 Dmitry Salychev <darkness.bsd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "avr/sim/sim.h"
#include "avr/sim/bootloader.h"
#include "avr/sim/simcore.h"

static int decode_inst(struct avr *mcu, uint16_t inst);
static int is_inst32(uint16_t inst);

/*
 * AVR opcodes executors.
 */
static void exec_in_out(struct avr *mcu, uint16_t inst,
			uint8_t reg, uint8_t io_loc);
static void exec_cmp_immediate(struct avr *mcu, uint16_t inst);
static void exec_cmp_carry(struct avr *mcu, uint16_t inst);
static void exec_eor(struct avr *mcu, uint16_t inst);
static void exec_load_immediate(struct avr *mcu, uint16_t inst);
static void exec_rel_jump(struct avr *mcu, uint16_t inst);
static void exec_brne(struct avr *mcu, uint16_t inst);

static void exec_st_x(struct avr *mcu, uint16_t inst);
static void exec_st_y(struct avr *mcu, uint16_t inst);
static void exec_st_ydisp(struct avr *mcu, uint16_t inst);
static void exec_st_z(struct avr *mcu, uint16_t inst);
static void exec_st_zdisp(struct avr *mcu, uint16_t inst);
static void exec_st(struct avr *mcu, uint16_t inst,
		    uint8_t *addr_low, uint8_t *addr_high, uint8_t regr);

static void exec_rcall(struct avr *mcu, uint16_t inst);
static void exec_sts(struct avr *mcu, uint16_t inst);
static void exec_sts16(struct avr *mcu, uint16_t inst);
static void exec_ret(struct avr *mcu);
static void exec_ori(struct avr *mcu, uint16_t inst);
static void exec_sbi_cbi(struct avr *mcu, uint16_t inst, uint8_t set_bit);
static void exec_sbis_sbic(struct avr *mcu, uint16_t inst, uint8_t set_bit);
static void exec_push_pop(struct avr *mcu, uint16_t inst, uint8_t push);
static void exec_movw(struct avr *mcu, uint16_t inst);

static void exec_ld_x(struct avr *mcu, uint16_t inst);
static void exec_ld_y(struct avr *mcu, uint16_t inst);
static void exec_ld_ydisp(struct avr *mcu, uint16_t inst);
static void exec_ld_z(struct avr *mcu, uint16_t inst);
static void exec_ld_zdisp(struct avr *mcu, uint16_t inst);
static void exec_ld(struct avr *mcu, uint16_t inst,
		    uint8_t *addr_low, uint8_t *addr_high, uint8_t regd);
/*
 * END AVR opcodes executors.
 */

void simulate_avr(struct avr *mcu)
{
	uint16_t inst;

	while (1) {
		inst = mcu->prog_mem[mcu->pc];

		if (decode_inst(mcu, inst)) {
			fprintf(stderr, "Unknown instruction: 0x%X\n", inst);
			abort();
		}
	}
}

void sreg_update_flag(struct avr *mcu, enum avr_sreg_flag flag, uint8_t set_f)
{
	uint8_t v;

	if (!mcu) {
		fprintf(stderr, "MCU is null");
		return;
	}

	switch (flag) {
	case AVR_SREG_CARRY:
		v = 0x01;
		break;
	case AVR_SREG_ZERO:
		v = 0x02;
		break;
	case AVR_SREG_NEGATIVE:
		v = 0x04;
		break;
	case AVR_SREG_TWOSCOM_OF:
		v = 0x08;
		break;
	case AVR_SREG_SIGN:
		v = 0x10;
		break;
	case AVR_SREG_HALF_CARRY:
		v = 0x20;
		break;
	case AVR_SREG_BITCOPY_ST:
		v = 0x40;
		break;
	case AVR_SREG_GLOB_INT:
		v = 0x80;
		break;
	}

	if (set_f)
		*mcu->sreg |= v;
	else
		*mcu->sreg &= ~v;
}

uint8_t sreg_flag(struct avr *mcu, enum avr_sreg_flag flag)
{
	uint8_t v, pos;

	if (!mcu) {
		fprintf(stderr, "MCU is null");
		return UINT8_MAX;
	}

	switch (flag) {
	case AVR_SREG_CARRY:
		v = 0x01;
		pos = 0;
		break;
	case AVR_SREG_ZERO:
		v = 0x02;
		pos = 1;
		break;
	case AVR_SREG_NEGATIVE:
		v = 0x04;
		pos = 2;
		break;
	case AVR_SREG_TWOSCOM_OF:
		v = 0x08;
		pos = 3;
		break;
	case AVR_SREG_SIGN:
		v = 0x10;
		pos = 4;
		break;
	case AVR_SREG_HALF_CARRY:
		v = 0x20;
		pos = 5;
		break;
	case AVR_SREG_BITCOPY_ST:
		v = 0x40;
		pos = 6;
		break;
	case AVR_SREG_GLOB_INT:
		v = 0x80;
		pos = 7;
		break;
	}

	return (*mcu->sreg & v) >> pos;
}

void stack_push(struct avr *mcu, uint8_t val)
{
	uint16_t sp;

	sp = (uint16_t) ((*mcu->sp_low) | (*mcu->sp_high << 8));
	mcu->data_mem[sp--] = val;
	*mcu->sp_low = (uint8_t) (sp & 0xFF);
	*mcu->sp_high = (uint8_t) (sp >> 8);
}

uint8_t stack_pop(struct avr *mcu)
{
	uint16_t sp;
	uint8_t v;

	sp = (uint16_t) ((*mcu->sp_low) | (*mcu->sp_high << 8));
	v = mcu->data_mem[++sp];
	*mcu->sp_low = (uint8_t) (sp & 0xFF);
	*mcu->sp_high = (uint8_t) (sp >> 8);

	return v;
}

static int decode_inst(struct avr *mcu, uint16_t inst)
{
	switch (inst & 0xF000) {
	case 0x0000:
		switch (inst) {
		case 0x0000:	/* NOP – No Operation */
			mcu->pc++;
			break;
		default:
			switch (inst & 0xFC00) {
			case 0x0400:
				exec_cmp_carry(mcu, inst);
				goto exit;
			default:
				/* Do nothing here, keep going */
				break;
			}

			switch (inst & 0xFF00) {
			case 0x0100:
				exec_movw(mcu, inst);
				break;
			default:
				return -1;
			}
			break;
		}
		break;
	case 0x2000:
		switch (inst & 0xFC00) {
		case 0x2400:
			exec_eor(mcu, inst);
			break;
		default:
			return -1;
		}
		break;
	case 0x3000:
		exec_cmp_immediate(mcu, inst);
		break;
	case 0x6000:
		exec_ori(mcu, inst);
		break;
	case 0x8000:
		switch (inst & 0xD208) {
		case 0x8000:
			exec_ld_zdisp(mcu, inst);
			goto exit;
		case 0x8008:
			exec_ld_ydisp(mcu, inst);
			goto exit;
		case 0x8200:
			exec_st_zdisp(mcu, inst);
			goto exit;
		case 0x8208:
			exec_st_ydisp(mcu, inst);
			goto exit;
		}

		switch (inst & 0xFE0F) {
		case 0x8000:
			exec_ld_z(mcu, inst);
			break;
		case 0x8008:
			exec_ld_y(mcu, inst);
			break;
		case 0x8200:
			exec_st_z(mcu, inst);
			break;
		case 0x8208:
			exec_st_y(mcu, inst);
			break;
		default:
			return -1;
		}
		break;
	case 0x9000:
		if ((inst & 0xFF0F) == 0x9408) {
			/* ... */
			break;
		}

		switch (inst) {
		case 0x9508:
			exec_ret(mcu);
			break;
		default:
			switch (inst & 0xFE0F) {
			case 0x9001:
			case 0x9002:
				exec_ld_z(mcu, inst);
				break;
			case 0x9009:
			case 0x900A:
				exec_ld_y(mcu, inst);
				break;
			case 0x900C:
			case 0x900D:
			case 0x900E:
				exec_ld_x(mcu, inst);
				break;
			case 0x900F:
				exec_push_pop(mcu, inst, 0);
				break;
			case 0x9200:
				exec_sts(mcu, inst);
				break;
			case 0x9201:
			case 0x9202:
				exec_st_z(mcu, inst);
				break;
			case 0x9209:
			case 0x920A:
				exec_st_y(mcu, inst);
				break;
			case 0x920C:
			case 0x920D:
			case 0x920E:
				exec_st_x(mcu, inst);
				break;
			case 0x920F:
				exec_push_pop(mcu, inst, 1);
				break;
			default:
				switch (inst & 0xFF00) {
				case 0x9800:
					exec_sbi_cbi(mcu, inst, 0);
					break;
				case 0x9900:
					exec_sbis_sbic(mcu, inst, 0);
					break;
				case 0x9A00:
					exec_sbi_cbi(mcu, inst, 1);
					break;
				case 0x9B00:
					exec_sbis_sbic(mcu, inst, 1);
					break;
				default:
					return -1;
				}
			}
			break;
		}
		break;
	case 0xA000:
		switch (inst & 0xD208) {
		case 0x8000:
			exec_ld_zdisp(mcu, inst);
			break;
		case 0x8008:
			exec_ld_ydisp(mcu, inst);
			break;
		case 0x8200:
			exec_st_zdisp(mcu, inst);
			break;
		case 0x8208:
			exec_st_ydisp(mcu, inst);
			break;
		default:
			return -1;
		}
		break;
	case 0xB000:
		exec_in_out(mcu, inst,
			    (inst & 0x01F0) >> 4,
			    (inst & 0x0F) | ((inst & 0x0600) >> 5));
		break;
	case 0xC000:
		exec_rel_jump(mcu, inst);
		break;
	case 0xD000:
		exec_rcall(mcu, inst);
		break;
	case 0xE000:
		exec_load_immediate(mcu, inst);
		break;
	case 0xF000:
		switch (inst & 0xFC07) {
		case 0xF401:
			exec_brne(mcu, inst);
			break;
		default:
			return -1;
		}
		break;
	default:
		return -1;
	}
exit:
	return 0;
}

static void exec_eor(struct avr *mcu, uint16_t inst)
{
	/*
	 * EOR - Exclusive OR
	 */
	uint8_t rd, rr;

	rd = (inst & 0x01F0) >> 4;
	rr = (inst & 0x0F) | ((inst & 0x0200) >> 5);

	mcu->data_mem[rd] = mcu->data_mem[rd] ^ mcu->data_mem[rr];
	mcu->pc++;

	sreg_update_flag(mcu, AVR_SREG_ZERO, !mcu->data_mem[rd]);
	sreg_update_flag(mcu, AVR_SREG_NEGATIVE, mcu->data_mem[rd] & 0x80);
	sreg_update_flag(mcu, AVR_SREG_TWOSCOM_OF, 0);
	sreg_update_flag(mcu, AVR_SREG_SIGN, (mcu->data_mem[rd] & 0x80) ^ 0);
}

static void exec_in_out(struct avr *mcu, uint16_t inst,
			uint8_t reg, uint8_t io_loc)
{
	switch (inst & 0xF800) {
	/*
	 * IN - Load an I/O Location to Register
	 */
	case 0xB000:
		mcu->data_mem[reg] = mcu->data_mem[io_loc + mcu->sfr_off];
		break;
	/*
	 * OUT – Store Register to I/O Location
	 */
	case 0xB800:
		mcu->data_mem[io_loc + mcu->sfr_off] = mcu->data_mem[reg];
		break;
	}
	mcu->pc++;
}

static void exec_cmp_immediate(struct avr *mcu, uint16_t inst)
{
	/*
	 * CPI – Compare with Immediate
	 */
	uint8_t rd, c, v, buf;

	rd = ((inst & 0xF0) >> 4) + 0x10;
	c = (inst & 0x0F) | ((inst & 0x0F00) >> 4);
	v = mcu->data_mem[rd] - c;
	mcu->pc++;

	buf = (~rd & c) | (c & v) | (v & ~rd);
	sreg_update_flag(mcu, AVR_SREG_CARRY, (buf >> 7) & 0x01);
	sreg_update_flag(mcu, AVR_SREG_ZERO, !v);
	sreg_update_flag(mcu, AVR_SREG_NEGATIVE, v & 0x80);
	sreg_update_flag(mcu, AVR_SREG_TWOSCOM_OF,
			 (((rd & ~c & ~v) | (~rd & c & v)) >> 7) & 1);
	sreg_update_flag(mcu, AVR_SREG_SIGN,
			 sreg_flag(mcu, AVR_SREG_NEGATIVE) ^
			 sreg_flag(mcu, AVR_SREG_TWOSCOM_OF));
	sreg_update_flag(mcu, AVR_SREG_HALF_CARRY, (buf >> 3) & 0x01);
}

static void exec_cmp_carry(struct avr *mcu, uint16_t inst)
{
	/*
	 * CPC – Compare with Carry
	 */
	uint8_t rd, rr, v, buf;

	rd = (inst & 0x01F0) >> 4;
	rr = (inst & 0x0F) | ((inst & 0x0200) >> 5);
	v = mcu->data_mem[rd] -
	    mcu->data_mem[rr] -
	    sreg_flag(mcu, AVR_SREG_CARRY);
	mcu->pc++;

	buf = (~rd & rr) | (rr & v) | (v & ~rd);
	sreg_update_flag(mcu, AVR_SREG_CARRY, (buf >> 7) & 0x01);
	sreg_update_flag(mcu, AVR_SREG_HALF_CARRY, (buf >> 3) & 0x01);

	sreg_update_flag(mcu, AVR_SREG_NEGATIVE, v & 0x80);
	sreg_update_flag(mcu, AVR_SREG_TWOSCOM_OF,
			 (((rd & ~rr & ~v) | (~rd & rr & v)) >> 7) & 1);
	sreg_update_flag(mcu, AVR_SREG_SIGN,
			 sreg_flag(mcu, AVR_SREG_NEGATIVE) ^
			 sreg_flag(mcu, AVR_SREG_TWOSCOM_OF));
	if (v)
		sreg_update_flag(mcu, AVR_SREG_ZERO, 0);
}

static void exec_load_immediate(struct avr *mcu, uint16_t inst)
{
	/*
	 * LDI – Load Immediate
	 */
	uint8_t rd_off, c;

	rd_off = (inst & 0xF0) >> 4;
	c = (inst & 0x0F) | ((inst & 0x0F00) >> 4);

	mcu->data_mem[0x10 + rd_off] = c;
	mcu->pc++;
}

static void exec_rel_jump(struct avr *mcu, uint16_t inst)
{
	/*
	 * RJMP - Relative Jump
	 */
	int16_t c;

	c = inst & 0x0FFF;
	if (c >= 2048)
		c -= 4096;
	mcu->pc += c + 1;
}

static void exec_brne(struct avr *mcu, uint16_t inst)
{
	/*
	 * BRNE – Branch if Not Equal
	 */
	int16_t c;

	if (!sreg_flag(mcu, AVR_SREG_ZERO)) {
		/*
		 * Z == 0, i.e. Rd != Rr
		 */
		c = (int16_t) ((int16_t) (inst << 6)) >> 9;
		mcu->pc += c + 1;
	} else {
		/*
		 * Z == 1, i.e. Rd == Rr
		 */
		mcu->pc++;
	}
}

static void exec_st_x(struct avr *mcu, uint16_t inst)
{
	/*
	 * ST – Store Indirect From Register to Data Space using Index X
	 */
	uint8_t regr, *x_low, *x_high;

	x_low = &mcu->data_mem[26];
	x_high = &mcu->data_mem[27];
	regr = (inst & 0x01F0) >> 4;
	exec_st(mcu, inst, x_low, x_high, regr);
}

static void exec_st_y(struct avr *mcu, uint16_t inst)
{
	/*
	 * ST – Store Indirect From Register to Data Space using Index Y
	 */
	uint8_t regr, *y_low, *y_high;

	y_low = &mcu->data_mem[28];
	y_high = &mcu->data_mem[29];
	regr = (inst & 0x01F0) >> 4;
	exec_st(mcu, inst, y_low, y_high, regr);
}

static void exec_st_z(struct avr *mcu, uint16_t inst)
{
	/*
	 * ST – Store Indirect From Register to Data Space using Index Z
	 */
	uint8_t regr, *z_low, *z_high;

	z_low = &mcu->data_mem[30];
	z_high = &mcu->data_mem[31];
	regr = (inst & 0x01F0) >> 4;
	exec_st(mcu, inst, z_low, z_high, regr);
}

static void exec_st(struct avr *mcu, uint16_t inst,
		    uint8_t *addr_low, uint8_t *addr_high, uint8_t regr)
{
	/*
	 * ST – Store Indirect From Register to Data Space
	 *	using Index X, Y or Z
	 */
	uint16_t addr = (uint16_t) *addr_low | (uint16_t) (*addr_high << 8);
	regr = (inst & 0x01F0) >> 4;

	switch (inst & 0x03) {
	case 0x02:	/*	X ← X-1, (X) ← Rr	X: Pre decremented */
		addr--;
		*addr_low = (uint8_t) (addr & 0xFF);
		*addr_high = (uint8_t) (addr >> 8);
	case 0x00:	/*	(X) ← Rr		X: Unchanged */
		mcu->data_mem[addr] = mcu->data_mem[regr];
		break;
	case 0x01:	/*	(X) ← Rr, X ← X+1	X: Post incremented */
		mcu->data_mem[addr] = mcu->data_mem[regr];
		addr++;
		*addr_low = (uint8_t) (addr & 0xFF);
		*addr_high = (uint8_t) (addr >> 8);
		break;
	}
	mcu->pc++;
}

static void exec_st_ydisp(struct avr *mcu, uint16_t inst)
{
	/*
	 * ST (STD) – Store Indirect From Register to Data Space using Index Y
	 */
	uint16_t addr;
	uint8_t regr, *y_low, *y_high, disp;

	y_low = &mcu->data_mem[28];
	y_high = &mcu->data_mem[29];
	addr = (uint16_t) *y_low | (uint16_t) (*y_high << 8);
	regr = (inst & 0x01F0) >> 4;
	disp = (inst & 0x07) |
	       ((inst & 0x0C00) >> 7) |
	       ((inst & 0x2000) >> 8);

	mcu->data_mem[addr + disp] = mcu->data_mem[regr];
	mcu->pc++;
}

static void exec_st_zdisp(struct avr *mcu, uint16_t inst)
{
	/*
	 * ST (STD) – Store Indirect From Register to Data Space using Index Z
	 */
	uint16_t addr;
	uint8_t regr, *z_low, *z_high, disp;

	z_low = &mcu->data_mem[30];
	z_high = &mcu->data_mem[31];
	addr = (uint16_t) *z_low | (uint16_t) (*z_high << 8);
	regr = (inst & 0x01F0) >> 4;
	disp = (inst & 0x07) |
	       ((inst & 0x0C00) >> 7) |
	       ((inst & 0x2000) >> 8);

	mcu->data_mem[addr + disp] = mcu->data_mem[regr];
	mcu->pc++;
}

static void exec_rcall(struct avr *mcu, uint16_t inst)
{
	/*
	 * RCALL – Relative Call to Subroutine
	 */
	int16_t c;
	uint32_t pc;

	pc = mcu->pc + 1;
	c = inst & 0x0FFF;
	if (c >= 2048)
		c -= 4096;
	stack_push(mcu, (uint8_t) (pc & 0xFF));
	stack_push(mcu, (uint8_t) ((pc >> 8) & 0xFF));
	mcu->pc += c + 1;
}

static void exec_sts(struct avr *mcu, uint16_t inst)
{
	/*
	 * STS – Store Direct to Data Space
	 */
	uint16_t addr;
	uint8_t rr;

	addr = mcu->prog_mem[mcu->pc + 1];
	rr = (inst & 0x01F0) >> 4;
	mcu->data_mem[addr] = mcu->data_mem[rr];
	mcu->pc += 2;
}

static void exec_ret(struct avr *mcu)
{
	/*
	 * RET – Return from Subroutine
	 */
	uint8_t ah, al;

	ah = stack_pop(mcu);
	al = stack_pop(mcu);
	mcu->pc = (ah << 8) | al;
}

static void exec_ori(struct avr *mcu, uint16_t inst)
{
	/*
	 * ORI – Logical OR with Immediate
	 */
	uint8_t rd_off, c, v;

	rd_off = (inst << 8) >> 12;
	c = (inst & 0x0F) | ((inst & 0x0F00) >> 4);
	mcu->data_mem[0x10 + rd_off] |= c;
	v = mcu->data_mem[0x10 + rd_off];
	mcu->pc++;

	sreg_update_flag(mcu, AVR_SREG_ZERO, !v);
	sreg_update_flag(mcu, AVR_SREG_NEGATIVE, v >> 7);
	sreg_update_flag(mcu, AVR_SREG_TWOSCOM_OF, 0);
	sreg_update_flag(mcu, AVR_SREG_SIGN,
			 sreg_flag(mcu, AVR_SREG_NEGATIVE) ^
			 sreg_flag(mcu, AVR_SREG_TWOSCOM_OF));
}

static void exec_sbi_cbi(struct avr *mcu, uint16_t inst, uint8_t set_bit)
{
	/*
	 * SBI – Set Bit in I/O Register
	 * CBI – Clear Bit in I/O Register
	 */
	uint8_t reg, b;

	reg = (inst & 0x00F8) >> 3;
	b = inst & 0x07;
	if (set_bit)
		mcu->data_mem[reg] |= (1 << b);
	else
		mcu->data_mem[reg] &= ~(1 << b);

	mcu->pc++;
}

static void exec_sbis_sbic(struct avr *mcu, uint16_t inst, uint8_t set_bit)
{
	/*
	 * SBIS – Skip if Bit in I/O Register is Set
	 * SBIC – Skip if Bit in I/O Register is Cleared
	 */
	uint8_t reg, b, pc_delta;

	reg = (inst & 0x00F8) >> 3;
	b = inst & 0x07;
	pc_delta = 1;
	if (set_bit) {
		if (mcu->data_mem[reg] & (1 << b)) {
			if (is_inst32(mcu->prog_mem[mcu->pc + 1]))
				pc_delta = 3;
			else
				pc_delta = 2;
		}
	} else {
		if (mcu->data_mem[reg] ^ (1 << b)) {
			if (is_inst32(mcu->prog_mem[mcu->pc + 1]))
				pc_delta = 3;
			else
				pc_delta = 2;
		}
	}
	mcu->pc += pc_delta;
}

static int is_inst32(uint16_t inst)
{
	uint16_t i = inst & 0xfc0f;
	return	/* STS */ i == 0x9200 ||
		/* LDS */ i == 0x9000 ||
		/* JMP */ i == 0x940c ||
		/* JMP */ i == 0x940d ||
		/* CALL */i == 0x940e ||
		/* CALL */i == 0x940f;
}

static void exec_push_pop(struct avr *mcu, uint16_t inst, uint8_t push)
{
	/*
	 * PUSH – Push Register on Stack
	 * POP – Pop Register from Stack
	 */
	uint8_t reg;

	reg = (inst >> 4) & 0x1F;
	if (push)
		stack_push(mcu, mcu->data_mem[reg]);
	else
		mcu->data_mem[reg] = stack_pop(mcu);

	mcu->pc++;
}

static void exec_movw(struct avr *mcu, uint16_t inst)
{
	/*
	 * MOVW – Copy Register Word
	 */
	uint8_t regd, regr;

	regr = inst & 0x0F;
	regd = (inst >> 4) & 0x0F;
	mcu->data_mem[regd+1] = mcu->data_mem[regr+1];
	mcu->data_mem[regd] = mcu->data_mem[regr];
	mcu->pc++;
}

static void exec_ld_x(struct avr *mcu, uint16_t inst)
{
	/*
	 * LD – Load Indirect from Data Space to Register using Index X
	 */
	uint8_t regd, *x_low, *x_high;

	x_low = &mcu->data_mem[26];
	x_high = &mcu->data_mem[27];
	regd = (inst & 0x01F0) >> 4;
	exec_ld(mcu, inst, x_low, x_high, regd);
}

static void exec_ld_y(struct avr *mcu, uint16_t inst)
{
	/*
	 * LD – Load Indirect from Data Space to Register using Index Y
	 */
	uint8_t regd, *y_low, *y_high;

	y_low = &mcu->data_mem[28];
	y_high = &mcu->data_mem[29];
	regd = (inst & 0x01F0) >> 4;
	exec_ld(mcu, inst, y_low, y_high, regd);
}

static void exec_ld_z(struct avr *mcu, uint16_t inst)
{
	/*
	 * LD – Load Indirect from Data Space to Register using Index Z
	 */
	uint8_t regd, *z_low, *z_high;

	z_low = &mcu->data_mem[30];
	z_high = &mcu->data_mem[31];
	regd = (inst & 0x01F0) >> 4;
	exec_ld(mcu, inst, z_low, z_high, regd);
}

static void exec_ld(struct avr *mcu, uint16_t inst,
		    uint8_t *addr_low, uint8_t *addr_high, uint8_t regd)
{
	/*
	 * LD – Load Indirect from Data Space to Register
	 *	using Index X, Y or Z
	 */
	uint16_t addr = (uint16_t) *addr_low | (uint16_t) (*addr_high << 8);

	switch (inst & 0x03) {
	case 0x02:	/*	X ← X-1, Rd ← (X)	X: Pre decremented */
		addr--;
		*addr_low = (uint8_t) (addr & 0xFF);
		*addr_high = (uint8_t) (addr >> 8);
	case 0x00:	/*	Rd ← (X)		X: Unchanged */
		mcu->data_mem[regd] = mcu->data_mem[addr];
		break;
	case 0x01:	/*	Rd ← (X), X ← X+1	X: Post incremented */
		mcu->data_mem[regd] = mcu->data_mem[addr];
		addr++;
		*addr_low = (uint8_t) (addr & 0xFF);
		*addr_high = (uint8_t) (addr >> 8);
		break;
	}
	mcu->pc++;
}

static void exec_ld_ydisp(struct avr *mcu, uint16_t inst)
{
	/*
	 * LD – Load Indirect from Data Space to Register using Index Y
	 */
	uint16_t addr;
	uint8_t regd, *y_low, *y_high, disp;

	y_low = &mcu->data_mem[28];
	y_high = &mcu->data_mem[29];
	addr = (uint16_t) *y_low | (uint16_t) (*y_high << 8);
	regd = (inst & 0x01F0) >> 4;
	disp = (inst & 0x07) |
	       ((inst & 0x0C00) >> 7) |
	       ((inst & 0x2000) >> 8);

	mcu->data_mem[regd] = mcu->data_mem[addr + disp];
	mcu->pc++;
}

static void exec_ld_zdisp(struct avr *mcu, uint16_t inst)
{
	/*
	 * LD – Load Indirect from Data Space to Register using Index Z
	 */
	uint16_t addr;
	uint8_t regd, *z_low, *z_high, disp;

	z_low = &mcu->data_mem[30];
	z_high = &mcu->data_mem[31];
	addr = (uint16_t) *z_low | (uint16_t) (*z_high << 8);
	regd = (inst & 0x01F0) >> 4;
	disp = (inst & 0x07) |
	       ((inst & 0x0C00) >> 7) |
	       ((inst & 0x2000) >> 8);

	mcu->data_mem[regd] = mcu->data_mem[addr + disp];
	mcu->pc++;
}
