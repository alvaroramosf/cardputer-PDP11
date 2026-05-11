#include <cstdlib>
//#include <sched.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
//#include <unistd.h>
#include <Arduino.h>
#include "bootrom.h"
#include "kb11.h"
#include "options.h"
#include <SD.h>

/*
            ***** Update 1/3/2023 ISS *****
            * This update resolves a fundamental design flaw which placed the
            * CPU registers at an arbirary and umappped location in the IOPAGE.
            * Due to the fact that any instruction can reference either a register directly eg INC R0
            * or, using a register as an address (with optional indirection) eg INC (R0), with only 16 bits of address
            * being passed to the read/write routines, a method of determining if the operand is a register
            * or an address is required. The minimal implmentation of this is the use of rflag to indicate
            * if an operand is a register or a memory address. This is set if the destination is a register and cleared if not.
            * The flag is cleared at the beginning of each CPU step and set in DA(...).
*/


void disasm(uint32_t ia);
void fp11(int32 IR);

void KB11::reset(uint16_t start,int bootdev) {
    unibus.reset();
    for (auto i = 0; i < 29; i++) {
        unibus.write16(02000 + (i * 2), bootrom[i]);
    }
    if (bootdev)
        unibus.rl11.loadboot();           // Overlay RK05 boot with RL boot
    // In PDP-11/23 mode, enable 22-bit MMU from the start (F-11 default)
    if (current_options.cpu_model == CPU_PDP1123) {
        mmu.SR[3] = 020; // Enable 22-bit addressing
    }
    R[7] = start;
    stacklimit = 0xff;
    switchregister = 0173030;
    switchregister = 0;
    wtstate = false;
}

inline uint16_t KB11::read16(const uint16_t va) {
    uint32_t tm;
    auto a = tm = mmu.decode<false>(va, currentmode());

    if (!(mmu.SR[3] & 020))
        a |= 017000000;

    if ((a & 017777760) == 017777700)
        return R[(a & 017) >> 1];

    switch (a) {
    case 017777776:
        return PSW;
    case 017777774:
        return stacklimit;
    case 017777570:
        return switchregister;
    default:
        return unibus.read16(tm);
    }
}

inline void KB11::write16(const uint16_t va, const uint16_t v) {
    uint32_t tm;
    auto a = tm = mmu.decode<true>(va, currentmode());

    if (!(mmu.SR[3] & 020))
        a |= 017000000;

    if ((a & 017777760) == 017777700)
    {
        R[(a & 017) >> 1] = v;
        return;
    }

    switch (a) {
    case 017777776:
        stackpointer[currentmode()] = R[6];
        writePSW(v & ~020);
        R[6] = stackpointer[currentmode()];
        break;
    case 017777774:
        stacklimit = v;
        break;
    case 017777570:
        displayregister = v;
        break;
    default:
        unibus.write16(tm, v);
    }
}

// ADD 06SSDD
void KB11::ADD(const uint16_t instr) {
    const auto src = SS<2>(instr);
    const auto da = DA<2>(instr);
    const auto dst = read<2>(da);
    const auto sum = src + dst;
    write<2>(da, sum);
    PSW &= 0xFFF0;
    setNZ<2>(sum);
    if (((~src ^ dst) & (src ^ sum)) & 0x8000) {
        PSW |= FLAGV;
    }
    if ((int32_t(src) + int32_t(dst)) > 0xFFFF) {
        PSW |= FLAGC;
    }
}

// SUB 16SSDD
void KB11::SUB(const uint16_t instr) {
    const auto val1 = SS<2>(instr);
    const auto da = DA<2>(instr);
    const auto val2 = read<2>(da);
    const auto uval = (val2 - val1) & 0xFFFF;
    PSW &= 0xFFF0;
    write<2>(da, uval);
    setNZ<2>(uval);
    if (((val1 ^ val2) & (~val1 ^ uval)) & 0x8000) {
        PSW |= FLAGV;
    }
    if (val1 > val2) {
        PSW |= FLAGC;
    }
}

