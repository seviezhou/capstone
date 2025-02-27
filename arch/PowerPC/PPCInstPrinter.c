//===-- PPCInstPrinter.cpp - Convert PPC MCInst to assembly syntax --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class prints an PPC MCInst to a .s file.
//
//===----------------------------------------------------------------------===//

/* Capstone Disassembly Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2013-2015 */

#ifdef CAPSTONE_HAS_POWERPC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PPCInstPrinter.h"
#include "PPCPredicates.h"
#include "../../MCInst.h"
#include "../../utils.h"
#include "../../SStream.h"
#include "../../MCRegisterInfo.h"
#include "../../MathExtras.h"
#include "PPCMapping.h"

#ifndef CAPSTONE_DIET
static const char *getRegisterName(unsigned RegNo);
#endif

static void printOperand(MCInst *MI, unsigned OpNo, SStream *O);
static void printInstruction(MCInst *MI, SStream *O);
static void printAbsBranchOperand(MCInst *MI, unsigned OpNo, SStream *O);
static char *printAliasInstr(MCInst *MI, SStream *OS, MCRegisterInfo *MRI);
static char *printAliasBcc(MCInst *MI, SStream *OS, void *info);
static void printCustomAliasOperand(MCInst *MI, unsigned OpIdx,
		unsigned PrintMethodIdx, SStream *OS);

#if 0
static void printRegName(SStream *OS, unsigned RegNo)
{
	char *RegName = getRegisterName(RegNo);

	if (RegName[0] == 'q' /* QPX */) {
		// The system toolchain on the BG/Q does not understand QPX register names
		// in .cfi_* directives, so print the name of the floating-point
		// subregister instead.
		RegName[0] = 'f';
	}

	SStream_concat0(OS, RegName);
}
#endif

static void set_mem_access(MCInst *MI, bool status)
{
	if (MI->csh->detail != CS_OPT_ON)
		return;

	MI->csh->doing_mem = status;

	if (status) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_MEM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.base = PPC_REG_INVALID;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.disp = 0;
	} else {
		// done, create the next operand slot
		MI->flat_insn->detail->ppc.op_count++;
	}
}

void PPC_post_printer(csh ud, cs_insn *insn, char *insn_asm, MCInst *mci)
{
	if (((cs_struct *)ud)->detail != CS_OPT_ON)
		return;

	// check if this insn has branch hint
	if (strrchr(insn->mnemonic, '+') != NULL && !strstr(insn_asm, ".+")) {
		insn->detail->ppc.bh = PPC_BH_PLUS;
	} else if (strrchr(insn->mnemonic, '-') != NULL) {
		insn->detail->ppc.bh = PPC_BH_MINUS;
	}

	if (strrchr(insn->mnemonic, '.') != NULL) {
		insn->detail->ppc.update_cr0 = true;
	}
}

#define GET_INSTRINFO_ENUM
#include "PPCGenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "PPCGenRegisterInfo.inc"

static void op_addBC(MCInst *MI, unsigned int bc)
{
	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.bc = (ppc_bc)bc;
	}
}

#define CREQ (0)
#define CRGT (1)
#define CRLT (2)
#define CRUN (3)

static int getBICRCond(int bi)
{
	return (bi - PPC_CR0EQ) >> 3;
}

static int getBICR(int bi)
{
	return ((bi - PPC_CR0EQ) & 7) + PPC_CR0;
}

