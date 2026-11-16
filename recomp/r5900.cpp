#include "r5900.hpp"

#include <array>
#include <cstdio>

namespace recomp {

namespace {

using enum Op;

constexpr std::array<Op, 64> kPrimary = {
    Invalid, Invalid, J, Jal, Beq, Bne, Blez, Bgtz,
    Addi, Addiu, Slti, Sltiu, Andi, Ori, Xori, Lui,
    Invalid, Invalid, Invalid, Invalid, Beql, Bnel, Blezl, Bgtzl,
    Daddi, Daddiu, Ldl, Ldr, Invalid, Invalid, Lq, Sq,
    Lb, Lh, Lwl, Lw, Lbu, Lhu, Lwr, Lwu,
    Sb, Sh, Swl, Sw, Sdl, Sdr, Swr, Cache,
    Invalid, Lwc1, Invalid, Pref, Invalid, Invalid, Lqc2, Ld,
    Invalid, Swc1, Invalid, Invalid, Invalid, Invalid, Sqc2, Sd,
};

constexpr std::array<Op, 64> kSpecial = {
    Sll, Invalid, Srl, Sra, Sllv, Invalid, Srlv, Srav,
    Jr, Jalr, Movz, Movn, Syscall, Break, Invalid, Sync,
    Mfhi, Mthi, Mflo, Mtlo, Dsllv, Invalid, Dsrlv, Dsrav,
    Mult, Multu, Div, Divu, Invalid, Invalid, Invalid, Invalid,
    Add, Addu, Sub, Subu, And, Or, Xor, Nor,
    Mfsa, Mtsa, Slt, Sltu, Dadd, Daddu, Dsub, Dsubu,
    Tge, Tgeu, Tlt, Tltu, Teq, Invalid, Tne, Invalid,
    Dsll, Invalid, Dsrl, Dsra, Dsll32, Invalid, Dsrl32, Dsra32,
};

constexpr std::array<Op, 32> kRegimm = {
    Bltz, Bgez, Bltzl, Bgezl, Invalid, Invalid, Invalid, Invalid,
    Tgei, Tgeiu, Tlti, Tltiu, Teqi, Invalid, Tnei, Invalid,
    Bltzal, Bgezal, Bltzall, Bgezall, Invalid, Invalid, Invalid, Invalid,
    Mtsab, Mtsah, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<Op, 64> kMmi = {
    Madd, Maddu, Invalid, Invalid, Plzcw, Invalid, Invalid, Invalid,
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    Mfhi1, Mthi1, Mflo1, Mtlo1, Invalid, Invalid, Invalid, Invalid,
    Mult1, Multu1, Div1, Divu1, Invalid, Invalid, Invalid, Invalid,
    Madd1, Maddu1, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    Pmfhl, Pmthl, Invalid, Invalid, Psllh, Invalid, Psrlh, Psrah,
    Invalid, Invalid, Invalid, Invalid, Psllw, Invalid, Psrlw, Psraw,
};

constexpr std::array<Op, 32> kMmi0 = {
    Paddw, Psubw, Pcgtw, Pmaxw, Paddh, Psubh, Pcgth, Pmaxh,
    Paddb, Psubb, Pcgtb, Invalid, Invalid, Invalid, Invalid, Invalid,
    Paddsw, Psubsw, Pextlw, Ppacw, Paddsh, Psubsh, Pextlh, Ppach,
    Paddsb, Psubsb, Pextlb, Ppacb, Invalid, Invalid, Pext5, Ppac5,
};

constexpr std::array<Op, 32> kMmi1 = {
    Invalid, Pabsw, Pceqw, Pminw, Padsbh, Pabsh, Pceqh, Pminh,
    Invalid, Invalid, Pceqb, Invalid, Invalid, Invalid, Invalid, Invalid,
    Padduw, Psubuw, Pextuw, Invalid, Padduh, Psubuh, Pextuh, Invalid,
    Paddub, Psubub, Pextub, Qfsrv, Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<Op, 32> kMmi2 = {
    Pmaddw, Invalid, Psllvw, Psrlvw, Pmsubw, Invalid, Invalid, Invalid,
    Pmfhi, Pmflo, Pinth, Invalid, Pmultw, Pdivw, Pcpyld, Invalid,
    Pmaddh, Phmadh, Pand, Pxor, Pmsubh, Phmsbh, Invalid, Invalid,
    Invalid, Invalid, Pexeh, Prevh, Pmulth, Pdivbw, Pexew, Prot3w,
};

constexpr std::array<Op, 32> kMmi3 = {
    Pmadduw, Invalid, Invalid, Psravw, Invalid, Invalid, Invalid, Invalid,
    Pmthi, Pmtlo, Pinteh, Invalid, Pmultuw, Pdivuw, Pcpyud, Invalid,
    Invalid, Invalid, Por, Pnor, Invalid, Invalid, Invalid, Invalid,
    Invalid, Invalid, Pexch, Pcpyh, Invalid, Invalid, Pexcw, Invalid,
};

constexpr std::array<Op, 64> kCop1S = {
    AddS, SubS, MulS, DivS, SqrtS, AbsS, MovS, NegS,
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, RsqrtS, Invalid,
    AddaS, SubaS, MulaS, Invalid, MaddS, MsubS, MaddaS, MsubaS,
    Invalid, Invalid, Invalid, Invalid, CvtWS, Invalid, Invalid, Invalid,
    MaxS, MinS, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    CF, Invalid, CEq, Invalid, CLt, Invalid, CLe, Invalid,
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<Op, 64> kVector = {
    Vaddbc, Vaddbc, Vaddbc, Vaddbc, Vsubbc, Vsubbc, Vsubbc, Vsubbc,
    Vmaddbc, Vmaddbc, Vmaddbc, Vmaddbc, Vmsubbc, Vmsubbc, Vmsubbc, Vmsubbc,
    Vmaxbc, Vmaxbc, Vmaxbc, Vmaxbc, Vminibc, Vminibc, Vminibc, Vminibc,
    Vmulbc, Vmulbc, Vmulbc, Vmulbc, Vmulq, Vmaxi, Vmuli, Vminii,
    Vaddq, Vmaddq, Vaddi, Vmaddi, Vsubq, Vmsubq, Vsubi, Vmsubi,
    Vadd, Vmadd, Vmul, Vmax, Vsub, Vmsub, Vopmsub, Vmini,
    Viadd, Visub, Viaddi, Invalid, Viand, Vior, Invalid, Invalid,
    Vcallms, Vcallmsr, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<std::array<Op, 32>, 4> kVectorX = {{
    {Vaddabc, Vsubabc, Vmaddabc, Vmsubabc, Vitof0, Vftoi0, Vmulabc, Vmulaq,
     Vaddaq, Vsubaq, Vadda, Vsuba, Vmove, Vlqi, Vdiv, Vmtir,
     Vrnext, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
     Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid},
    {Vaddabc, Vsubabc, Vmaddabc, Vmsubabc, Vitof4, Vftoi4, Vmulabc, Vabs,
     Vmaddaq, Vmsubaq, Vmadda, Vmsuba, Vmr32, Vsqi, Vsqrt, Vmfir,
     Vrget, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
     Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid},
    {Vaddabc, Vsubabc, Vmaddabc, Vmsubabc, Vitof12, Vftoi12, Vmulabc, Vmulai,
     Vaddai, Vsubai, Vmula, Vopmula, Invalid, Vlqd, Vrsqrt, Vilwr,
     Vrinit, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
     Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid},
    {Vaddabc, Vsubabc, Vmaddabc, Vmsubabc, Vitof15, Vftoi15, Vmulabc, Vclip,
     Vmaddai, Vmsubai, Invalid, Vnop, Invalid, Vsqd, Vwaitq, Viswr,
     Vrxor, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
     Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid},
}};

Op decode_mmi(std::uint32_t word) {
    const std::uint32_t funct = word & 63u, sub = (word >> 6) & 31u;
    switch (funct) {
    case 8: return kMmi0[sub];
    case 40: return kMmi1[sub];
    case 9: return kMmi2[sub];
    case 41: return kMmi3[sub];
    case 48: return sub <= 4 ? Pmfhl : Invalid;
    case 49: return sub == 0 ? Pmthl : Invalid;
    default: return kMmi[funct];
    }
}

Op decode_cop0(std::uint32_t word) {
    const std::uint32_t rs = (word >> 21) & 31u;
    if (rs == 0) return Mfc0;
    if (rs == 4) return Mtc0;
    if (rs == 8) {
        constexpr Op kBc[4] = {Bc0f, Bc0t, Bc0fl, Bc0tl};
        const std::uint32_t rt = (word >> 16) & 31u;
        return rt < 4 ? kBc[rt] : Invalid;
    }
    if (rs == 16) {
        switch (word & 63u) {
        case 1: return Tlbr;
        case 2: return Tlbwi;
        case 6: return Tlbwr;
        case 8: return Tlbp;
        case 24: return Eret;
        case 56: return Ei;
        case 57: return Di;
        default: return Invalid;
        }
    }
    return Invalid;
}

Op decode_cop1(std::uint32_t word) {
    switch ((word >> 21) & 31u) {
    case 0: return Mfc1;
    case 2: return Cfc1;
    case 4: return Mtc1;
    case 6: return Ctc1;
    case 8: {
        constexpr Op kBc[4] = {Bc1f, Bc1t, Bc1fl, Bc1tl};
        const std::uint32_t rt = (word >> 16) & 31u;
        return rt < 4 ? kBc[rt] : Invalid;
    }
    case 16: return kCop1S[word & 63u];
    case 20: return (word & 63u) == 32u ? CvtSW : Invalid;
    default: return Invalid;
    }
}

Op decode_cop2(std::uint32_t word) {
    const std::uint32_t rs = (word >> 21) & 31u;
    if (rs >= 16) {
        const std::uint32_t funct = word & 63u;
        if (funct >= 60) return kVectorX[funct & 3u][(word >> 6) & 31u];
        return kVector[funct];
    }
    switch (rs) {
    case 1: return Qmfc2;
    case 2: return Cfc2;
    case 5: return Qmtc2;
    case 6: return Ctc2;
    case 8: {
        constexpr Op kBc[4] = {Bc2f, Bc2t, Bc2fl, Bc2tl};
        const std::uint32_t rt = (word >> 16) & 31u;
        return rt < 4 ? kBc[rt] : Invalid;
    }
    default: return Invalid;
    }
}

constexpr const char *kNames[] = {
#define X(name) #name,
    R5900_OPS(X)
#undef X
};

}

Insn decode(std::uint32_t word, std::uint32_t pc) {
    Insn insn{pc, word, Invalid};
    switch (word >> 26) {
    case 0: insn.op = kSpecial[word & 63u]; break;
    case 1: insn.op = kRegimm[(word >> 16) & 31u]; break;
    case 16: insn.op = decode_cop0(word); break;
    case 17: insn.op = decode_cop1(word); break;
    case 18: insn.op = decode_cop2(word); break;
    case 28: insn.op = decode_mmi(word); break;
    default: insn.op = kPrimary[word >> 26]; break;
    }
    return insn;
}

std::uint32_t Insn::branch_target_of() const {
    return op == J || op == Jal ? jump_target() : branch_target();
}

const char *name(Op op) { return kNames[static_cast<std::size_t>(op)]; }

Flow flow(Op op) {
    switch (op) {
    case Beq: case Bne: case Blez: case Bgtz: case Bltz: case Bgez:
    case Bc0f: case Bc0t: case Bc1f: case Bc1t: case Bc2f: case Bc2t:
        return Flow::Branch;
    case Beql: case Bnel: case Blezl: case Bgtzl: case Bltzl: case Bgezl:
    case Bc0fl: case Bc0tl: case Bc1fl: case Bc1tl: case Bc2fl: case Bc2tl:
        return Flow::BranchLikely;
    case Bltzal: case Bgezal: case Bltzall: case Bgezall:
    case Jal:
        return Flow::Call;
    case J: return Flow::Jump;
    case Jr: return Flow::JumpRegister;
    case Jalr: return Flow::CallRegister;
    case Eret: case Invalid: return Flow::Stop;
    default: return Flow::None;
    }
}

bool has_delay_slot(Op op) {
    const Flow f = flow(op);
    return f != Flow::None && f != Flow::Stop;
}

Vu0Flags vu0_flag_effect(const Insn &insn) {
    switch (insn.op) {
    case Vaddbc: case Vsubbc: case Vmaddbc: case Vmsubbc: case Vmulbc: case Vmulq: case Vmuli:
    case Vaddq: case Vmaddq: case Vaddi: case Vmaddi: case Vsubq: case Vmsubq: case Vsubi: case Vmsubi:
    case Vadd: case Vmadd: case Vmul: case Vsub: case Vmsub: case Vopmsub:
    case Vaddabc: case Vsubabc: case Vmaddabc: case Vmsubabc: case Vmulabc: case Vmulaq: case Vmulai:
    case Vaddaq: case Vmaddaq: case Vaddai: case Vmaddai: case Vsubaq: case Vmsubaq: case Vsubai: case Vmsubai:
    case Vadda: case Vmadda: case Vmula: case Vsuba: case Vmsuba: case Vopmula:
        return Vu0Flags::Sets;

    // The vector unit's instructions that work on its registers only. VDIV, VSQRT and VRSQRT change the two division
    // bits of the status register and leave the others as they are; VCLIP has its own register.
    case Vmaxbc: case Vminibc: case Vmaxi: case Vminii: case Vmax: case Vmini:
    case Viadd: case Visub: case Viaddi: case Viand: case Vior:
    case Vitof0: case Vitof4: case Vitof12: case Vitof15: case Vftoi0: case Vftoi4: case Vftoi12: case Vftoi15:
    case Vabs: case Vclip: case Vnop: case Vmove: case Vmr32: case Vdiv: case Vsqrt: case Vrsqrt: case Vwaitq:
    case Vmtir: case Vmfir: case Vrnext: case Vrget: case Vrinit: case Vrxor:
        return Vu0Flags::Keeps;
    // With the interlock bit they wait for a running vector program, which can read the flags.
    case Qmfc2: case Qmtc2:
        return (insn.word & 1u) ? Vu0Flags::Reads : Vu0Flags::Keeps;
    // The integer registers are plain registers; from 16 up they are the control registers, the flags among them.
    case Cfc2: case Ctc2:
        return insn.fs() < 16 ? Vu0Flags::Keeps : Vu0Flags::Reads;

    // Integer work on registers, without the instructions that can trap.
    case Sll: case Srl: case Sra: case Sllv: case Srlv: case Srav: case Movz: case Movn:
    case Mfhi: case Mthi: case Mflo: case Mtlo: case Dsllv: case Dsrlv: case Dsrav:
    case Addu: case Subu: case And: case Or: case Xor: case Nor: case Slt: case Sltu: case Daddu: case Dsubu:
    case Dsll: case Dsrl: case Dsra: case Dsll32: case Dsrl32: case Dsra32:
    case Addiu: case Slti: case Sltiu: case Andi: case Ori: case Xori: case Lui: case Daddiu:
    // The main processor's own float unit: its registers and its own flag register.
    case Mfc1: case Mtc1: case AddS: case SubS: case MulS: case DivS: case SqrtS: case AbsS: case MovS: case NegS: case RsqrtS:
    case AddaS: case SubaS: case MulaS: case MaddS: case MsubS: case MaddaS: case MsubaS: case CvtWS: case CvtSW:
    case MaxS: case MinS: case CF: case CEq: case CLt: case CLe:
        return Vu0Flags::Keeps;

    default:
        return Vu0Flags::Reads;
    }
}

std::string disassemble(const Insn &insn) {
    char text[96];
    std::snprintf(text, sizeof text, "%08X: %08X %s", insn.pc, insn.word, name(insn.op));
    return text;
}

}