// MUL 070RSS
void KB11::MUL(const uint16_t instr) {
	const auto reg = (instr >> 6) & 7;
	int32_t val1 = R[reg];
	if (val1 & 0x8000) {
		val1 = -((0xFFFF ^ val1) + 1);
	}
	int32_t val2 = read<2>(DA<2>(instr));
	if (val2 & 0x8000) {
		val2 = -((0xFFFF ^ val2) + 1);
	}
	int sval = val1 * val2;
	R[reg] = sval >> 16;
	R[reg | 1] = sval & 0xFFFF;
	PSW &= 0xFFF0;
	if (sval < 0) {
		PSW |= FLAGN;
	}
	if (sval == 0) {
		PSW |= FLAGZ;
	}
	if ((sval < -(1 << 15)) || (sval >= ((1L << 15) - 1))) {
		PSW |= FLAGC;
	}
}

void KB11::DIV(const uint16_t instr) {
	const auto reg = (instr >> 6) & 7;
	const int32_t val1 = (R[reg] << 16) | (R[reg | 1]);
	int32_t val2 = read<2>(DA<2>(instr));
	PSW &= 0xFFF0;
	if (val2 > 32767)
		val2 |= 0xffff0000;
	if (val2 == 0) {
		PSW |= FLAGC | FLAGV;
		return;
	}
	if ((val1 / val2) >= 0x10000) {
		PSW |= FLAGV;
		return;
	}
	R[reg] = (val1 / val2) & 0xFFFF;
	R[reg | 1] = (val1 % val2) & 0xFFFF;
	if (R[reg] == 0) {
		PSW |= FLAGZ;
	}
	if (R[reg] & 0100000) {
		PSW |= FLAGN;
	}
}

void KB11::ASH(const uint16_t instr) {
	const auto reg = (instr >> 6) & 7;
	const auto val1 = R[reg];
	auto val2 = read<2>(DA<2>(instr)) & 077;
	PSW &= 0xFFF0;
	int32_t sval = val1;
	if (val2 & 040) {
		val2 = 0100 - val2;
		if (sval & 0100000)
			sval |= 0xffff0000;
		sval = sval << 1;
		sval = sval >> val2;
		if (sval & 1)
			PSW |= FLAGC;
		sval = sval >> 1;
	}
	else {
		uint32_t msk = 0xffff << val2;
		sval = sval << val2;
		if (sval & 0xffff0000)
			if ((sval & 0xffff0000) != (msk & 0xffff0000))
				PSW |= FLAGV;
		if ((sval & 0x8000) != (val1 & 0x8000))
			PSW |= FLAGV;
		if (sval & 0200000)
			PSW |= FLAGC;
	}
	sval &= 0xffff;
	R[reg & 7] = sval;
	setZ(sval == 0);
	if (sval & 0100000) {
		PSW |= FLAGN;
	}
}

void KB11::ASHC(const uint16_t instr) {
    const auto reg = (instr >> 6) & 7;
    const auto val1 = ((uint32_t)(R[reg]) << 16) | (uint32_t)R[reg | 1];
    auto val2 = read<2>(DA<2>(instr)) & 077;
    PSW &= 0xFFF0;
    uint64_t msk;
    int64_t sval = (int)val1;
    if (val2 == 0) {
        setZ((sval & 0xffffffff) == 0);
        if (sval & 0x80000000)
            PSW |= FLAGN;
        return;
    }
    if (val2 & 040)
    {
        val2 = 0100 - val2;
        msk = ((uint64_t)1 << (val2 - 1));
        sval = sval >> val2;
        if ((val1 & msk) && val2)
            PSW |= FLAGC;
    }
    else
    {
        sval = sval << val2;
        msk = 0x100000000 >> val2;
        if ((val1 & msk) && val2)
            PSW |= FLAGC;
    }
    if ((sval & 0x80000000) != (val1 & 0x80000000))
        PSW |= FLAGV;
    R[reg & 7] = (sval >> 16) & 0xFFFF;
    R[(reg & 7) | 1] = sval & 0xFFFF;
    setZ((sval & 0xffffffff) == 0);
    if (sval & 0x80000000)
        PSW |= FLAGN;
}

// XOR 064RDD
void KB11::XOR(const uint16_t instr) {
    const auto reg = R[(instr >> 6) & 7];
    const auto da = DA<2>(instr);
    const auto dst = reg ^ read<2>(da);
    write<2>(da, dst);
    setNZ<2>(dst);
}

// SOB 077RNN