static void op_addReg(MCInst *MI, unsigned int reg)
{
	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_REG;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].reg = reg;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void add_CRxx(MCInst *MI, ppc_reg reg)
{
	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_REG;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].reg = reg;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static char *printAliasBcc(MCInst *MI, SStream *OS, void *info)
{
#define GETREGCLASS_CONTAIN(_class, _reg) MCRegisterClass_contains(MCRegisterInfo_getRegClass(MRI, _class), MCOperand_getReg(MCInst_getOperand(MI, _reg)))
	SStream ss;
	const char *opCode;
	char *tmp, *AsmMnem, *AsmOps, *c;
	int OpIdx, PrintMethodIdx;
	int decCtr = false, needComma = false;
	MCRegisterInfo *MRI = (MCRegisterInfo *)info;

	SStream_Init(&ss);

	switch (MCInst_getOpcode(MI)) {
		default: return NULL;
		case PPC_gBC:
				 opCode = "b%s";
				 break;
		case PPC_gBCA:
				 opCode = "b%sa";
				 break;
		case PPC_gBCCTR:
				 opCode = "b%sctr";
				 break;
		case PPC_gBCCTRL:
				 opCode = "b%sctrl";
				 break;
		case PPC_gBCL:
				 opCode = "b%sl";
				 break;
		case PPC_gBCLA:
				 opCode = "b%sla";
				 break;
		case PPC_gBCLR:
				 opCode = "b%slr";
				 break;
		case PPC_gBCLRL:
				 opCode = "b%slrl";
				 break;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 0) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 1)) {
		SStream_concat(&ss, opCode, "dnzf");
		decCtr = true;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 2) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 3)) {
		SStream_concat(&ss, opCode, "dzf");
		decCtr = true;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 4) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 7) &&
			MCOperand_isReg(MCInst_getOperand(MI, 1)) &&
			GETREGCLASS_CONTAIN(PPC_CRBITRCRegClassID, 1)) {
		int cr = getBICRCond(MCOperand_getReg(MCInst_getOperand(MI, 1)));

		switch(cr) {
			case CREQ:
				SStream_concat(&ss, opCode, "ne");
				break;
			case CRGT:
				SStream_concat(&ss, opCode, "le");
				break;
			case CRLT:
				SStream_concat(&ss, opCode, "ge");
				break;
			case CRUN:
				SStream_concat(&ss, opCode, "ns");
				break;
		}

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 6)
			SStream_concat0(&ss, "-");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 7)
			SStream_concat0(&ss, "+");

		decCtr = false;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 8) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 9)) {
		SStream_concat(&ss, opCode, "dnzt");
		decCtr = true;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 10) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 11)) {
		SStream_concat(&ss, opCode, "dzt");
		decCtr = true;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) >= 12) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) <= 15) &&
			MCOperand_isReg(MCInst_getOperand(MI, 1)) &&
			GETREGCLASS_CONTAIN(PPC_CRBITRCRegClassID, 1)) {
		int cr = getBICRCond(MCOperand_getReg(MCInst_getOperand(MI, 1)));

		switch(cr) {
			case CREQ:
				SStream_concat(&ss, opCode, "eq");
				break;
			case CRGT:
				SStream_concat(&ss, opCode, "gt");
				break;
			case CRLT:
				SStream_concat(&ss, opCode, "lt");
				break;
			case CRUN:
				SStream_concat(&ss, opCode, "so");
				break;
		}

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 14)
			SStream_concat0(&ss, "-");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 15)
			SStream_concat0(&ss, "+");

		decCtr = false;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			((MCOperand_getImm(MCInst_getOperand(MI, 0)) & 0x12)== 16)) {
		SStream_concat(&ss, opCode, "dnz");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 24)
			SStream_concat0(&ss, "-");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 25)
			SStream_concat0(&ss, "+");

		needComma = false;
	}

	if (MCInst_getNumOperands(MI) == 3 &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			((MCOperand_getImm(MCInst_getOperand(MI, 0)) & 0x12)== 18)) {
		SStream_concat(&ss, opCode, "dz");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 26)
			SStream_concat0(&ss, "-");

		if (MCOperand_getImm(MCInst_getOperand(MI, 0)) == 27)
			SStream_concat0(&ss, "+");

		needComma = false;
	}

	if (MCOperand_isReg(MCInst_getOperand(MI, 1)) &&
			GETREGCLASS_CONTAIN(PPC_CRBITRCRegClassID, 1) &&
			MCOperand_isImm(MCInst_getOperand(MI, 0)) &&
			(MCOperand_getImm(MCInst_getOperand(MI, 0)) < 16)) {
		int cr = getBICR(MCOperand_getReg(MCInst_getOperand(MI, 1)));

		if (decCtr) {
			int cd;
			needComma = true;
			SStream_concat0(&ss, " ");

			if (cr > PPC_CR0) {
				SStream_concat(&ss, "4*cr%d+", cr - PPC_CR0);
			}

			cd = getBICRCond(MCOperand_getReg(MCInst_getOperand(MI, 1)));
			switch(cd) {
				case CREQ:
					SStream_concat0(&ss, "eq");
					if (cr <= PPC_CR0)
						add_CRxx(MI, PPC_REG_CR0EQ);
					op_addBC(MI, PPC_BC_EQ);
					break;
				case CRGT:
					SStream_concat0(&ss, "gt");
					if (cr <= PPC_CR0)
						add_CRxx(MI, PPC_REG_CR0GT);
					op_addBC(MI, PPC_BC_GT);
					break;
				case CRLT:
					SStream_concat0(&ss, "lt");
					if (cr <= PPC_CR0)
						add_CRxx(MI, PPC_REG_CR0LT);
					op_addBC(MI, PPC_BC_LT);
					break;
				case CRUN:
					SStream_concat0(&ss, "so");
					if (cr <= PPC_CR0)
						add_CRxx(MI, PPC_REG_CR0UN);
					op_addBC(MI, PPC_BC_SO);
					break;
			}

			if (cr > PPC_CR0) {
				if (MI->csh->detail) {
					MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_REG;
					MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].reg = MCOperand_getReg(MCInst_getOperand(MI, 1));
					MI->flat_insn->detail->ppc.op_count++;
				}
			}
		} else {
			if (cr > PPC_CR0) {
				needComma = true;
				SStream_concat(&ss, " cr%d", cr - PPC_CR0);
				op_addReg(MI, PPC_REG_CR0 + cr - PPC_CR0);
			}
		}
	}

	if (MCOperand_isImm(MCInst_getOperand(MI, 2)) &&
			MCOperand_getImm(MCInst_getOperand(MI, 2)) != 0) {
		if (needComma)
			SStream_concat0(&ss, ",");

		SStream_concat0(&ss, " $\xFF\x03\x01");
	}

	tmp = cs_strdup(ss.buffer);
	AsmMnem = tmp;
	for(AsmOps = tmp; *AsmOps; AsmOps++) {
		if (*AsmOps == ' ' || *AsmOps == '\t') {
			*AsmOps = '\0';
			AsmOps++;
			break;
		}
	}

	SStream_concat0(OS, AsmMnem);
	if (*AsmOps) {
		SStream_concat0(OS, "\t");
		for (c = AsmOps; *c; c++) {
			if (*c == '$') {
				c += 1;
				if (*c == (char)0xff) {
					c += 1;
					OpIdx = *c - 1;
					c += 1;
					PrintMethodIdx = *c - 1;
					printCustomAliasOperand(MI, OpIdx, PrintMethodIdx, OS);
				} else
					printOperand(MI, *c - 1, OS);
			} else {
				SStream_concat1(OS, *c);
			}
		}
	}

	return tmp;
}

