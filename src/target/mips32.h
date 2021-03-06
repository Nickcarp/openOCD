/***************************************************************************
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by David T.L. Wong                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef MIPS32_H
#define MIPS32_H

#include "target.h"
#include "mips32_pracc.h"

#define MIPS32_COMMON_MAGIC		0xB320B320

/* offsets into mips32 core register cache */
enum
{
	MIPS32_PC = 37,
	MIPS32NUMCOREREGS
};

enum mips32_isa_mode
{
	MIPS32_ISA_MIPS32 = 0,
	MIPS32_ISA_MIPS16E = 1,
};

struct mips32_comparator
{
	int used;
	uint32_t bp_value;
	uint32_t reg_address;
};

struct mips32_common
{
	uint32_t common_magic;
	void *arch_info;
	struct reg_cache *core_cache;
	struct mips_ejtag ejtag_info;
	uint32_t core_regs[MIPS32NUMCOREREGS];
	enum mips32_isa_mode isa_mode;

	/* working area for fastdata access */
	struct working_area *fast_data_area;

	int bp_scanned;
	int num_inst_bpoints;
	int num_data_bpoints;
	int num_inst_bpoints_avail;
	int num_data_bpoints_avail;
	struct mips32_comparator *inst_break_list;
	struct mips32_comparator *data_break_list;

	/* register cache to processor synchronization */
	int (*read_core_reg)(struct target *target, int num);
	int (*write_core_reg)(struct target *target, int num);
};

static inline struct mips32_common *
target_to_mips32(struct target *target)
{
	return target->arch_info;
}

struct mips32_core_reg
{
	uint32_t num;
	struct target *target;
	struct mips32_common *mips32_common;
};

struct mips32_algorithm
{
	int common_magic;
	enum mips32_isa_mode isa_mode;
};

#define MIPS32_OP_BEQ	0x04
#define MIPS32_OP_BNE	0x05
#define MIPS32_OP_ADDI	0x08
#define MIPS32_OP_AND	0x24
#define MIPS32_OP_COP0	0x10
#define MIPS32_OP_JR	0x08
#define MIPS32_OP_LUI	0x0F
#define MIPS32_OP_LW	0x23
#define MIPS32_OP_LBU	0x24
#define MIPS32_OP_LHU	0x25
#define MIPS32_OP_MFHI	0x10
#define MIPS32_OP_MTHI	0x11
#define MIPS32_OP_MFLO	0x12
#define MIPS32_OP_MTLO	0x13
#define MIPS32_OP_SB	0x28
#define MIPS32_OP_SH	0x29
#define MIPS32_OP_SW	0x2B
#define MIPS32_OP_ORI	0x0D
#define MIPS32_OP_XOR	0x26
#define MIPS32_OP_SRL	0x03

#define MIPS32_COP0_MF	0x00
#define MIPS32_COP0_MT	0x04

#define MIPS32_R_INST(opcode, rs, rt, rd, shamt, funct)	(((opcode) << 26) |((rs) << 21) | ((rt) << 16) | ((rd) << 11)| ((shamt) << 6) | (funct))
#define MIPS32_I_INST(opcode, rs, rt, immd)	(((opcode) << 26) |((rs) << 21) | ((rt) << 16) | (immd))
#define MIPS32_J_INST(opcode, addr)	(((opcode) << 26) |(addr))

#define MIPS32_NOP					0
#define MIPS32_ADDI(tar, src, val)	MIPS32_I_INST(MIPS32_OP_ADDI, src, tar, val)
#define MIPS32_AND(reg, off, val)	MIPS32_R_INST(0, off, val, reg, 0, MIPS32_OP_AND)
#define MIPS32_B(off)				MIPS32_BEQ(0, 0, off)
#define MIPS32_BEQ(src,tar,off)		MIPS32_I_INST(MIPS32_OP_BEQ, src, tar, off)
#define MIPS32_BNE(src,tar,off)		MIPS32_I_INST(MIPS32_OP_BNE, src, tar, off)
#define MIPS32_JR(reg)				MIPS32_R_INST(0, reg, 0, 0, 0, MIPS32_OP_JR)
#define MIPS32_MFC0(gpr, cpr, sel)	MIPS32_R_INST(MIPS32_OP_COP0, MIPS32_COP0_MF, gpr, cpr, 0, sel)
#define MIPS32_MTC0(gpr,cpr, sel)	MIPS32_R_INST(MIPS32_OP_COP0, MIPS32_COP0_MT, gpr, cpr, 0, sel)
#define MIPS32_LBU(reg, off, base)	MIPS32_I_INST(MIPS32_OP_LBU, base, reg, off)
#define MIPS32_LHU(reg, off, base)	MIPS32_I_INST(MIPS32_OP_LHU, base, reg, off)
#define MIPS32_LUI(reg, val)		MIPS32_I_INST(MIPS32_OP_LUI, 0, reg, val)
#define MIPS32_LW(reg, off, base)	MIPS32_I_INST(MIPS32_OP_LW, base, reg, off)
#define MIPS32_MFLO(reg)			MIPS32_R_INST(0, 0, 0, reg, 0, MIPS32_OP_MFLO)
#define MIPS32_MFHI(reg)			MIPS32_R_INST(0, 0, 0, reg, 0, MIPS32_OP_MFHI)
#define MIPS32_MTLO(reg)			MIPS32_R_INST(0, reg, 0, 0, 0, MIPS32_OP_MTLO)
#define MIPS32_MTHI(reg)			MIPS32_R_INST(0, reg, 0, 0, 0, MIPS32_OP_MTHI)
#define MIPS32_ORI(tar, src, val)	MIPS32_I_INST(MIPS32_OP_ORI, src, tar, val)
#define MIPS32_SB(reg, off, base)	MIPS32_I_INST(MIPS32_OP_SB, base, reg, off)
#define MIPS32_SH(reg, off, base)	MIPS32_I_INST(MIPS32_OP_SH, base, reg, off)
#define MIPS32_SW(reg, off, base)	MIPS32_I_INST(MIPS32_OP_SW, base, reg, off)
#define MIPS32_XOR(reg, val1, val2)	MIPS32_R_INST(0, val1, val2, reg, 0, MIPS32_OP_XOR)
#define MIPS32_SRL(reg, src, off)	MIPS32_R_INST(0, 0, src, reg, off, MIPS32_OP_SRL)

/* ejtag specific instructions */
#define MIPS32_DRET					0x4200001F
#define MIPS32_SDBBP				0x7000003F
#define MIPS16_SDBBP				0xE801

int mips32_arch_state(struct target *target);

int mips32_init_arch_info(struct target *target,
		struct mips32_common *mips32, struct jtag_tap *tap);

int mips32_restore_context(struct target *target);
int mips32_save_context(struct target *target);

struct reg_cache *mips32_build_reg_cache(struct target *target);

int mips32_run_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		uint32_t entry_point, uint32_t exit_point,
		int timeout_ms, void *arch_info);

int mips32_configure_break_unit(struct target *target);

int mips32_enable_interrupts(struct target *target, int enable);

int mips32_examine(struct target *target);

int mips32_register_commands(struct command_context *cmd_ctx);

int mips32_get_gdb_reg_list(struct target *target,
		struct reg **reg_list[], int *reg_list_size);
int mips32_checksum_memory(struct target *target, uint32_t address,
		uint32_t count, uint32_t* checksum);
int mips32_blank_check_memory(struct target *target,
		uint32_t address, uint32_t count, uint32_t* blank);

#endif	/*MIPS32_H*/