void KB11::SOB(const uint16_t instr) {
    const auto reg = (instr >> 6) & 7;
    R[reg]--;
    if (R[reg]) {
        R[7] -= (instr & 077) << 1;
    }
}

void KB11::FIS(const uint16_t instr)
{
    union fltint {
        uint32_t xint;
        float xflt;
    };
    uint32_t arg1, arg2;
    fltint bfr;
    uint16_t adr = R[instr & 7];      // Base address from specfied reg
    float op1, op2;

    arg1 = bfr.xint = read<2>(adr + 6) | (read<2>(adr + 4) << 16);
    op1 = bfr.xflt;
    arg2 = bfr.xint = read<2>(adr + 2) | (read<2>(adr) << 16);
    op2 = bfr.xflt;
    PSW &= ~(FLAGN | FLAGV | FLAGZ | FLAGC);
    switch (instr & 070) {
    case 0:                 // FADD
        bfr.xflt = op1 + op2;
        break;
    case 010:
        bfr.xflt = op1 - op2;
        break;
    case 020:
        bfr.xflt = (op1 * op2) / 4.0f;
        break;
    case 030:
        if (op2 == 0.0) {
            PSW |= (FLAGN | FLAGV | FLAGC);
            trap(INTFIS);
        }
        bfr.xflt = (op1 / op2) * 4.0f;
        break;
    }
    R[instr & 7] += 4;
    if (bfr.xflt == 0.0)
        PSW |= FLAGZ;
    if (bfr.xflt < 0.0)
        PSW |= FLAGN;
    write<2>(adr + 4, bfr.xint >> 16);
    write<2>(adr + 6, bfr.xint);
}


// MTPS

void KB11::MTPS(const uint16_t instr)

{
    const auto da = DA<1>(instr);
    auto src = read<1>(da);
    PSW = (PSW & 0177400) | src & 0357;
}

// MFPS

void KB11::MFPS(const uint16_t instr)
{
    const auto da = DA<1>(instr);
    auto dst = PSW & 0357;
    if (PSW & msb<1>() && ((instr & 030) == 0)) {
        dst |= 0177400;
        write<2>(da, dst);
    }
    else
        write<1>(da, dst);
    setNZ<1>(PSW & 0377);
}

// JSR 004RDD
void KB11::JSR(const uint16_t instr) {
    if (((instr >> 3) & 7) == 0) {
        trap(INTBUS);
    }
    const auto dst = DA<2>(instr);
    const auto reg = (instr >> 6) & 7;
    push(R[reg]);
    R[reg] = R[7];
    R[7] = dst;
}

// JMP 0001DD
void KB11::JMP(const uint16_t instr) {
    if (((instr >> 3) & 7) == 0) {
        // Registers don't have a virtual address so trap!
        //Serial.printf("JMP called on register\n");
        //printstate();
        trap(INTBUS);
        //std::abort();
    } else
       R[7] = DA<2>(instr);
}

// MARK 0064NN
void KB11::MARK(const uint16_t instr) {
    R[6] = R[7] + ((instr & 077) << 1);
    R[7] = R[5];
    R[5] = pop();
}

// MFPI 0065SS
void KB11::MFPI(const uint16_t instr) {
    uint16_t uval;
    if (!(instr & 0x38)) {
        const auto reg = instr & 7;
        if ((reg != 6) || (currentmode() == previousmode())) {
            uval = R[reg];
        }
        else {
            uval = stackpointer[previousmode()];
        }
    }
    else {
        const auto da = DA<2>(instr);
        uval = unibus.read16(mmu.decode<false>(da, previousmode()));
    }
    push(uval);
    setNZ<2>(uval);
}

// MTPI 0066DD
void KB11::MTPI(const uint16_t instr) {
    const auto uval = pop();
    if (!(instr & 0x38)) {
        const auto reg = instr & 7;
        if ((reg != 6) || (currentmode() == previousmode())) {
            R[reg] = uval;
        } else {
            stackpointer[previousmode()] = uval;
        }
    } else {
        const auto da = DA<2>(instr);
        unibus.write16(mmu.decode<true>(da, previousmode()), uval);
    }
    setNZ<2>(uval);
}

// RTS 00020R
void KB11::RTS(const uint16_t instr) {
    const auto reg = instr & 7;
    R[7] = R[reg];
    R[reg] = pop();
}