static bool isBOCTRBranch(unsigned int op)
{
	return ((op >= PPC_BDNZ) && (op <= PPC_BDZp));
}

void PPC_printInst(MCInst *MI, SStream *O, void *Info)
{
	char *mnem;
	unsigned int opcode = MCInst_getOpcode(MI);

	// printf("opcode = %u\n", opcode);

	// Check for slwi/srwi mnemonics.
	if (opcode == PPC_RLWINM) {
		unsigned char SH = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 2));
		unsigned char MB = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 3));
		unsigned char ME = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 4));
		bool useSubstituteMnemonic = false;

		if (SH <= 31 && MB == 0 && ME == (31 - SH)) {
			SStream_concat0(O, "slwi\t");
			MCInst_setOpcodePub(MI, PPC_INS_SLWI);
			useSubstituteMnemonic = true;
		}

		if (SH <= 31 && MB == (32 - SH) && ME == 31) {
			SStream_concat0(O, "srwi\t");
			MCInst_setOpcodePub(MI, PPC_INS_SRWI);
			useSubstituteMnemonic = true;
			SH = 32 - SH;
		}

		if (useSubstituteMnemonic) {
			printOperand(MI, 0, O);
			SStream_concat0(O, ", ");
			printOperand(MI, 1, O);

			if (SH > HEX_THRESHOLD)
				SStream_concat(O, ", 0x%x", (unsigned int)SH);
			else
				SStream_concat(O, ", %u", (unsigned int)SH);

			if (MI->csh->detail) {
				cs_ppc *ppc = &MI->flat_insn->detail->ppc;

				ppc->operands[ppc->op_count].type = PPC_OP_IMM;
				ppc->operands[ppc->op_count].imm = SH;
				++ppc->op_count;
			}

			return;
		}
	}

	if ((opcode == PPC_OR || opcode == PPC_OR8) &&
			MCOperand_getReg(MCInst_getOperand(MI, 1)) == MCOperand_getReg(MCInst_getOperand(MI, 2))) {
		SStream_concat0(O, "mr\t");
		MCInst_setOpcodePub(MI, PPC_INS_MR);

		printOperand(MI, 0, O);
		SStream_concat0(O, ", ");
		printOperand(MI, 1, O);

		return;
	}

	if (opcode == PPC_RLDICR ||
			opcode == PPC_RLDICR_32) {
		unsigned char SH = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 2));
		unsigned char ME = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 3));

		// rldicr RA, RS, SH, 63-SH == sldi RA, RS, SH
		if (63 - SH == ME) {
			SStream_concat0(O, "sldi\t");
			MCInst_setOpcodePub(MI, PPC_INS_SLDI);

			printOperand(MI, 0, O);
			SStream_concat0(O, ", ");
			printOperand(MI, 1, O);

			if (SH > HEX_THRESHOLD)
				SStream_concat(O, ", 0x%x", (unsigned int)SH);
			else
				SStream_concat(O, ", %u", (unsigned int)SH);

			if (MI->csh->detail) {
				cs_ppc *ppc = &MI->flat_insn->detail->ppc;

				ppc->operands[ppc->op_count].type = PPC_OP_IMM;
				ppc->operands[ppc->op_count].imm = SH;
				++ppc->op_count;
			}


			return;
		}
	}

	// dcbt[st] is printed manually here because:
	//  1. The assembly syntax is different between embedded and server targets
	//  2. We must print the short mnemonics for TH == 0 because the
	//     embedded/server syntax default will not be stable across assemblers
	//  The syntax for dcbt is:
	//    dcbt ra, rb, th [server]
	//    dcbt th, ra, rb [embedded]
	//  where th can be omitted when it is 0. dcbtst is the same.
	if (opcode == PPC_DCBT || opcode == PPC_DCBTST) {
		unsigned char TH = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 0));

		SStream_concat0(O, "dcbt");
		MCInst_setOpcodePub(MI, PPC_INS_DCBT);

		if (opcode == PPC_DCBTST) {
			SStream_concat0(O, "st");
			MCInst_setOpcodePub(MI, PPC_INS_DCBTST);
		}

		if (TH == 16) {
			SStream_concat0(O, "t");
			MCInst_setOpcodePub(MI, PPC_INS_DCBTSTT);
		}

		SStream_concat0(O, "\t");

		if (MI->csh->mode & CS_MODE_BOOKE && TH != 0 && TH != 16) {
			if (TH > HEX_THRESHOLD)
				SStream_concat(O, "0x%x, ", (unsigned int)TH);
			else
				SStream_concat(O, "%u, ", (unsigned int)TH);

			if (MI->csh->detail) {
				cs_ppc *ppc = &MI->flat_insn->detail->ppc;

				ppc->operands[ppc->op_count].type = PPC_OP_IMM;
				ppc->operands[ppc->op_count].imm = TH;
				++ppc->op_count;
			}
		}

		printOperand(MI, 1, O);
		SStream_concat0(O, ", ");
		printOperand(MI, 2, O);

		if (!(MI->csh->mode & CS_MODE_BOOKE) && TH != 0 && TH != 16) {
			if (TH > HEX_THRESHOLD)
				SStream_concat(O, ", 0x%x", (unsigned int)TH);
			else
				SStream_concat(O, ", %u", (unsigned int)TH);

			if (MI->csh->detail) {
				cs_ppc *ppc = &MI->flat_insn->detail->ppc;

				ppc->operands[ppc->op_count].type = PPC_OP_IMM;
				ppc->operands[ppc->op_count].imm = TH;
				++ppc->op_count;
			}
		}

		return;
	}

	if (opcode == PPC_DCBF) {
		unsigned char L = (unsigned char)MCOperand_getImm(MCInst_getOperand(MI, 0));

		if (!L || L == 1 || L == 3) {
			SStream_concat0(O, "dcbf");
			MCInst_setOpcodePub(MI, PPC_INS_DCBF);

			if (L == 1 || L == 3) {
				SStream_concat0(O, "l");
				MCInst_setOpcodePub(MI, PPC_INS_DCBFL);
			}

			if (L == 3) {
				SStream_concat0(O, "p");
				MCInst_setOpcodePub(MI, PPC_INS_DCBFLP);
			}

			SStream_concat0(O, "\t");

			printOperand(MI, 1, O);
			SStream_concat0(O, ", ");
			printOperand(MI, 2, O);

			return;
		}
	}

	if (opcode == PPC_B || opcode == PPC_BA || opcode == PPC_BL ||
			opcode == PPC_BLA) {
		int64_t bd = MCOperand_getImm(MCInst_getOperand(MI, 0));
		bd = SignExtend64(bd, 24);
		MCOperand_setImm(MCInst_getOperand(MI, 0), bd);
	}

	if (opcode == PPC_gBC || opcode == PPC_gBCA || opcode == PPC_gBCL ||
			opcode == PPC_gBCLA) {
		int64_t bd = MCOperand_getImm(MCInst_getOperand(MI, 2));
		bd = SignExtend64(bd, 14);
		MCOperand_setImm(MCInst_getOperand(MI, 2), bd);
	}

	if (isBOCTRBranch(MCInst_getOpcode(MI))) {
		if (MCOperand_isImm(MCInst_getOperand(MI,0))) {
			int64_t bd = MCOperand_getImm(MCInst_getOperand(MI, 0));
			bd = SignExtend64(bd, 14);
			MCOperand_setImm(MCInst_getOperand(MI, 0), bd);
		}
	}

	mnem = printAliasBcc(MI, O, Info);
	if (!mnem)
		mnem = printAliasInstr(MI, O, Info);

	if (mnem != NULL) {
		if (strlen(mnem) > 0) {
			// check to remove the last letter of ('.', '-', '+')
			if (mnem[strlen(mnem) - 1] == '-' || mnem[strlen(mnem) - 1] == '+' || mnem[strlen(mnem) - 1] == '.')
				mnem[strlen(mnem) - 1] = '\0';

            MCInst_setOpcodePub(MI, PPC_map_insn(mnem));

            if (MI->csh->detail) {
				struct ppc_alias alias;

				if (PPC_alias_insn(mnem, &alias)) {
					MI->flat_insn->detail->ppc.bc = (ppc_bc)alias.cc;
				}
            }
		}

		cs_mem_free(mnem);
	} else
		printInstruction(MI, O);
}

