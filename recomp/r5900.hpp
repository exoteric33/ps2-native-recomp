#pragma once

#include <cstdint>
#include <string>

namespace recomp {

#define R5900_OPS(X) \
    X(Invalid) \
    X(Sll) X(Srl) X(Sra) X(Sllv) X(Srlv) X(Srav) X(Jr) X(Jalr) X(Movz) X(Movn) X(Syscall) X(Break) X(Sync) \
    X(Mfhi) X(Mthi) X(Mflo) X(Mtlo) X(Dsllv) X(Dsrlv) X(Dsrav) X(Mult) X(Multu) X(Div) X(Divu) \
    X(Add) X(Addu) X(Sub) X(Subu) X(And) X(Or) X(Xor) X(Nor) X(Mfsa) X(Mtsa) X(Slt) X(Sltu) \
    X(Dadd) X(Daddu) X(Dsub) X(Dsubu) X(Tge) X(Tgeu) X(Tlt) X(Tltu) X(Teq) X(Tne) \
    X(Dsll) X(Dsrl) X(Dsra) X(Dsll32) X(Dsrl32) X(Dsra32) \
    X(Bltz) X(Bgez) X(Bltzl) X(Bgezl) X(Tgei) X(Tgeiu) X(Tlti) X(Tltiu) X(Teqi) X(Tnei) \
    X(Bltzal) X(Bgezal) X(Bltzall) X(Bgezall) X(Mtsab) X(Mtsah) \
    X(J) X(Jal) X(Beq) X(Bne) X(Blez) X(Bgtz) X(Addi) X(Addiu) X(Slti) X(Sltiu) X(Andi) X(Ori) X(Xori) X(Lui) \
    X(Beql) X(Bnel) X(Blezl) X(Bgtzl) X(Daddi) X(Daddiu) X(Ldl) X(Ldr) X(Lq) X(Sq) \
    X(Lb) X(Lh) X(Lwl) X(Lw) X(Lbu) X(Lhu) X(Lwr) X(Lwu) X(Sb) X(Sh) X(Swl) X(Sw) X(Sdl) X(Sdr) X(Swr) \
    X(Cache) X(Lwc1) X(Pref) X(Lqc2) X(Ld) X(Swc1) X(Sqc2) X(Sd) \
    X(Madd) X(Maddu) X(Plzcw) X(Mfhi1) X(Mthi1) X(Mflo1) X(Mtlo1) X(Mult1) X(Multu1) X(Div1) X(Divu1) \
    X(Madd1) X(Maddu1) X(Pmfhl) X(Pmthl) X(Psllh) X(Psrlh) X(Psrah) X(Psllw) X(Psrlw) X(Psraw) \
    X(Paddw) X(Psubw) X(Pcgtw) X(Pmaxw) X(Paddh) X(Psubh) X(Pcgth) X(Pmaxh) X(Paddb) X(Psubb) X(Pcgtb) \
    X(Paddsw) X(Psubsw) X(Pextlw) X(Ppacw) X(Paddsh) X(Psubsh) X(Pextlh) X(Ppach) \
    X(Paddsb) X(Psubsb) X(Pextlb) X(Ppacb) X(Pext5) X(Ppac5) \
    X(Pabsw) X(Pceqw) X(Pminw) X(Padsbh) X(Pabsh) X(Pceqh) X(Pminh) X(Pceqb) \
    X(Padduw) X(Psubuw) X(Pextuw) X(Padduh) X(Psubuh) X(Pextuh) X(Paddub) X(Psubub) X(Pextub) X(Qfsrv) \
    X(Pmaddw) X(Psllvw) X(Psrlvw) X(Pmsubw) X(Pmfhi) X(Pmflo) X(Pinth) X(Pmultw) X(Pdivw) X(Pcpyld) \
    X(Pmaddh) X(Phmadh) X(Pand) X(Pxor) X(Pmsubh) X(Phmsbh) X(Pexeh) X(Prevh) X(Pmulth) X(Pdivbw) X(Pexew) X(Prot3w) \
    X(Pmadduw) X(Psravw) X(Pmthi) X(Pmtlo) X(Pinteh) X(Pmultuw) X(Pdivuw) X(Pcpyud) X(Por) X(Pnor) \
    X(Pexch) X(Pcpyh) X(Pexcw) \
    X(Mfc0) X(Mtc0) X(Bc0f) X(Bc0t) X(Bc0fl) X(Bc0tl) X(Tlbr) X(Tlbwi) X(Tlbwr) X(Tlbp) X(Eret) X(Ei) X(Di) \
    X(Mfc1) X(Cfc1) X(Mtc1) X(Ctc1) X(Bc1f) X(Bc1t) X(Bc1fl) X(Bc1tl) \
    X(AddS) X(SubS) X(MulS) X(DivS) X(SqrtS) X(AbsS) X(MovS) X(NegS) X(RsqrtS) X(AddaS) X(SubaS) X(MulaS) \
    X(MaddS) X(MsubS) X(MaddaS) X(MsubaS) X(CvtWS) X(MaxS) X(MinS) X(CF) X(CEq) X(CLt) X(CLe) X(CvtSW) \
    X(Qmfc2) X(Cfc2) X(Qmtc2) X(Ctc2) X(Bc2f) X(Bc2t) X(Bc2fl) X(Bc2tl) \
    X(Vaddbc) X(Vsubbc) X(Vmaddbc) X(Vmsubbc) X(Vmaxbc) X(Vminibc) X(Vmulbc) X(Vmulq) X(Vmaxi) X(Vmuli) X(Vminii) \
    X(Vaddq) X(Vmaddq) X(Vaddi) X(Vmaddi) X(Vsubq) X(Vmsubq) X(Vsubi) X(Vmsubi) \
    X(Vadd) X(Vmadd) X(Vmul) X(Vmax) X(Vsub) X(Vmsub) X(Vopmsub) X(Vmini) \
    X(Viadd) X(Visub) X(Viaddi) X(Viand) X(Vior) X(Vcallms) X(Vcallmsr) \
    X(Vaddabc) X(Vsubabc) X(Vmaddabc) X(Vmsubabc) X(Vitof0) X(Vitof4) X(Vitof12) X(Vitof15) \
    X(Vftoi0) X(Vftoi4) X(Vftoi12) X(Vftoi15) X(Vmulabc) X(Vmulaq) X(Vabs) X(Vmulai) X(Vclip) \
    X(Vaddaq) X(Vmaddaq) X(Vaddai) X(Vmaddai) X(Vsubaq) X(Vmsubaq) X(Vsubai) X(Vmsubai) \
    X(Vadda) X(Vmadda) X(Vmula) X(Vsuba) X(Vmsuba) X(Vopmula) X(Vnop) X(Vmove) X(Vmr32) \
    X(Vlqi) X(Vsqi) X(Vlqd) X(Vsqd) X(Vdiv) X(Vsqrt) X(Vrsqrt) X(Vwaitq) X(Vmtir) X(Vmfir) X(Vilwr) X(Viswr) \
    X(Vrnext) X(Vrget) X(Vrinit) X(Vrxor)

enum class Op : std::uint16_t {
#define X(name) name,
    R5900_OPS(X)
#undef X
    Count
};

struct Insn {
    std::uint32_t pc{};
    std::uint32_t word{};
    Op op{Op::Invalid};