// MFPT 000007
// Returns processor type: PDP-11/23 (F-11) returns 1 in R0.
// PDP-11/40 does not have MFPT; executing it traps to vector 010.
void KB11::MFPT() {
    if (current_options.cpu_model == CPU_PDP1123) {
        R[0] = 1;   // F-11 chip identifier
    } else {
        trap(010);  // PDP-11/40: MFPT is an illegal instruction
    }
}

// RTI 000004, RTT 000006
void KB11::RTT() {
    R[7] = pop();
    auto psw = pop();
    psw &= 0xf8ff;
    if (currentmode()) { // user / super restrictions
        // keep SPL and allow lower only for modes and register set
        //psw = (PSW & 0xf81f) | (psw & 0xf8e0);
        psw = (PSW & 0177760) | (psw & 017);
    }
    writePSW(psw);
    if (R[7] == 0)
        R[7] = R[7];
}

void KB11::WAIT() {
    if (currentmode())
        return;
    wtstate = true;
}

void KB11::RESET() {
    if (currentmode()) {
        // RESET is ignored outside of kernel mode
        return;
    }
    unibus.reset();
}

// SWAB 0003DD
void KB11::SWAB(const uint16_t instr) {
    const auto da = DA<2>(instr);
    auto dst = read<2>(da);
    dst = (dst << 8) | (dst >> 8);
    write<2>(da, dst);
    PSW &= 0xFFF0;
    if ((dst & 0xff) == 0) {
        PSW |= FLAGZ;
    }
    if (dst & 0x80) {
        PSW |= FLAGN;
    }
}

// SXT 0067DD
void KB11::SXT(const uint16_t instr) {
    if (N()) {
        write<2>(DA<2>(instr), 0xffff);
        PSW &= ~FLAGZ;
    } else {
        write<2>(DA<2>(instr), 0);
        PSW |= FLAGZ;
    }
    PSW &= ~FLAGV;
}