// FIXME
enum ppc_bc_hint {
	PPC_BC_LT_MINUS = (0 << 5) | 14,
	PPC_BC_LE_MINUS = (1 << 5) |  6,
	PPC_BC_EQ_MINUS = (2 << 5) | 14,
	PPC_BC_GE_MINUS = (0 << 5) |  6,
	PPC_BC_GT_MINUS = (1 << 5) | 14,
	PPC_BC_NE_MINUS = (2 << 5) |  6,
	PPC_BC_UN_MINUS = (3 << 5) | 14,
	PPC_BC_NU_MINUS = (3 << 5) |  6,
	PPC_BC_LT_PLUS  = (0 << 5) | 15,
	PPC_BC_LE_PLUS  = (1 << 5) |  7,
	PPC_BC_EQ_PLUS  = (2 << 5) | 15,
	PPC_BC_GE_PLUS  = (0 << 5) |  7,
	PPC_BC_GT_PLUS  = (1 << 5) | 15,
	PPC_BC_NE_PLUS  = (2 << 5) |  7,
	PPC_BC_UN_PLUS  = (3 << 5) | 15,
	PPC_BC_NU_PLUS  = (3 << 5) |  7,
};

// FIXME
// normalize CC to remove _MINUS & _PLUS
static int cc_normalize(int cc)
{
	switch(cc) {
		default: return cc;
		case PPC_BC_LT_MINUS: return PPC_BC_LT;
		case PPC_BC_LE_MINUS: return PPC_BC_LE;
		case PPC_BC_EQ_MINUS: return PPC_BC_EQ;
		case PPC_BC_GE_MINUS: return PPC_BC_GE;
		case PPC_BC_GT_MINUS: return PPC_BC_GT;
		case PPC_BC_NE_MINUS: return PPC_BC_NE;
		case PPC_BC_UN_MINUS: return PPC_BC_UN;
		case PPC_BC_NU_MINUS: return PPC_BC_NU;
		case PPC_BC_LT_PLUS : return PPC_BC_LT;
		case PPC_BC_LE_PLUS : return PPC_BC_LE;
		case PPC_BC_EQ_PLUS : return PPC_BC_EQ;
		case PPC_BC_GE_PLUS : return PPC_BC_GE;
		case PPC_BC_GT_PLUS : return PPC_BC_GT;
		case PPC_BC_NE_PLUS : return PPC_BC_NE;
		case PPC_BC_UN_PLUS : return PPC_BC_UN;
		case PPC_BC_NU_PLUS : return PPC_BC_NU;
	}
}

