#!/usr/bin/env python3
"""Synthetic VU0 macro operand decoding regression; no game or emulator code.

Generate native EE code, reject bounds diagnostics, then execute it against an
independent operand/result oracle using a minimal host ABI. No runtime/GPU needed.
"""
import argparse,json,pathlib,struct,subprocess,platform
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--recompiler',type=pathlib.Path,required=True);p.add_argument('--compiler',type=pathlib.Path,required=True);p.add_argument('--work',type=pathlib.Path,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1];d=a.work.resolve();d.mkdir(parents=True,exist_ok=False)
cases=[]
def case(kind,ft,fs,fd=0,mask=0,imm=0):
    fun=(0x30,0x31,0x34,0x35,0x32)[kind] if kind<5 else None
    if kind<5:word=0x4a000000|(ft<<16)|(fs<<11)|(((imm&31) if kind==4 else fd)<<6)|fun
    elif kind<13:
        # LQI/SQI/LQD/SQD, MTIR/MFIR/ILWR/ISWR.
        col=(kind-5)%4;index=13 if kind<9 else 15
        word=0x4a000000|(mask<<21)|(ft<<16)|(fs<<11)|(index<<6)|(0x3c+col)
    else:word=(0x12<<26)|((2 if kind==13 else 6)<<21)|(3<<16)|(fs<<11)
    cases.append((kind,ft,fs,fd,mask,imm,word))
for kind in range(4):
    for dest in (0,3,15):
        for s in (1,15):
            for t in (0,2):
                for high in range(8):case(kind,t|((high&1)<<4),s|((high&2)<<3),dest|((high&4)<<2))
for imm in range(-16,16):
    for dest in (0,3,15):
        for high in range(4):case(4,dest|((high&1)<<4),1|((high&2)<<3),imm=imm)
for kind in range(5,9):
    for high in (0,16):
        for vi in (0,1,15):
            # Integer address is fs for loads, ft for stores. Vector is vf31.
            case(kind,31 if kind in (5,7) else vi|high,vi|high if kind in (5,7) else 31,mask=15)
for vi in (0,1,15):
    for high in (0,16):
        case(9,vi|high,31,mask=2)   # MTIR, vf31.z to integer.
        case(10,31,vi|high,mask=15) # MFIR, integer to vf31.
for high in range(4):
    case(11,3|((high&1)<<4),1|((high&2)<<3),mask=8)
    case(12,2|((high&1)<<4),1|((high&2)<<3),mask=15)