void KB11::step() {
    PC = R[7];
    rflag = 0;
    if (print)
        printstate();
    const auto instr = fetch16();
    if (!(mmu.SR[0] & 0160000)) {
        mmu.SR[2] = PC;
        mmu.SR[0] |= 0200;
    }

    switch (instr >> 12) {    // xxSSDD Mostly double operand instructions
    case 0:                   // 00xxxx mixed group
        switch (instr >> 8) { // 00xxxx 8 bit instructions first (branch & JSR)
        case 0:               // 000xXX Misc zero group
            switch (instr >> 6) { // 000xDD group (4 case full decode)
            case 0:               // 0000xx group
                switch (instr) {
                case 0: // HALT 000000
                    if (currentmode()) {
                        trap(010);
                    }
                    Serial.printf("HALT: DR: %06o\r\n", displayregister);
                    printstate();
                    wtstate = true; // CPU idles; G0 button opens Options Menu
                    return;
                case 1: // WAIT 000001
                    WAIT();
                    return;
                case 3:          // BPT  000003
                    trap(014); // Trap 14 - BPT
                    return;
                case 4: // IOT  000004
                    trap(020);
                    return;
                case 5: // RESET 000005
                    RESET();
                    return;
                case 2: // RTI 000002
                case 6: // RTT 000006
                    RTT();
                    return;
                case 7: // MFPT
                    MFPT();
                    return;
                default: // We don't know this 0000xx instruction
                    Serial.printf("unknown 0000xx instruction\n");
                    printstate();
                    trap(INTINVAL);
                    return;
                }
            case 1: // JMP 0001DD
                JMP(instr);
                return;
            case 2:                         // 00002xR single register group
                switch ((instr >> 3) & 7) { // 00002xR register or CC
                case 0:                     // RTS 00020R
                    RTS(instr);
                    return;
                case 3: // SPL 00023N
                    if (currentmode())
                        trap(010);
                    PSW = ((PSW & 0xf81f) | ((instr & 7) << 5));
                    return;
                case 4: // CLR CC 00024C Part 1 without N
                case 5: // CLR CC 00025C Part 2 with N
                    PSW = (PSW & ~(instr & 017));
                    return;
                case 6: // SET CC 00026C Part 1 without N
                case 7: // SET CC 00027C Part 2 with N
                    PSW = (PSW | (instr & 017));
                    return;
                default: // We don't know this 00002xR instruction
                    Serial.printf("unknown 0002xR instruction\n");
                    printstate();
                    trap(INTINVAL);
                    return;
                }
            case 3: // SWAB 0003DD
                SWAB(instr);
                return;
            default:
                Serial.printf("unknown 000xDD instruction\n");
                printstate();
                trapat(INTINVAL);
                return;
            }
        case 1: // BR 0004 offset
            branch(instr);
            return;
        case 2: // BNE 0010 offset
            if (!Z()) {
                branch(instr);
            }
            return;
        case 3: // BEQ 0014 offset
            if (Z()) {
                branch(instr);
            }
            return;
        case 4: // BGE 0020 offset
            if (!(N() xor V())) {
                branch(instr);
            }
            return;
        case 5: // BLT 0024 offset
            if (N() xor V()) {
                branch(instr);
            }
            return;
        case 6: // BGT 0030 offset
            if ((!(N() xor V())) && (!Z())) {
                branch(instr);
            }
            return;
        case 7: // BLE 0034 offset
            if ((N() xor V()) || Z()) {
                branch(instr);
            }
            return;
        case 8: // JSR 004RDD In two parts
        case 9: // JSR 004RDD continued (9 bit instruction so use 2 x 8 bit
            JSR(instr);
            return;
        default: // Remaining 0o00xxxx instructions where xxxx >= 05000
            switch (instr >> 6) { // 00xxDD
            case 050:             // CLR 0050DD
                CLR<2>(instr);
                return;
            case 051: // COM 0051DD
                COM<2>(instr);
                return;
            case 052: // INC 0052DD
                INC<2>(instr);
                return;
            case 053: // DEC 0053DD
                _DEC<2>(instr);
                return;
            case 054: // NEG 0054DD
                NEG<2>(instr);
                return;
            case 055: // ADC 0055DD
                _ADC<2>(instr);
                return;
            case 056: // SBC 0056DD
                SBC<2>(instr);
                return;
            case 057: // TST 0057DD
                TST<2>(instr);
                return;
            case 060: // ROR 0060DD
                ROR<2>(instr);
                return;
            case 061: // ROL 0061DD
                ROL<2>(instr);
                return;
            case 062: // ASR 0062DD
                ASR<2>(instr);
                return;
            case 063: // ASL 0063DD
                ASL<2>(instr);
                return;
            case 064: // MARK 0064nn
                MARK(instr);
                return;
            case 065: // MFPI 0065SS
                MFPI(instr);
                return;
            case 066: // MTPI 0066DD
                MTPI(instr);
                return;
            case 067: // SXT 0067DD
                SXT(instr);
                return;
            default: // We don't know this 0o00xxDD instruction
                Serial.printf("unknown 00xxDD instruction\n");
                printstate();
                trapat(INTINVAL);
                return;
            }
        }
    case 1: // MOV  01SSDD
        MOV<2>(instr);
        return;
    case 2: // CMP 02SSDD
        CMP<2>(instr);
        return;
    case 3: // BIT 03SSDD
        SBIT<2>(instr);
        return;
    case 4: // BIC 04SSDD
        BIC<2>(instr);
        return;
    case 5: // BIS 05SSDD
        BIS<2>(instr);
        return;
    case 6: // ADD 06SSDD
        ADD(instr);
        return;
    case 7:                         // 07xRSS instructions
        switch ((instr >> 9) & 7) { // 07xRSS
        case 0:                     // MUL 070RSS
            MUL(instr);
            return;
        case 1: // DIV 071RSS
            DIV(instr);
            return;
        case 2: // ASH 072RSS
            ASH(instr);
            return;
        case 3: // ASHC 073RSS
            ASHC(instr);
            return;
        case 4: // XOR 074RSS
            XOR(instr);
            return;
        case 5: // FIS
            FIS(instr);
            return;
        case 6:
            //Serial.printf("Invalid instruction:%06o\r\n",instr);
            trap(INTINVAL);
        case 7: // SOB 077Rnn
            SOB(instr);
            return;
        default: // We don't know this 07xRSS instruction
            Serial.printf("unknown 07xRSS instruction\n");
            printstate();
            trapat(INTINVAL);
            return;
        }
    case 8:                           // 10xxxx instructions
        switch ((instr >> 8) & 0xf) { // 10xxxx 8 bit instructions first
        case 0:                       // BPL 1000 offset
            if (!N()) {
                branch(instr);
            }
            return;
        case 1: // BMI 1004 offset
            if (N()) {
                branch(instr);
            }
            return;
        case 2: // BHI 1010 offset
            if ((!C()) && (!Z())) {
                branch(instr);
            }
            return;
        case 3: // BLOS 1014 offset
            if (C() || Z()) {
                branch(instr);
            }
            return;
        case 4: // BVC 1020 offset
            if (!V()) {
                branch(instr);
            }
            return;
        case 5: // BVS 1024 offset
            if (V()) {
                branch(instr);
            }
            return;
        case 6: // BCC 1030 offset
            if (!C()) {
                branch(instr);
            }
            return;
        case 7: // BCS 1034 offset
            if (C()) {
                branch(instr);
            }
            return;
        case 8:          // EMT 1040 operand
            trap(030); // Trap 30 - EMT instruction
        case 9:          // TRAP 1044 operand
            trap(034); // Trap 34 - TRAP instruction
        default: // Remaining 10xxxx instructions where xxxx >= 05000
            switch ((instr >> 6) & 077) { // 10xxDD group
            case 050:                     // CLRB 1050DD
                CLR<1>(instr);
                return;
            case 051: // COMB 1051DD
                COM<1>(instr);
                return;
            case 052: // INCB 1052DD
                INC<1>(instr);
                return;
            case 053: // DECB 1053DD
                _DEC<1>(instr);
                return;
            case 054: // NEGB 1054DD
                NEG<1>(instr);
                return;
            case 055: // ADCB 01055DD
                _ADC<1>(instr);
                return;
            case 056: // SBCB 01056DD
                SBC<1>(instr);
                return;
            case 057: // TSTB 1057DD
                TST<1>(instr);
                return;
            case 060: // RORB 1060DD
                ROR<1>(instr);
                return;
            case 061: // ROLB 1061DD
                ROL<1>(instr);
                return;
            case 062: // ASRB 1062DD
                ASR<1>(instr);
                return;
            case 063: // ASLB 1063DD
                ASL<1>(instr);
                return;
            case 064: // MTPS 1064SS
                MTPS(instr);
                return;
            case 067: // MFPS 106700
                MFPS(instr);
                return;
            case 065:
                MFPI(instr);
                return;
            case 066:
                MTPI(instr);
                return;
            // case 0o65: // MFPD 1065DD
            // case 0o66: // MTPD 1066DD
            // case 0o67: // MTFS 1064SS
            default: // We don't know this 0o10xxDD instruction
                Serial.printf("unknown 0o10xxDD instruction\n");
                printstate();
                trapat(INTINVAL);
                return;
            }
        }
    case 9: // MOVB 11SSDD
        MOV<1>(instr);
        return;
    case 10: // CMPB 12SSDD
        CMP<1>(instr);
        return;
    case 11: // BITB 13SSDD
        SBIT<1>(instr);
        return;
    case 12: // BICB 14SSDD
        BIC<1>(instr);
        return;
    case 13: // BISB 15SSDD
        BIS<1>(instr);
        return;
    case 14: // SUB 16SSDD
        SUB(instr);
        return;
    case 15:
        fp11(instr);
        return;
        Serial.printf("invalid 17xxxx FPP instruction\n");
        printstate();
        trap(INTINVAL);
        break;
        [[fallthrough]];
    default: // 15  17xxxx FPP instructions
        //Serial.printf("invalid 17xxxx FPP instruction\n");
        //return;
        //printstate();
        trap(INTINVAL);
    }
}