static void printPredicateOperand(MCInst *MI, unsigned OpNo,
		SStream *O, const char *Modifier)
{
	unsigned Code = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	MI->flat_insn->detail->ppc.bc = (ppc_bc)cc_normalize(Code);

	if (!strcmp(Modifier, "cc")) {
		switch ((ppc_predicate)Code) {
			default:	// unreachable
			case PPC_PRED_LT_MINUS:
			case PPC_PRED_LT_PLUS:
			case PPC_PRED_LT:
				SStream_concat0(O, "lt");
				return;
			case PPC_PRED_LE_MINUS:
			case PPC_PRED_LE_PLUS:
			case PPC_PRED_LE:
				SStream_concat0(O, "le");
				return;
			case PPC_PRED_EQ_MINUS:
			case PPC_PRED_EQ_PLUS:
			case PPC_PRED_EQ:
				SStream_concat0(O, "eq");
				return;
			case PPC_PRED_GE_MINUS:
			case PPC_PRED_GE_PLUS:
			case PPC_PRED_GE:
				SStream_concat0(O, "ge");
				return;
			case PPC_PRED_GT_MINUS:
			case PPC_PRED_GT_PLUS:
			case PPC_PRED_GT:
				SStream_concat0(O, "gt");
				return;
			case PPC_PRED_NE_MINUS:
			case PPC_PRED_NE_PLUS:
			case PPC_PRED_NE:
				SStream_concat0(O, "ne");
				return;
			case PPC_PRED_UN_MINUS:
			case PPC_PRED_UN_PLUS:
			case PPC_PRED_UN:
				SStream_concat0(O, "un");
				return;
			case PPC_PRED_NU_MINUS:
			case PPC_PRED_NU_PLUS:
			case PPC_PRED_NU:
				SStream_concat0(O, "nu");
				return;
			case PPC_PRED_BIT_SET:
			case PPC_PRED_BIT_UNSET:
				// llvm_unreachable("Invalid use of bit predicate code");
				SStream_concat0(O, "invalid-predicate");
				return;
		}
	}

	if (!strcmp(Modifier, "pm")) {
		switch ((ppc_predicate)Code) {
			case PPC_PRED_LT:
			case PPC_PRED_LE:
			case PPC_PRED_EQ:
			case PPC_PRED_GE:
			case PPC_PRED_GT:
			case PPC_PRED_NE:
			case PPC_PRED_UN:
			case PPC_PRED_NU:
				return;
			case PPC_PRED_LT_MINUS:
			case PPC_PRED_LE_MINUS:
			case PPC_PRED_EQ_MINUS:
			case PPC_PRED_GE_MINUS:
			case PPC_PRED_GT_MINUS:
			case PPC_PRED_NE_MINUS:
			case PPC_PRED_UN_MINUS:
			case PPC_PRED_NU_MINUS:
				SStream_concat0(O, "-");
				return;
			case PPC_PRED_LT_PLUS:
			case PPC_PRED_LE_PLUS:
			case PPC_PRED_EQ_PLUS:
			case PPC_PRED_GE_PLUS:
			case PPC_PRED_GT_PLUS:
			case PPC_PRED_NE_PLUS:
			case PPC_PRED_UN_PLUS:
			case PPC_PRED_NU_PLUS:
				SStream_concat0(O, "+");
				return;
			case PPC_PRED_BIT_SET:
			case PPC_PRED_BIT_UNSET:
				// llvm_unreachable("Invalid use of bit predicate code");
				SStream_concat0(O, "invalid-predicate");
				return;
			default:	// unreachable
				return;
		}
		// llvm_unreachable("Invalid predicate code");
	}

	//assert(StringRef(Modifier) == "reg" &&
	//		"Need to specify 'cc', 'pm' or 'reg' as predicate op modifier!");
	printOperand(MI, OpNo + 1, O);
}

