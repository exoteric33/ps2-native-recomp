#include "interp.hpp"

#include "ps2/generated.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include "r5900.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// The main processor interpreter.
//
// A game reaches code the build has no translation for (another stage, another path through the menus): the translated program stops
// there, or the game waits while the code is translated and compiled (ee_live.cpp). This runs that code instead, one instruction at a
// time, and hands control back to translated code at the first place that has some.
//
// It does what recomp/codegen.cpp writes for the same instructions, and charges cycles the way the translated code does, so that a game
// goes the same way (the same state, the same emulated time) whichever of the two ran a piece of code:
//   - a plain instruction costs 1, a branch with its delay slot costs 2, whether the branch is taken or not;
//   - translated code adds the plain instructions up and charges them in blocks: at the next branch (together with the branch's 2), before
//     a syscall, before COP0 register 25 (the performance counters), at the entry of a block that a branch can reach (a label) and at
//     the end of a 16 KiB unit. Hardware that is read or written in the middle of a block (a timer, a DMA channel) sees the time of the
//     last charge, not the exact instruction. So that the interpreter shows hardware the same time, it keeps the same count and charges at
//     the same places: a label is a target of a branch or jump that the scan of the 16 KiB around it finds (labels_for);
//   - the preemption point of a backward branch (budget below zero: preempt()) is the same.
// The first port of this was upstream's pull request 33 (commit 686d9aa, "An EE interpreter for code the translation lacks", MIT), which
// follows codegen.cpp the same way; this one follows our codegen.cpp (floating point flags, the performance counters, branches in delay
// slots, the cycle count).
namespace ps2 {

namespace {

using recomp::Flow;
using recomp::Insn;
using enum recomp::Op;

// ---------------------------------------------------------------------------------------------------------------- registers

std::uint32_t w(const Cpu &c, std::uint32_t r) { return r ? c.r[r].w[0] : 0u; }
std::uint64_t d(const Cpu &c, std::uint32_t r) { return r ? c.r[r].d[0] : 0ull; }
std::int64_t sd(const Cpu &c, std::uint32_t r) { return r ? c.r[r].sd[0] : 0ll; }
void set_w(Cpu &c, std::uint32_t r, std::uint32_t v) {
    if (r) set32(c.r[r], v);
}
void set_d(Cpu &c, std::uint32_t r, std::uint64_t v) {
    if (r) c.r[r].d[0] = v;
}
std::uint32_t addr(const Cpu &c, const Insn &i) { return w(c, i.rs()) + static_cast<std::uint32_t>(i.simm()); }
void link(Cpu &c, std::uint32_t r, std::uint32_t ret) {
    if (r) c.r[r].sd[0] = static_cast<std::int32_t>(ret);
}

bool condition(Cpu &c, const Insn &i) {
    switch (i.op) {
    case Beq: case Beql: return d(c, i.rs()) == d(c, i.rt());
    case Bne: case Bnel: return d(c, i.rs()) != d(c, i.rt());
    case Blez: case Blezl: return sd(c, i.rs()) <= 0;
    case Bgtz: case Bgtzl: return sd(c, i.rs()) > 0;
    case Bltz: case Bltzl: case Bltzal: case Bltzall: return sd(c, i.rs()) < 0;
    case Bgez: case Bgezl: case Bgezal: case Bgezall: return sd(c, i.rs()) >= 0;
    case Bc0f: case Bc0fl: return !cpcond0(c);
    case Bc0t: case Bc0tl: return cpcond0(c);
    case Bc1f: case Bc1fl: return !(c.fcr31 & kFpuCondition);
    case Bc1t: case Bc1tl: return (c.fcr31 & kFpuCondition) != 0;
    case Bc2f: case Bc2fl: return !vu0_busy(c);
    case Bc2t: case Bc2tl: return vu0_busy(c);
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------- plain instructions

// Executes one instruction that does not change the flow of control, the way codegen.cpp's simple() writes it. False when the game was
// stopped (an instruction the translator does not do either).
bool simple(Cpu &c, const Insn &i) {
    const std::uint32_t rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
    const std::uint32_t fsf = (i.word >> 21) & 3u, ftf = (i.word >> 23) & 3u;
    Reg128 scratch;
    Reg128 &dest = rd ? c.r[rd] : scratch;
    auto &v = c.vu0;
    switch (i.op) {
    case Sll: set_w(c, rd, w(c, rt) << sa); break;
    case Srl: set_w(c, rd, w(c, rt) >> sa); break;
    case Sra: set_w(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(w(c, rt)) >> sa)); break;
    case Sllv: set_w(c, rd, w(c, rt) << (w(c, rs) & 31u)); break;
    case Srlv: set_w(c, rd, w(c, rt) >> (w(c, rs) & 31u)); break;
    case Srav: set_w(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(w(c, rt)) >> (w(c, rs) & 31u))); break;
    case Movz: if (rd && d(c, rt) == 0) c.r[rd].d[0] = d(c, rs); break;
    case Movn: if (rd && d(c, rt) != 0) c.r[rd].d[0] = d(c, rs); break;
    case Break: breakpoint(c, i.pc); break;
    case Sync: case Cache: case Pref: break;
    case Mfhi: set_d(c, rd, c.hi.d[0]); break;
    case Mflo: set_d(c, rd, c.lo.d[0]); break;
    case Mfhi1: set_d(c, rd, c.hi.d[1]); break;
    case Mflo1: set_d(c, rd, c.lo.d[1]); break;
    case Mthi: c.hi.d[0] = d(c, rs); break;
    case Mtlo: c.lo.d[0] = d(c, rs); break;
    case Mthi1: c.hi.d[1] = d(c, rs); break;
    case Mtlo1: c.lo.d[1] = d(c, rs); break;
    case Dsllv: set_d(c, rd, d(c, rt) << (w(c, rs) & 63u)); break;
    case Dsrlv: set_d(c, rd, d(c, rt) >> (w(c, rs) & 63u)); break;
    case Dsrav: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> (w(c, rs) & 63u))); break;
    case Mult: mmi::mult(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Multu: mmi::multu(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Madd: mmi::madd(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Maddu: mmi::maddu(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Mult1: mmi::mult(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Multu1: mmi::multu(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Madd1: mmi::madd(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Maddu1: mmi::maddu(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Div: mmi::div(c, 0, w(c, rs), w(c, rt)); break;
    case Divu: mmi::divu(c, 0, w(c, rs), w(c, rt)); break;
    case Div1: mmi::div(c, 1, w(c, rs), w(c, rt)); break;
    case Divu1: mmi::divu(c, 1, w(c, rs), w(c, rt)); break;
    case Add: case Addu: set_w(c, rd, w(c, rs) + w(c, rt)); break;
    case Sub: case Subu: set_w(c, rd, w(c, rs) - w(c, rt)); break;
    case And: set_d(c, rd, d(c, rs) & d(c, rt)); break;
    case Or: set_d(c, rd, d(c, rs) | d(c, rt)); break;
    case Xor: set_d(c, rd, d(c, rs) ^ d(c, rt)); break;
    case Nor: set_d(c, rd, ~(d(c, rs) | d(c, rt))); break;
    case Mfsa: set_d(c, rd, c.sa); break;
    case Mtsa: c.sa = w(c, rs) & 15u; break;
    case Mtsab: c.sa = (w(c, rs) ^ i.uimm()) & 15u; break;
    case Mtsah: c.sa = ((w(c, rs) ^ i.uimm()) & 7u) * 2u; break;
    case Slt: set_d(c, rd, sd(c, rs) < sd(c, rt) ? 1ull : 0ull); break;
    case Sltu: set_d(c, rd, d(c, rs) < d(c, rt) ? 1ull : 0ull); break;
    case Dadd: case Daddu: set_d(c, rd, d(c, rs) + d(c, rt)); break;
    case Dsub: case Dsubu: set_d(c, rd, d(c, rs) - d(c, rt)); break;
    case Tge: if (sd(c, rs) >= sd(c, rt)) trap(c, i.pc); break;
    case Tgeu: if (d(c, rs) >= d(c, rt)) trap(c, i.pc); break;
    case Tlt: if (sd(c, rs) < sd(c, rt)) trap(c, i.pc); break;
    case Tltu: if (d(c, rs) < d(c, rt)) trap(c, i.pc); break;
    case Teq: if (d(c, rs) == d(c, rt)) trap(c, i.pc); break;
    case Tne: if (d(c, rs) != d(c, rt)) trap(c, i.pc); break;
    case Tgei: if (sd(c, rs) >= i.simm()) trap(c, i.pc); break;
    case Tgeiu: if (d(c, rs) >= static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Tlti: if (sd(c, rs) < i.simm()) trap(c, i.pc); break;
    case Tltiu: if (d(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Teqi: if (sd(c, rs) == i.simm()) trap(c, i.pc); break;
    case Tnei: if (sd(c, rs) != i.simm()) trap(c, i.pc); break;
    case Dsll: set_d(c, rd, d(c, rt) << sa); break;
    case Dsrl: set_d(c, rd, d(c, rt) >> sa); break;
    case Dsra: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> sa)); break;
    case Dsll32: set_d(c, rd, d(c, rt) << (sa + 32)); break;
    case Dsrl32: set_d(c, rd, d(c, rt) >> (sa + 32)); break;
    case Dsra32: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> (sa + 32))); break;
    case Addi: case Addiu: set_w(c, rt, w(c, rs) + static_cast<std::uint32_t>(i.simm())); break;
    case Slti: set_d(c, rt, sd(c, rs) < i.simm() ? 1ull : 0ull); break;
    case Sltiu: set_d(c, rt, d(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm())) ? 1ull : 0ull); break;
    case Andi: set_d(c, rt, d(c, rs) & i.uimm()); break;
    case Ori: set_d(c, rt, d(c, rs) | i.uimm()); break;
    case Xori: set_d(c, rt, d(c, rs) ^ i.uimm()); break;
    case Lui: if (rt) c.r[rt].sd[0] = static_cast<std::int32_t>(i.uimm() << 16); break;
    case Daddi: case Daddiu: set_d(c, rt, d(c, rs) + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))); break;
    case Ldl: if (rt) ldl(c.r[rt], addr(c, i)); break;
    case Ldr: if (rt) ldr(c.r[rt], addr(c, i)); break;
    case Lwl: if (rt) lwl(c.r[rt], addr(c, i)); break;
    case Lwr: if (rt) lwr(c.r[rt], addr(c, i)); break;
    case Sdl: sdl(c.r[rt], addr(c, i)); break;
    case Sdr: sdr(c.r[rt], addr(c, i)); break;
    case Swl: swl(c.r[rt], addr(c, i)); break;
    case Swr: swr(c.r[rt], addr(c, i)); break;
    case Lq: ld128(addr(c, i) & ~15u, rt ? c.r[rt] : scratch); break;
    case Sq: st128(addr(c, i) & ~15u, c.r[rt]); break;
    case Lb: { const auto x = ld8(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int8_t>(x); break; }
    case Lbu: { const auto x = ld8(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lh: { const auto x = ld16(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int16_t>(x); break; }
    case Lhu: { const auto x = ld16(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lw: { const auto x = ld32(addr(c, i)); if (rt) set32(c.r[rt], x); break; }
    case Lwu: { const auto x = ld32(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Ld: { const auto x = ld64(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Sb: st8(addr(c, i), static_cast<std::uint8_t>(w(c, rt))); break;
    case Sh: st16(addr(c, i), static_cast<std::uint16_t>(w(c, rt))); break;
    case Sw: st32(addr(c, i), w(c, rt)); break;
    case Sd: st64(addr(c, i), d(c, rt)); break;
    case Lwc1: c.f[i.ft()] = ld32(addr(c, i)); break;
    case Swc1: st32(addr(c, i), c.f[i.ft()]); break;
    case Lqc2: ld128(addr(c, i) & ~15u, i.ft() ? c.vu0.vf[i.ft()] : scratch); break;
    case Sqc2: st128(addr(c, i) & ~15u, c.vu0.vf[i.ft()]); break;

    case Plzcw: if (rd) mmi::plzcw(c.r[rd], c.r[rs]); break;
    case Pmfhl: if (rd) mmi::pmfhl(c, c.r[rd], sa); break;
    case Pmthl: mmi::pmthl(c, c.r[rs]); break;
    case Psllh: if (rd) mmi::psllh(c.r[rd], c.r[rt], sa); break;
    case Psrlh: if (rd) mmi::psrlh(c.r[rd], c.r[rt], sa); break;
    case Psrah: if (rd) mmi::psrah(c.r[rd], c.r[rt], sa); break;
    case Psllw: if (rd) mmi::psllw(c.r[rd], c.r[rt], sa); break;
    case Psrlw: if (rd) mmi::psrlw(c.r[rd], c.r[rt], sa); break;
    case Psraw: if (rd) mmi::psraw(c.r[rd], c.r[rt], sa); break;
#define MMI3(op, fn) case op: if (rd) mmi::fn(c.r[rd], c.r[rs], c.r[rt]); break;
#define MMI2(op, fn) case op: if (rd) mmi::fn(c.r[rd], c.r[rt]); break;
    MMI3(Paddw, paddw) MMI3(Psubw, psubw) MMI3(Pcgtw, pcgtw) MMI3(Pmaxw, pmaxw) MMI3(Paddh, paddh) MMI3(Psubh, psubh)
    MMI3(Pcgth, pcgth) MMI3(Pmaxh, pmaxh) MMI3(Paddb, paddb) MMI3(Psubb, psubb) MMI3(Pcgtb, pcgtb) MMI3(Paddsw, paddsw)
    MMI3(Psubsw, psubsw) MMI3(Pextlw, pextlw) MMI3(Ppacw, ppacw) MMI3(Paddsh, paddsh) MMI3(Psubsh, psubsh) MMI3(Pextlh, pextlh)
    MMI3(Ppach, ppach) MMI3(Paddsb, paddsb) MMI3(Psubsb, psubsb) MMI3(Pextlb, pextlb) MMI3(Ppacb, ppacb)
    MMI2(Pext5, pext5) MMI2(Ppac5, ppac5) MMI2(Pabsw, pabsw) MMI3(Pceqw, pceqw) MMI3(Pminw, pminw) MMI3(Padsbh, padsbh)
    MMI2(Pabsh, pabsh) MMI3(Pceqh, pceqh) MMI3(Pminh, pminh) MMI3(Pceqb, pceqb) MMI3(Padduw, padduw) MMI3(Psubuw, psubuw)
    MMI3(Pextuw, pextuw) MMI3(Padduh, padduh) MMI3(Psubuh, psubuh) MMI3(Pextuh, pextuh) MMI3(Paddub, paddub)
    MMI3(Psubub, psubub) MMI3(Pextub, pextub) MMI3(Psllvw, psllvw) MMI3(Psrlvw, psrlvw) MMI3(Psravw, psravw)
    MMI3(Pinth, pinth) MMI3(Pinteh, pinteh) MMI3(Pcpyld, pcpyld) MMI3(Pcpyud, pcpyud) MMI3(Pand, pand) MMI3(Pxor, pxor)
    MMI3(Por, por) MMI3(Pnor, pnor) MMI2(Pexeh, pexeh) MMI2(Prevh, prevh) MMI2(Pexew, pexew) MMI2(Prot3w, prot3w)
    MMI2(Pexch, pexch) MMI2(Pcpyh, pcpyh) MMI2(Pexcw, pexcw)
#undef MMI3
#undef MMI2
    case Qfsrv: if (rd) mmi::qfsrv(c.r[rd], c.r[rs], c.r[rt], c.sa); break;
    case Pmaddw: case Pmsubw: mmi::pmaddw(c, dest, c.r[rs], c.r[rt], i.op == Pmsubw); break;
    case Pmadduw: mmi::pmadduw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmultw: mmi::pmultw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmultuw: mmi::pmultuw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmulth: case Pmaddh: case Pmsubh: mmi::pmulth(c, dest, c.r[rs], c.r[rt], i.op == Pmulth ? 0 : i.op == Pmaddh ? 1 : 2); break;
    case Phmadh: case Phmsbh: mmi::phmadh(c, dest, c.r[rs], c.r[rt], i.op == Phmsbh); break;
    case Pdivw: mmi::pdivw(c, c.r[rs], c.r[rt]); break;
    case Pdivuw: mmi::pdivuw(c, c.r[rs], c.r[rt]); break;
    case Pdivbw: mmi::pdivbw(c, c.r[rs], c.r[rt]); break;
    case Pmfhi: if (rd) c.r[rd] = c.hi; break;
    case Pmflo: if (rd) c.r[rd] = c.lo; break;
    case Pmthi: c.hi = c.r[rs]; break;
    case Pmtlo: c.lo = c.r[rs]; break;

    case Mfc0: set_w(c, rt, rd == 25 ? mfperf(c, i.word & 63u) : mfc0(c, rd, i.word & 7u)); break;
    case Mtc0:
        if (rd == 25) mtperf(c, i.word & 63u, w(c, rt));
        else mtc0(c, rd, w(c, rt));
        break;
    case Ei: set_interrupts(c, true); break;
    case Di: set_interrupts(c, false); break;
    case Tlbr: case Tlbwi: case Tlbwr: case Tlbp:
        stop(c, i.pc, recomp::name(i.op));
        return false;

    case Mfc1: set_w(c, rt, c.f[i.fs()]); break;
    case Mtc1: c.f[i.fs()] = w(c, rt); break;
    case Cfc1: set_w(c, rt, cfc1(c, i.fs())); break;
    case Ctc1: ctc1(c, i.fs(), w(c, rt)); break;
    case AddS: c.f[i.fd()] = fp::ee::add(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case SubS: c.f[i.fd()] = fp::ee::sub(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case MulS: c.f[i.fd()] = fp::ee::mul(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case DivS: c.f[i.fd()] = fp::ee::div(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case SqrtS: c.f[i.fd()] = fp::ee::sqrt(c.fcr31, c.f[i.ft()]); break;
    case AbsS: c.f[i.fd()] = c.f[i.fs()] & fp::kAbs; break;
    case MovS: c.f[i.fd()] = c.f[i.fs()]; break;
    case NegS: c.f[i.fd()] = c.f[i.fs()] ^ fp::kSign; break;
    case RsqrtS: c.f[i.fd()] = fp::ee::rsqrt(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case AddaS: c.facc = fp::ee::add(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case SubaS: c.facc = fp::ee::sub(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case MulaS: c.facc = fp::ee::mul(c.fcr31, c.f[i.fs()], c.f[i.ft()]); break;
    case MaddS: c.f[i.fd()] = fp::ee::madd(c.fcr31, c.facc, c.f[i.fs()], c.f[i.ft()]); break;
    case MsubS: c.f[i.fd()] = fp::ee::msub(c.fcr31, c.facc, c.f[i.fs()], c.f[i.ft()]); break;
    case MaddaS: c.facc = fp::ee::madd(c.fcr31, c.facc, c.f[i.fs()], c.f[i.ft()]); break;
    case MsubaS: c.facc = fp::ee::msub(c.fcr31, c.facc, c.f[i.fs()], c.f[i.ft()]); break;
    case CvtWS: c.f[i.fd()] = fp::cvt_w_s(c.f[i.fs()]); break;
    case CvtSW: c.f[i.fd()] = fp::cvt_s_w(c.f[i.fs()]); break;
    case MaxS: c.f[i.fd()] = fp::max(c.f[i.fs()], c.f[i.ft()]); break;
    case MinS: c.f[i.fd()] = fp::min(c.f[i.fs()], c.f[i.ft()]); break;
    case CF: fpu_compare(c, false); break;
    case CEq: fpu_compare(c, fp::key(c.f[i.fs()]) == fp::key(c.f[i.ft()])); break;
    case CLt: fpu_compare(c, fp::key(c.f[i.fs()]) < fp::key(c.f[i.ft()])); break;
    case CLe: fpu_compare(c, fp::key(c.f[i.fs()]) <= fp::key(c.f[i.ft()])); break;

    case Qmfc2:
        if (i.word & 1u) vu0_wait(c);
        if (rt) c.r[rt] = v.vf[i.fs()];
        break;
    case Qmtc2:
        if (i.word & 1u) vu0_wait(c);
        if (i.fs()) v.vf[i.fs()] = c.r[rt];
        break;
    case Cfc2: set_w(c, rt, i.fs() < 16 ? v.vi[i.fs()] : cfc2(c, i.fs())); break;
    case Ctc2:
        if (i.fs() < 16) vu::set_vi(v, i.fs(), w(c, rt));
        else ctc2(c, i.fs(), w(c, rt));
        break;
    case Vaddbc: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vsubbc: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmaddbc: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmsubbc: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmaxbc: vu::maxbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vminibc: vu::minibc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmulbc: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmulq: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaxi: vu::maxbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmuli: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vminii: vu::minibc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vaddq: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaddq: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vaddi: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmaddi: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vsubq: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmsubq: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vsubi: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmsubi: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vadd: vu::add(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmadd: vu::madd(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmul: vu::mul(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmax: vu::max(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vsub: vu::sub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmsub: vu::msub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmini: vu::mini(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vopmsub: vu::opmsub(v, i.fd(), i.fs(), i.ft()); break;
    case Viadd: vu::set_vi(v, i.fd(), v.vi[i.fs()] + v.vi[i.ft()]); break;
    case Visub: vu::set_vi(v, i.fd(), v.vi[i.fs()] - v.vi[i.ft()]); break;
    case Viaddi: vu::set_vi(v, i.ft(), v.vi[i.fs()] + i.imm5()); break;
    case Viand: vu::set_vi(v, i.fd(), v.vi[i.fs()] & v.vi[i.ft()]); break;
    case Vior: vu::set_vi(v, i.fd(), v.vi[i.fs()] | v.vi[i.ft()]); break;
    case Vcallms: vcallms(c, i.imm15() * 8u); break;
    case Vcallmsr: vcallms(c, v.cmsar0 * 8u); break;
    case Vaddabc: vu::addabc(v, i.dest(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vsubabc: vu::subabc(v, i.dest(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmaddabc: vu::maddabc(v, i.dest(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmsubabc: vu::msubabc(v, i.dest(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmulabc: vu::mulabc(v, i.dest(), i.fs(), v.vf[i.ft()].w[i.bc()]); break;
    case Vmulaq: vu::mulabc(v, i.dest(), i.fs(), v.q); break;
    case Vmulai: vu::mulabc(v, i.dest(), i.fs(), v.i); break;
    case Vaddaq: vu::addabc(v, i.dest(), i.fs(), v.q); break;
    case Vmaddaq: vu::maddabc(v, i.dest(), i.fs(), v.q); break;
    case Vaddai: vu::addabc(v, i.dest(), i.fs(), v.i); break;
    case Vmaddai: vu::maddabc(v, i.dest(), i.fs(), v.i); break;
    case Vsubaq: vu::subabc(v, i.dest(), i.fs(), v.q); break;
    case Vmsubaq: vu::msubabc(v, i.dest(), i.fs(), v.q); break;
    case Vsubai: vu::subabc(v, i.dest(), i.fs(), v.i); break;
    case Vmsubai: vu::msubabc(v, i.dest(), i.fs(), v.i); break;
    case Vadda: vu::adda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmadda: vu::madda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmula: vu::mula(v, i.dest(), i.fs(), i.ft()); break;
    case Vsuba: vu::suba(v, i.dest(), i.fs(), i.ft()); break;
    case Vmsuba: vu::msuba(v, i.dest(), i.fs(), i.ft()); break;
    case Vopmula: vu::opmula(v, i.fs(), i.ft()); break;
    case Vitof0: vu::itof(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vitof4: vu::itof(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vitof12: vu::itof(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vitof15: vu::itof(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vftoi0: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vftoi4: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vftoi12: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vftoi15: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vabs: vu::abs(v, i.dest(), i.ft(), i.fs()); break;
    case Vmove: vu::move(v, i.dest(), i.ft(), i.fs()); break;
    case Vmr32: vu::mr32(v, i.dest(), i.ft(), i.fs()); break;
    case Vclip: vu::clip(v, i.fs(), i.ft()); break;
    case Vnop: case Vwaitq: break;
    case Vlqi: lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); vu::set_vi(v, i.fs(), v.vi[i.fs()] + 1u); break;
    case Vsqi: sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); vu::set_vi(v, i.ft(), v.vi[i.ft()] + 1u); break;
    case Vlqd: vu::set_vi(v, i.fs(), v.vi[i.fs()] - 1u); lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); break;
    case Vsqd: vu::set_vi(v, i.ft(), v.vi[i.ft()] - 1u); sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); break;
    case Vdiv: vu::div(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vsqrt: vu::sqrt(v, v.vf[i.ft()].w[ftf]); break;
    case Vrsqrt: vu::rsqrt(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vmtir: vu::set_vi(v, i.ft(), v.vf[i.fs()].w[fsf]); break;
    case Vmfir: vu::mfir(v, i.dest(), i.ft(), i.fs()); break;
    case Vilwr: {
        std::uint16_t x;
        __builtin_memcpy(&x, vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * static_cast<std::uint32_t>(std::countl_zero(i.dest() << 28)), 2);
        vu::set_vi(v, i.ft(), x);
        break;
    }
    case Viswr:
        for (int k = 0; k < 4; ++k)
            if (vu::lane(i.dest(), k)) {
                const std::uint32_t x = v.vi[i.ft()];
                __builtin_memcpy(vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * k, &x, 4);
            }
        break;
    case Vrnext:
        v.r = vu::rand_next(v.r);
        vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; });
        break;
    case Vrget: vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; }); break;
    case Vrinit: v.r = v.vf[i.fs()].w[fsf] & 0x7FFFFFu; break;
    case Vrxor: v.r = (v.r ^ v.vf[i.fs()].w[fsf]) & 0x7FFFFFu; break;

    default:
        stop(c, i.pc, recomp::name(i.op));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- decoding

// decode() and flow() are two calls into another file; the same words come again and again, so the answer is kept by the word.
struct Decoded {
    std::uint32_t word = 0;
    recomp::Op op = recomp::Op::Sll;   // what 0 (a nop) decodes to: an empty slot is right for every word that hashes elsewhere
    Flow flow = Flow::None;
};
Decoded g_decoded[4096];

inline const Decoded &decoded(std::uint32_t word) {
    Decoded &e = g_decoded[(word * 0x9E3779B1u) >> 20];
    if (e.word != word) [[unlikely]] {
        const Insn i = recomp::decode(word, 0);
        e = Decoded{word, i.op, recomp::flow(i.op)};
    }
    return e;
}

inline Insn fetch(std::uint32_t pc) {
    const std::uint32_t word = ld32(pc);
    return Insn{pc, word, decoded(word).op};
}

// ---------------------------------------------------------------------------------------------------------------- labels

// Where translated code charges the plain instructions it has added up (the top of this file): at the blocks' starts. A label of the
// translation is a target of a branch or jump (inside its unit, or an entry of the unit when it is in another one), a function entry and
// the start of a 16 KiB unit. The interpreter finds the targets by looking through the 16 KiB around the code it runs, and a bit more on
// both sides (a branch reaches 128 KiB; functions do not branch that far), once per frame and 16 KiB of code. It is exact for code the
// scan sees as the translator saw it; a data word that looks like a branch makes one label too many, which costs nothing but the position
// of a charge inside a block.
constexpr std::uint32_t kRegionBytes = 0x4000, kRegionWords = kRegionBytes / 4, kScanMargin = 0x8000;

struct Labels {
    std::uint32_t base = 0;
    std::uint64_t hash = 0;
    std::uint64_t frame = ~0ull;
    std::uint64_t bits[kRegionWords / 64] = {};
    bool is(std::uint32_t pc) const {
        const std::uint32_t o = (pc - base) >> 2;
        return (bits[o >> 6] >> (o & 63u)) & 1u;
    }
};
std::unordered_map<std::uint32_t, Labels> g_labels;   // by 16 KiB region; nodes stay where they are

// The scan reads the memory itself, not through ld32(): a word of hardware must not be read for looking at it.
inline std::uint32_t ram_word(std::uint32_t a) {
    std::uint32_t v;
    std::memcpy(&v, g_ram + a, 4);
    return v;
}

std::uint64_t region_hash(std::uint32_t base) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::uint32_t o = 0; o < kRegionBytes; o += 8) {
        std::uint64_t word;
        std::memcpy(&word, g_ram + base + o, 8);
        h = (h ^ word) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

void scan_labels(Labels &l) {
    for (auto &b : l.bits) b = 0;
    l.bits[0] = 1;   // the start of the unit: translated code charges before it runs on into the next unit
    const std::uint32_t from = l.base >= kScanMargin ? l.base - kScanMargin : 0u, to = std::min(l.base + kRegionBytes + kScanMargin, kRamSize);
    for (std::uint32_t a = from; a < to; a += 4) {
        const std::uint32_t word = ram_word(a);
        const std::uint32_t top = word >> 26;
        // Only jumps and branches can be targets: keep the other words out of decode() (most of the words)
        if (top != 0 && top != 1 && top != 2 && top != 3 && !(top >= 4 && top <= 7) && !(top >= 20 && top <= 23) && top != 16 && top != 17 && top != 18) continue;
        const Insn i = recomp::decode(word, a);
        switch (recomp::flow(i.op)) {
        case Flow::Branch: case Flow::BranchLikely: case Flow::Jump: case Flow::Call: break;
        default: continue;
        }
        const std::uint32_t t = i.branch_target_of();
        if (t - l.base < kRegionBytes) {
            const std::uint32_t o = (t - l.base) >> 2;
            l.bits[o >> 6] |= 1ull << (o & 63u);
        }
    }
}

const Labels *labels_for(std::uint32_t pc) {
    const std::uint32_t base = pc & ~(kRegionBytes - 1u);
    static Labels outside;   // code that is not in main memory (the scratchpad): the start of the region is all there is
    if (base >= kRamSize) {
        outside.base = base;
        outside.bits[0] = 1;
        return &outside;
    }
    Labels &l = g_labels[base >> 14];
    const std::uint64_t frame = frame_count();
    if (l.frame == frame) return &l;
    const std::uint64_t hash = region_hash(base);
    if (l.frame == ~0ull || l.hash != hash || l.base != base) {
        l.base = base;
        l.hash = hash;
        scan_labels(l);
    }
    l.frame = frame;
    return &l;
}

// ---------------------------------------------------------------------------------------------------------------- accounting

std::uint64_t g_charged = 0;   // cycles the interpreter charged: the instructions it ran
std::uint64_t g_entries = 0;   // times dispatch() started it

struct Report {
    ~Report() {
        if (g_entries && !os::env("PS2_INTERP_STATS").empty())
            std::fprintf(stderr, "[interp] %llu instructions interpreted in %llu entries\n", static_cast<unsigned long long>(g_charged), static_cast<unsigned long long>(g_entries));
    }
} g_report;

}  // namespace

std::uint64_t interp_instructions() { return g_charged; }

namespace {

inline void charge(Cpu &c, std::uint32_t n) {
    c.budget -= static_cast<std::int32_t>(n);
    g_charged += n;
}

// ---------------------------------------------------------------------------------------------------------------- control flow

enum class Go { Continue, Return, Dispatch };

// A branch or jump with its delay slot has run, and the code goes on at `pc`. A backward one is where translated code may be preempted.
inline Go jump(Cpu &c, std::uint32_t target, std::uint32_t from, std::uint32_t &pc) {
    if (target <= from && c.budget < 0 && preempt(c)) {
        c.pc = target | kUnwind;
        return Go::Return;
    }
    pc = target;
    return Go::Continue;
}

// What the flow of control does once the instruction in hand has run (codegen.cpp, Next).
struct Next {
    enum Kind { Jump, Call, Return, Indirect, IndirectCall };
    Kind kind;
    std::uint32_t pc;    // Jump, Call: the target
    std::uint32_t ret;   // Call, IndirectCall: where the callee returns to
    std::uint32_t var;   // Return, Indirect, IndirectCall: the target
    bool known() const { return kind == Jump || kind == Call; }
};

constexpr unsigned kMaxNesting = 8;   // codegen.cpp: the same limit on branches in delay slots

// A call to a known target: translated code calls inside its own image without setting c.pc (interp.hpp, call_quiet)
inline void call_direct(Cpu &c, std::uint32_t target, std::uint32_t from, std::uint32_t depth) {
    if (same_image(from, target)) call_quiet(c, target, depth);
    else call(c, target, depth);
}

Go transfer(Cpu &c, const Next &n, std::uint32_t from, std::uint32_t depth, std::uint32_t &pc) {
    switch (n.kind) {
    case Next::Jump:
        return jump(c, n.pc, from, pc);
    case Next::Call:
        call_direct(c, n.pc, from, depth + 1u);
        if (c.pc != n.ret) return Go::Return;
        return jump(c, n.ret, from, pc);
    case Next::Return:
        c.pc = n.var;
        return Go::Return;
    case Next::Indirect:
        c.pc = n.var;
        return Go::Dispatch;
    case Next::IndirectCall:
        c.pc = n.var;
        call(c, n.var, depth + 1u);
        if (c.pc != n.ret) return Go::Return;
        return jump(c, n.ret, from, pc);
    }
    return Go::Return;
}

// Runs the instruction at p as the delay slot of the pending transfer `next`, then makes the transfer. A branch there does what
// ps2autotests recorded on a console (codegen.cpp, slot_flow): the first branch has already sent the fetch to its target, so the
// instruction at that target becomes the delay slot of the second branch, which then goes to its own target. A branch-likely that is not
// taken drops that instruction instead. BC1T and BC1F are the exception in the recording: the result is that of the last branch of the
// chain, as if each one replaced the earlier target and took the instruction after it in memory as its delay slot. A syscall in a delay
// slot resumes at the destination of the branch.
Go slot_flow(Cpu &c, std::uint32_t p, const Next &next, unsigned nesting, std::uint32_t depth, std::uint32_t &pc) {
    const Insn x = fetch(p);
    if (x.op == Invalid || nesting > kMaxNesting) {
        stop(c, p, "unsupported delay slot");
        return Go::Return;
    }
    if (!recomp::has_delay_slot(x.op)) {
        if (x.op != Syscall) {
            if (!simple(c, x)) return Go::Return;
        } else {
            const std::uint32_t where = next.known() ? next.pc : next.var;
            c.pc = where;
            syscall(c, p);
            if (c.pc != where) return Go::Return;
        }
        return transfer(c, next, p, depth, pc);
    }

    const std::uint32_t ret = p + 8, target = x.branch_target_of();
    const bool likely = recomp::flow(x.op) == Flow::BranchLikely || x.op == Bltzall || x.op == Bgezall;
    bool has_condition = true, go = true;
    Next to{Next::Jump, target, 0, 0};
    switch (x.op) {
    case J:
        has_condition = false;
        break;
    case Jal:
        has_condition = false;
        link(c, 31, ret);
        to = Next{Next::Call, target, ret, 0};
        break;
    case Jr: {
        has_condition = false;
        const std::uint32_t dst = w(c, x.rs());
        to = Next{x.rs() == 31 ? Next::Return : Next::Indirect, 0, 0, dst};
        break;
    }
    case Jalr: {
        has_condition = false;
        const std::uint32_t dst = w(c, x.rs());
        link(c, x.rd(), ret);
        to = Next{Next::IndirectCall, 0, ret, dst};
        break;
    }
    case Bltzal: case Bgezal: case Bltzall: case Bgezall:
        go = condition(c, x);
        link(c, 31, ret);
        to = Next{Next::Call, target, ret, 0};
        break;
    default:
        go = condition(c, x);
        break;
    }
    if (!has_condition || go) {
        if (x.op == Bc1f || x.op == Bc1t) return slot_flow(c, p + 4, to, nesting + 1, depth, pc);
        if (next.known()) return slot_flow(c, next.pc, to, nesting + 1, depth, pc);
        stop(c, p, "unsupported delay slot");
        return Go::Return;
    }
    if (!likely) return transfer(c, next, p, depth, pc);
    if (next.known()) {
        Next skipped = next;
        skipped.pc += 4;
        return transfer(c, skipped, p, depth, pc);
    }
    stop(c, p, "unsupported delay slot");
    return Go::Return;
}

// The delay slot of a branch whose slot is not a branch or a syscall: one plain instruction.
inline bool slot(Cpu &c, const Insn &ds) {
    if (ds.op == Invalid) {
        stop(c, ds.pc, "unsupported delay slot");
        return false;
    }
    return simple(c, ds);
}

bool g_verbose = false, g_verbose_known = false;
std::uint32_t g_reported = 0;

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- the interpreter

void interpret(Cpu &c, std::uint32_t, std::uint32_t depth) { [[clang::musttail]] return interpret_from(c, c.pc, depth); }

void interpret_from(Cpu &c, std::uint32_t pc, std::uint32_t depth) {
    ++g_entries;
    if (!g_verbose_known) {
        g_verbose_known = true;
        g_verbose = !os::env("PS2_INTERP_VERBOSE").empty();
    }
    if (g_verbose && g_reported < 200) {
        ++g_reported;
        std::fprintf(stderr, "[interp] interpreting at 0x%08X (ra 0x%08X, frame %llu)\n", pc, c.r[31].w[0], static_cast<unsigned long long>(frame_count()));
    }
    const bool forced_any = interp_forced_any();
    const Labels *labels = labels_for(pc);
    std::uint32_t seen_epoch = modules_epoch();
    std::uint32_t pending = 0;   // plain instructions run since the last charge
    bool check = false;          // at the start of a block that a branch reached: look for translated code
    std::uint32_t from = pc;     // the branch that reached it

    for (;;) {
        // back to translated code at the first branch target that has some. Only when something could have changed since the last look
        // (a module was added): a look into the modules costs about as much as a hundred instructions.
        if (check) {
            bool look = false;
            if (forced_any) look = true;
            else if (const std::uint32_t epoch = modules_epoch(); epoch != seen_epoch) {
                seen_epoch = epoch;
                look = true;
            }
            if (look && code_translated(pc)) {
                if (same_image(from, pc)) [[clang::musttail]] return call_quiet(c, pc, depth);
                c.pc = pc;
                [[clang::musttail]] return dispatch(c, 0u, depth);
            }
            check = false;
        }
        for (;;) {
            if (pc - labels->base >= kRegionBytes) [[unlikely]] labels = labels_for(pc);
            if (pending && labels->is(pc)) {
                charge(c, pending);
                pending = 0;
            }
            const Insn i = fetch(pc);
            const Flow fl = decoded(i.word).flow;
            if (fl == Flow::None) [[likely]] {
                if (i.op == Syscall) {
                    charge(c, pending);
                    pending = 0;
                    const std::uint32_t next = pc + 4u;
                    c.pc = next;
                    syscall(c, pc);
                    if (c.pc != next) return;
                } else {
                    if (i.op == Mfc0 || i.op == Mtc0) {
                        if (i.rd() == 25) {
                            charge(c, pending);
                            pending = 0;
                        }
                    }
                    if (!simple(c, i)) return;
                }
                ++pending;
                pc += 4u;
                continue;
            }
            if (i.op == Eret) {
                charge(c, pending);
                c.pc = eret(c);
                [[clang::musttail]] return dispatch(c, 0u, depth);
            }
            if (fl == Flow::Stop) {
                charge(c, pending);
                stop(c, pc, recomp::name(i.op));
                return;
            }
            // A branch or jump: it and its delay slot cost 2, with the plain instructions before them
            charge(c, pending + 2u);
            pending = 0;
            from = pc;
            const Insn ds = fetch(pc + 4u);
            if (ds.op != Invalid && (recomp::has_delay_slot(ds.op) || ds.op == Syscall)) {
                switch (slot_flow(c, pc, Next{Next::Jump, pc + 4u, 0, 0}, 0, depth, pc)) {
                case Go::Continue: check = true; break;
                case Go::Return: return;
                case Go::Dispatch: [[clang::musttail]] return dispatch(c, 0u, depth);
                }
                break;
            }
            const std::uint32_t ret = pc + 8u, t = i.branch_target_of();
            switch (i.op) {
            case Jal:
                link(c, 31, ret);
                if (!slot(c, ds)) return;
                call_direct(c, t, i.pc, depth + 1u);
                if (c.pc != ret) return;
                pc = ret;
                continue;
            case Bltzal: case Bgezal: {
                const bool go = condition(c, i);
                link(c, 31, ret);
                if (!slot(c, ds)) return;
                if (go) {
                    call_direct(c, t, i.pc, depth + 1u);
                    if (c.pc != ret) return;
                }
                pc = ret;
                continue;
            }
            case Bltzall: case Bgezall: {
                const bool go = condition(c, i);
                link(c, 31, ret);
                if (go) {
                    if (!slot(c, ds)) return;
                    call_direct(c, t, i.pc, depth + 1u);
                    if (c.pc != ret) return;
                }
                pc = ret;
                continue;
            }
            case Jalr: {
                const std::uint32_t target = w(c, i.rs());
                link(c, i.rd(), ret);
                if (!slot(c, ds)) return;
                c.pc = target;
                call(c, target, depth + 1u);
                if (c.pc != ret) return;
                pc = ret;
                continue;
            }
            case Jr: {
                const std::uint32_t target = w(c, i.rs());
                if (!slot(c, ds)) return;
                c.pc = target;
                if (i.rs() == 31) return;
                // A jump through a register: a jump table, or a call that never comes back. Translated code where it lands, the
                // dispatcher decides (it also starts the translation of what is missing); otherwise the interpreter goes on here.
                if (!forced_any && !code_translated(target)) {
                    pc = target;
                    check = false;
                    break;
                }
                [[clang::musttail]] return dispatch(c, 0u, depth);
            }
            case J:
                if (!slot(c, ds)) return;
                if (jump(c, t, i.pc, pc) == Go::Return) return;
                check = true;
                break;
            default:
                if (recomp::flow(i.op) == Flow::BranchLikely) {
                    if (condition(c, i)) {
                        if (!slot(c, ds)) return;
                        if (jump(c, t, i.pc, pc) == Go::Return) return;
                        check = true;
                    } else {
                        pc = ret;
                        continue;
                    }
                } else {
                    const bool go = condition(c, i);
                    if (!slot(c, ds)) return;
                    if (go) {
                        if (jump(c, t, i.pc, pc) == Go::Return) return;
                        check = true;
                    } else {
                        pc = ret;
                        continue;
                    }
                }
                break;
            }
            break;
        }
    }
}

}  // namespace ps2