void KB11::interrupt(uint8_t vec, uint8_t pri) {
    if (vec & 1) {
        printf("Thou darst calling interrupt() with an odd vector number?\n");
        std::abort();
    }
    wtstate = false;
    // fast path
    if (itab[0].vec == 0) {
        itab[0].vec = vec;
        itab[0].pri = pri;
        return;
    }
    uint8_t i = 0;
    for (i = 0; i < itab.size(); i++)
        if (itab[1].vec == vec && itab[i].pri == pri)
            return;
    i = 0;
    while (itab[i].pri > pri)
        i++;
    for (uint8_t j = itab.size() - 1; j > i; j--) {
        itab[j] = itab[j - 1];
    }
    itab[i].vec = vec;
    itab[i].pri = pri;

    if (i >= itab.size()) {
        printf("interrupt table full:%d\n", i);
        for (i = 0; i < itab.size(); i++)
            printf("%o %d:%d\r\n", itab[i].vec, i, itab.size());
        std::abort();
    }
}

// pop the top interrupt off the itab.
void KB11::popirq() {
    for (uint8_t i = 0; i < itab.size() - 1; i++) {
        itab[i] = itab[i + 1];
    }
    itab[itab.size() - 1].vec = 0;
    itab[itab.size() - 1].pri = 0;
    wtstate = false;
}