static void printATBitsAsHint(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned Code = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	if (Code == 2) {
		SStream_concat0(O, "-");
	} else if (Code == 3) {
		SStream_concat0(O, "+");
	}
}

static void printU1ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	// assert(Value <= 1 && "Invalid u1imm argument!");

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU2ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
	//assert(Value <= 3 && "Invalid u2imm argument!");

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU3ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
	//assert(Value <= 8 && "Invalid u3imm argument!");

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU4ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
	//assert(Value <= 15 && "Invalid u4imm argument!");

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printS5ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	int Value = (int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
	Value = SignExtend32(Value, 5);

	printInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU5ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	//assert(Value <= 31 && "Invalid u5imm argument!");
	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU6ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	//assert(Value <= 63 && "Invalid u6imm argument!");
	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU7ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	//assert(Value <= 127 && "Invalid u7imm argument!");
	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

// Operands of BUILD_VECTOR are signed and we use this to print operands
// of XXSPLTIB which are unsigned. So we simply truncate to 8 bits and
// print as unsigned.
static void printU8ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printU10ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned int Value = (unsigned int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	//assert(Value <= 1023 && "Invalid u10imm argument!");
	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printS12ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	if (MCOperand_isImm(MCInst_getOperand(MI, OpNo))) {
		int Imm = (int)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
		Imm = SignExtend32(Imm, 12);

		printInt32(O, Imm);

		if (MI->csh->detail) {
			if (MI->csh->doing_mem) {
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.disp = Imm;
			} else {
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Imm;
                MI->flat_insn->detail->ppc.op_count++;
            }
		}
	} else
		printOperand(MI, OpNo, O);
}

static void printU12ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned short Value = (unsigned short)MCOperand_getImm(MCInst_getOperand(MI, OpNo));

	// assert(Value <= 4095 && "Invalid u12imm argument!");

	printUInt32(O, Value);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Value;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printS16ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	if (MCOperand_isImm(MCInst_getOperand(MI, OpNo))) {
		short Imm = (short)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
		printInt32(O, Imm);

		if (MI->csh->detail) {
			if (MI->csh->doing_mem) {
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.disp = Imm;
			} else {
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
                MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Imm;
                MI->flat_insn->detail->ppc.op_count++;
            }
		}
	} else
		printOperand(MI, OpNo, O);
}