for reg in range(16,32):case(13,0,reg);case(14,0,reg)
base=0x100000;payload=bytearray(len(cases)*16)
for n,c in enumerate(cases):struct.pack_into('<4I',payload,n*16,c[-1],0x03e00008,0,0x0d)
elf=bytearray(0x100)+payload;elf[:7]=b'\x7fELF\x01\x01\x01'
struct.pack_into('<HHIIIIIHHHHHH',elf,16,2,8,1,base,52,0,0,52,32,1,40,0,0);struct.pack_into('<8I',elf,52,1,0x100,base,0,len(payload),len(payload),5,4)
(d/'input.elf').write_bytes(elf);(d/'entries.txt').write_text(''.join(f'{base+n*16:x}\n' for n in range(len(cases))))
gen=subprocess.run([str(a.recompiler.resolve()),str(d/'input.elf'),'--entries',str(d/'entries.txt'),'--out',str(d/'generated')],capture_output=True,text=True);(d/'generation.log').write_text(gen.stdout+gen.stderr);assert gen.returncode==0
(d/'cases.hpp').write_text('struct Case {unsigned kind,ft,fs,fd,mask; int imm; unsigned word;};\nstatic const Case cases[]={\n'+''.join('{'+','.join(str(v) for v in c)+'},\n' for c in cases)+'};\n')
harness=r'''
#include "ps2/generated.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstddef>
#include "cases.hpp"
namespace ps2 {
alignas(64) std::uint8_t g_ram[kRamSize+64], g_spr[kScratchpadSize];
static std::vector<UnitInfo> units;
static std::array<std::uint8_t,4096> memory;
static unsigned control_reg,control_value;
void register_units(const UnitInfo *u,std::uint32_t n){units.assign(u,u+n);}
void register_modules(const ModuleInfo*,std::uint32_t){}
void bad_entry(Cpu&,std::uint32_t){std::abort();}
bool preempt(Cpu&){return false;}
void vu0_wait(Cpu&){}
std::uint8_t *vu0_data(){return memory.data();}
std::uint32_t cfc2(Cpu&,std::uint32_t r){control_reg=r;return 0x12340000u|r;}
void ctc2(Cpu&,std::uint32_t r,std::uint32_t v){control_reg=r;control_value=v;}
void dispatch(Cpu& c,std::uint32_t,std::uint32_t depth){
    for(const auto &u:units)if(c.pc>=u.base&&c.pc<u.base+u.size){auto e=u.entries[(c.pc-u.base)/4];if(e){u.function(c,e,depth);return;}}
    std::abort();
}
}
void register_generated_code();
int main(){
 using namespace ps2;register_generated_code();
 for(unsigned n=0;n<std::size(cases);++n){const auto &x=cases[n];Cpu c{};c.pc=0x100000+n*16;c.budget=1000;c.r[31].w[0]=0x200000;c.r[3].w[0]=0x76543210;
 for(unsigned f=0;f<32;++f)for(unsigned k=0;k<4;++k)c.vu0.vf[f].w[k]=0x10000000u+f*0x10000u+k*0x1111u;
 c.vu0.vf[0]={};c.vu0.vf[0].w[3]=0x3f800000;
 for(unsigned k=0;k<16;++k)c.vu0.vi[k]=static_cast<unsigned short>(0x8000+k*137);
 c.vu0.vi[0]=0;c.vu0.vi[1]=3;c.vu0.vi[2]=7;c.vu0.vi[15]=0xffff;
 c.vu0.q=0x12345678;c.vu0.p=0xabcdef01;c.vu0.i=0x98765432;c.vu0.r=0x10203040;
 for(unsigned k=0;k<memory.size();++k)memory[k]=static_cast<unsigned char>(k*7+3);
 auto expected=c.vu0;auto mem=memory;auto regs=c.r[3];control_reg=999;control_value=0;
 const unsigned s=x.fs%16,t=x.ft%16,d=x.fd%16;
 auto write=[&](unsigned r,unsigned value){if(r)expected.vi[r]=static_cast<unsigned short>(value);};
 const unsigned l=expected.vi[s],r=expected.vi[t];
 switch(x.kind){
 case 0:write(d,l+r);break;case 1:write(d,l-r);break;case 2:write(d,l&r);break;case 3:write(d,l|r);break;case 4:write(t,l+x.imm);break;
 case 5:case 7:{unsigned at=l;if(x.kind==7){write(s,at-1);at=expected.vi[s];}if(x.ft)std::memcpy(&expected.vf[x.ft],mem.data()+(at%256)*16,16);if(x.kind==5)write(s,l+1);break;}
 case 6:case 8:{unsigned at=r;if(x.kind==8){write(t,at-1);at=expected.vi[t];}std::memcpy(mem.data()+(at%256)*16,&expected.vf[x.fs],16);if(x.kind==6)write(t,r+1);break;}
 case 9:write(t,expected.vf[x.fs].w[2]);break;
 case 10:if(x.ft)for(int k=0;k<4;++k)expected.vf[x.ft].w[k]=static_cast<unsigned>(static_cast<int>(static_cast<short>(l)));break;
 case 11:{unsigned short value;std::memcpy(&value,mem.data()+(l%256)*16,2);write(t,value);break;}
 case 12:for(int k=0;k<4;++k){unsigned v=r;std::memcpy(mem.data()+(l%256)*16+k*4,&v,4);}break;
 default:break;
 }
 dispatch(c,0,0);
 bool ok=std::memcmp(&expected,&c.vu0,offsetof(Vu0Regs,tpc)+sizeof expected.tpc)==0&&mem==memory;
 if(x.kind==13)ok=ok&&control_reg==x.fs&&c.r[3].w[0]==(0x12340000u|x.fs);
 if(x.kind==14)ok=ok&&control_reg==x.fs&&control_value==regs.w[0];
 if(!ok){std::fprintf(stderr,"case %u kind %u word %08x failed\n",n,x.kind,x.word);return 1;}
 }
 std::printf("%zu native VU0 operand cases passed\n",std::size(cases));
}
'''
(d/'native.cpp').write_text(harness);exe=d/('fixture.exe')
cmd=[str(a.compiler.resolve()),'-std=c++20','-O2','-ffp-contract=off','-Werror=array-bounds','-I'+str(root/'runtime/include'),'-I'+str(d),str(d/'native.cpp')]+list(map(str,(d/'generated').glob('*.cpp')))+['-o',str(exe)]
if platform.machine().lower() in ('amd64','x86_64'):cmd.insert(1,'-mavx2')
if a.compiler.suffix.lower()=='.exe':cmd[1:1]=['-static-libgcc','-static-libstdc++']
build=subprocess.run(cmd,capture_output=True,text=True);(d/'build.log').write_text(build.stdout+build.stderr)
if build.returncode:raise SystemExit('Native fixture compilation failed; see '+str(d/'build.log'))
run=subprocess.run([str(exe)],capture_output=True,text=True);(d/'result.json').write_text(json.dumps(dict(cases=len(cases),exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr),indent=2));print(run.stdout+run.stderr);assert run.returncode==0