void KB11::trapat(uint16_t vec) {
    if (vec & 1) {
        Serial.printf("Thou darst calling trapat() with an odd vector number?\n");
        std::abort();
    }
    //Serial.printf("trap: vec: %03o pc:%06o\r\n", vec, R[7]);
    auto opsw = PSW;
    auto npsw = unibus.read16(mmu.decode<false>(vec, 0) + 2);
    writePSW(npsw | (currentmode() << 12));
    push(opsw);
    push(R[7]);
    R[7] = unibus.read16(mmu.decode<false>(vec, 0));       // Get from K-apace
    wtstate = false;
}

void KB11::saveSnapshot(const char* dir) {
    String path_cpu = String(dir) + "/cpu.bin";
    File f_cpu = SD.open(path_cpu.c_str(), FILE_WRITE);
    if (f_cpu) {
        f_cpu.write((uint8_t*)&PC, sizeof(PC));
        f_cpu.write((uint8_t*)&PSW, sizeof(PSW));
        f_cpu.write((uint8_t*)R, sizeof(R));
        f_cpu.write((uint8_t*)&oldPSW, sizeof(oldPSW));
        f_cpu.write((uint8_t*)&stacklimit, sizeof(stacklimit));
        f_cpu.write((uint8_t*)&switchregister, sizeof(switchregister));
        f_cpu.write((uint8_t*)&displayregister, sizeof(displayregister));
        f_cpu.write((uint8_t*)stackpointer.data(), sizeof(stackpointer));
        f_cpu.write((uint8_t*)itab.data(), itab.size() * sizeof(itab[0]));
        f_cpu.write((uint8_t*)&rflag, sizeof(rflag));
        f_cpu.write((uint8_t*)&wtstate, sizeof(wtstate));
        f_cpu.close();
    }

    String path_mmu = String(dir) + "/mmu.bin";
    File f_mmu = SD.open(path_mmu.c_str(), FILE_WRITE);
    if (f_mmu) {
        f_mmu.write((uint8_t*)&mmu, sizeof(KT11));
        f_mmu.close();
    }

    String path_umap = String(dir) + "/umap.bin";
    File f_umap = SD.open(path_umap.c_str(), FILE_WRITE);
    if (f_umap) {
        f_umap.write((uint8_t*)unibus.umap, sizeof(unibus.umap));
        f_umap.close();
    }

    String path_mem = String(dir) + "/mem.bin";
    File f_mem = SD.open(path_mem.c_str(), FILE_WRITE);
    if (f_mem) {
        int num_pages = (mmu.SR[3] & 020) ? (MEMSIZE22 / 32768) : (MEMSIZE / 32768);
        if ((mmu.SR[3] & 020) && (MEMSIZE22 % 32768 != 0)) num_pages++;
        else if (!(mmu.SR[3] & 020) && (MEMSIZE % 32768 != 0)) num_pages++;
        f_mem.write((uint8_t*)&num_pages, sizeof(num_pages));
        for (int i = 0; i < num_pages; i++) {
            if (unibus.core_pages[i]) {
                f_mem.write((uint8_t*)unibus.core_pages[i], 32768);
            }
        }
        f_mem.close();
    }

    String path_dev = String(dir) + "/devices.bin";
    File f_dev = SD.open(path_dev.c_str(), FILE_WRITE);
    if (f_dev) {
        unibus.rk11.saveSnapshot(f_dev);
        unibus.rl11.saveSnapshot(f_dev);
        unibus.kw11.saveSnapshot(f_dev);
        unibus.cons.saveSnapshot(f_dev);
        unibus.dl11.saveSnapshot(f_dev);
        f_dev.close();
    }
}