static void printU16ImmOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	if (MCOperand_isImm(MCInst_getOperand(MI, OpNo))) {
		unsigned short Imm = (unsigned short)MCOperand_getImm(MCInst_getOperand(MI, OpNo));
		printUInt32(O, Imm);

		if (MI->csh->detail) {
			MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
			MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = Imm;
			MI->flat_insn->detail->ppc.op_count++;
		}
	} else
		printOperand(MI, OpNo, O);
}

static void printBranchOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	if (!MCOperand_isImm(MCInst_getOperand(MI, OpNo))) {
		printOperand(MI, OpNo, O);

		return;
	}

	// Branches can take an immediate operand.  This is used by the branch
	// selection pass to print .+8, an eight byte displacement from the PC.
	// O << ".+";
	printAbsBranchOperand(MI, OpNo, O);
}

static void printAbsBranchOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	int64_t imm;

	if (!MCOperand_isImm(MCInst_getOperand(MI, OpNo))) {
		printOperand(MI, OpNo, O);

		return;
	}

	imm = SignExtend32(MCOperand_getImm(MCInst_getOperand(MI, OpNo)) * 4, 32);
	//imm = MCOperand_getImm(MCInst_getOperand(MI, OpNo)) * 4;

	if (!PPC_abs_branch(MI->csh, MCInst_getOpcode(MI))) {
		imm += MI->address;
	}

	printUInt64(O, imm);

	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = imm;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

static void printcrbitm(MCInst *MI, unsigned OpNo, SStream *O)
{
	unsigned RegNo;
	unsigned CCReg = MCOperand_getReg(MCInst_getOperand(MI, OpNo));

	switch (CCReg) {
		default: // llvm_unreachable("Unknown CR register");
		case PPC_CR0: RegNo = 0; break;
		case PPC_CR1: RegNo = 1; break;
		case PPC_CR2: RegNo = 2; break;
		case PPC_CR3: RegNo = 3; break;
		case PPC_CR4: RegNo = 4; break;
		case PPC_CR5: RegNo = 5; break;
		case PPC_CR6: RegNo = 6; break;
		case PPC_CR7: RegNo = 7; break;
	}

	printUInt32(O, 0x80 >> RegNo);
}