    std::uint32_t rs() const { return (word >> 21) & 31u; }
    std::uint32_t rt() const { return (word >> 16) & 31u; }
    std::uint32_t rd() const { return (word >> 11) & 31u; }
    std::uint32_t sa() const { return (word >> 6) & 31u; }
    std::uint32_t fs() const { return rd(); }
    std::uint32_t ft() const { return rt(); }
    std::uint32_t fd() const { return sa(); }
    std::int32_t simm() const { return static_cast<std::int16_t>(word & 0xFFFFu); }
    std::uint32_t uimm() const { return word & 0xFFFFu; }
    std::uint32_t jump_target() const { return ((pc + 4u) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2); }
    std::uint32_t branch_target() const { return pc + 4u + (static_cast<std::uint32_t>(simm()) << 2); }
    std::uint32_t branch_target_of() const;
    std::uint32_t dest() const { return (word >> 21) & 15u; }
    std::uint32_t bc() const { return word & 3u; }
    std::int32_t imm5() const { return static_cast<std::int32_t>(sa() << 27) >> 27; }
    std::uint32_t imm15() const { return (word >> 6) & 0x7FFFu; }
};

enum class Flow { None, Branch, BranchLikely, Jump, Call, JumpRegister, CallRegister, Stop };

Insn decode(std::uint32_t word, std::uint32_t pc);
const char *name(Op op);
Flow flow(Op op);
bool has_delay_slot(Op op);
std::string disassemble(const Insn &insn);

// What an instruction does to the flags of the first vector unit that its macro instructions set (the MAC register
// and the low four bits of the status register; the sticky status bits are not part of this, they are always kept):
//   Sets   it writes all of them, whatever they were (the arithmetic of COP2, with any destination mask);
//   Keeps  it cannot read them, cannot leave the straight line and cannot let anything else look at the machine: work
//          on registers only (the vector unit's other instructions, plain integer and float arithmetic);
//   Reads  everything else. Not only CFC2, CTC2 and VCALLMS: also every load and store (a hardware register can start
//          anything), every syscall, trap and branch, and whatever this list does not know.
enum class Vu0Flags { Sets, Keeps, Reads };
Vu0Flags vu0_flag_effect(const Insn &insn);

// True when the flags that the instruction at `pc` sets cannot be seen by anything: on the straight line after it
// another instruction sets them again, with nothing but Keeps instructions in between. `next(pc, insn)` fills in the
// instruction at an address and answers false where the straight line ends (a label, a branch, the end of the code).
template <typename Next>
bool vu0_flags_dead(std::uint32_t pc, Next &&next) {
    for (int k = 0; k < 64; ++k) {
        pc += 4u;
        Insn insn;
        if (!next(pc, insn)) return false;
        switch (vu0_flag_effect(insn)) {
        case Vu0Flags::Sets: return true;
        case Vu0Flags::Keeps: break;
        case Vu0Flags::Reads: return false;
        }
    }
    return false;
}

}