void KB11::loadSnapshot(const char* dir) {
    String path_cpu = String(dir) + "/cpu.bin";
    File f_cpu = SD.open(path_cpu.c_str(), FILE_READ);
    if (f_cpu) {
        f_cpu.read((uint8_t*)&PC, sizeof(PC));
        f_cpu.read((uint8_t*)&PSW, sizeof(PSW));
        f_cpu.read((uint8_t*)R, sizeof(R));
        f_cpu.read((uint8_t*)&oldPSW, sizeof(oldPSW));
        f_cpu.read((uint8_t*)&stacklimit, sizeof(stacklimit));
        f_cpu.read((uint8_t*)&switchregister, sizeof(switchregister));
        f_cpu.read((uint8_t*)&displayregister, sizeof(displayregister));
        f_cpu.read((uint8_t*)stackpointer.data(), sizeof(stackpointer));
        f_cpu.read((uint8_t*)itab.data(), itab.size() * sizeof(itab[0]));
        f_cpu.read((uint8_t*)&rflag, sizeof(rflag));
        f_cpu.read((uint8_t*)&wtstate, sizeof(wtstate));
        f_cpu.close();
    }

    String path_mmu = String(dir) + "/mmu.bin";
    File f_mmu = SD.open(path_mmu.c_str(), FILE_READ);
    if (f_mmu) {
        f_mmu.read((uint8_t*)&mmu, sizeof(KT11));
        f_mmu.close();
    }

    String path_umap = String(dir) + "/umap.bin";
    File f_umap = SD.open(path_umap.c_str(), FILE_READ);
    if (f_umap) {
        f_umap.read((uint8_t*)unibus.umap, sizeof(unibus.umap));
        f_umap.close();
    }

    String path_mem = String(dir) + "/mem.bin";
    File f_mem = SD.open(path_mem.c_str(), FILE_READ);
    if (f_mem) {
        int num_pages = 0;
        f_mem.read((uint8_t*)&num_pages, sizeof(num_pages));
        for (int i = 0; i < num_pages; i++) {
            if (unibus.core_pages[i]) {
                f_mem.read((uint8_t*)unibus.core_pages[i], 32768);
            } else {
                f_mem.seek(f_mem.position() + 32768);
            }
        }
        f_mem.close();
    }

    String path_dev = String(dir) + "/devices.bin";
    File f_dev = SD.open(path_dev.c_str(), FILE_READ);
    if (f_dev) {
        unibus.rk11.loadSnapshot(f_dev);
        unibus.rl11.loadSnapshot(f_dev);
        unibus.kw11.loadSnapshot(f_dev);
        unibus.cons.loadSnapshot(f_dev);
        unibus.dl11.loadSnapshot(f_dev);
        f_dev.close();
    }
}


void KB11::printstate() {

    return;


    Serial.printf("R0 %06o R1 %06o R2 %06o R3 %06o R4 %06o R5 %06o R6 %06o R7 "
           "%06o\r\n",
           R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7]);
    Serial.printf("[%s%s%s%s%s%s", previousmode() ? "u" : "k",
           currentmode() ? "U" : "K", N() ? "N" : " ", Z() ? "Z" : " ",
           V() ? "V" : " ", C() ? "C" : " ");
    Serial.printf("]  instr %06o: %06o\t ", PC, read16(PC));
    disasm(PC);
    Serial.printf(" PSW:%06o\r\n",PSW);
}

void KB11::ptstate() {
    Serial.printf("R0 %06o R1 %06o R2 %06o R3 %06o R4 %06o R5 %06o R6 %06o R7 %06o\r\n",
        uint16_t(R[0]), uint16_t(R[1]), uint16_t(R[2]), uint16_t(R[3]), uint16_t(R[4]), uint16_t(R[5]), uint16_t(R[6]), uint16_t(R[7]));
    Serial.printf("[%s%s%s%s%s%s",
        PSW & 0140000 ? "U" : "K",
        PSW & 0030000 ? "U" : "K",
        N() ? "N" : " ",
        Z() ? "Z" : " ",
        V() ? "V" : " ",
        C() ? "C" : " ");
    Serial.printf("]  instr %06o: %06o\t ", PC, read16(PC));

    disasm(PC);

    Serial.printf("  PS:%o\r\n", PSW);
}