static void printMemRegImm(MCInst *MI, unsigned OpNo, SStream *O)
{
	set_mem_access(MI, true);

	printS16ImmOperand(MI, OpNo, O);

	SStream_concat0(O, "(");

	if (MCOperand_getReg(MCInst_getOperand(MI, OpNo + 1)) == PPC_R0)
		SStream_concat0(O, "0");
	else
		printOperand(MI, OpNo + 1, O);

	SStream_concat0(O, ")");

	set_mem_access(MI, false);
}

static void printPSMemRegImm(MCInst *MI, unsigned OpNo, SStream *O)
{
	set_mem_access(MI, true);

	printS12ImmOperand(MI, OpNo, O);

	SStream_concat0(O, "(");
	printOperand(MI, OpNo + 1, O);
	SStream_concat0(O, ")");

	set_mem_access(MI, false);
}

static void printMemRegReg(MCInst *MI, unsigned OpNo, SStream *O)
{
	// When used as the base register, r0 reads constant zero rather than
	// the value contained in the register.  For this reason, the darwin
	// assembler requires that we print r0 as 0 (no r) when used as the base.
	if (MCOperand_getReg(MCInst_getOperand(MI, OpNo)) == PPC_R0)
		SStream_concat0(O, "0");
	else
		printOperand(MI, OpNo, O);
	SStream_concat0(O, ", ");

	printOperand(MI, OpNo + 1, O);
}

static void printTLSCall(MCInst *MI, unsigned OpNo, SStream *O)
{
	set_mem_access(MI, true);
	//printBranchOperand(MI, OpNo, O);

	// On PPC64, VariantKind is VK_None, but on PPC32, it's VK_PLT, and it must
	// come at the _end_ of the expression.

	SStream_concat0(O, "(");
	printOperand(MI, OpNo + 1, O);
	SStream_concat0(O, ")");

	set_mem_access(MI, false);
}

/// stripRegisterPrefix - This method strips the character prefix from a
/// register name so that only the number is left.  Used by for linux asm.
static char *stripRegisterPrefix(const char *RegName)
{
	switch (RegName[0]) {
		case 'r':
		case 'f':
		case 'q': // for QPX
		case 'v':
			if (RegName[1] == 's')
				return cs_strdup(RegName + 2);

			return cs_strdup(RegName + 1);
		case 'c':
			if (RegName[1] == 'r') {
				// skip the first 2 letters "cr"
				char *name = cs_strdup(RegName + 2);

				// also strip the last 2 letters
				name[strlen(name) - 2] = '\0';

				return name;
			}
	}

	return cs_strdup(RegName);
}

static void printOperand(MCInst *MI, unsigned OpNo, SStream *O)
{
	MCOperand *Op = MCInst_getOperand(MI, OpNo);
	if (MCOperand_isReg(Op)) {
		unsigned reg = MCOperand_getReg(Op);
#ifndef CAPSTONE_DIET
		const char *RegName = getRegisterName(reg);

		// printf("reg = %u (%s)\n", reg, RegName);

		// convert internal register ID to public register ID
		reg = PPC_name_reg(RegName);

		// The linux and AIX assembler does not take register prefixes.
		if (MI->csh->syntax == CS_OPT_SYNTAX_NOREGNAME) {
			char *name = stripRegisterPrefix(RegName);
			SStream_concat0(O, name);
			cs_mem_free(name);
		} else
			SStream_concat0(O, RegName);
#endif

		if (MI->csh->detail) {
			if (MI->csh->doing_mem) {
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.base = reg;
			} else {
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_REG;
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].reg = reg;
				MI->flat_insn->detail->ppc.op_count++;
			}
		}

		return;
	}

	if (MCOperand_isImm(Op)) {
		int32_t imm = (int32_t)MCOperand_getImm(Op);
		printInt32(O, imm);

		if (MI->csh->detail) {
			if (MI->csh->doing_mem) {
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].mem.disp = (int32_t)imm;
			} else {
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
				MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = imm;
				MI->flat_insn->detail->ppc.op_count++;
			}
		}
	}
}

static void op_addImm(MCInst *MI, int v)
{
	if (MI->csh->detail) {
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].type = PPC_OP_IMM;
		MI->flat_insn->detail->ppc.operands[MI->flat_insn->detail->ppc.op_count].imm = v;
		MI->flat_insn->detail->ppc.op_count++;
	}
}

#define PRINT_ALIAS_INSTR
#include "PPCGenRegisterName.inc"
#include "PPCGenAsmWriter.inc"

#endif
