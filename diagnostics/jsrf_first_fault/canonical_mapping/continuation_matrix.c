/* Included by validate_continuation.py; callbacks model list removal only. */
#include <stdio.h>
uint32_t expected[16384], actual_log[64], expected_log[64];
unsigned actual_n, expected_n, first_wrap, second_wrap, first_calls, second_calls;
#define R(a) expected[(uint32_t)(a)/4]
static void remove_entry(uint32_t *m, unsigned list, unsigned index) {
 unsigned off=list?0x90:0x70, countoff=list?0xb4:0xb0;
 unsigned n=m[(0x1000+countoff)/4];
 if(index>=n) abort();
 for(unsigned j=index;j+1<n;j++) m[(0x1000+off)/4+j]=m[(0x1000+off)/4+j+1];
 m[(0x1000+countoff)/4]=n-1;
}
static void mock_call(uint32_t address) {
 unsigned list=address==0x14610, i=MEM32(esp+4);
 if((address!=0x147a0 && address!=0x14610) || ecx!=0x1000 || actual_n>=64) abort();
 if(MEM32(esp)!=(list?0x14941:0x148f1)) abort();
 actual_log[actual_n++]=(list<<16)|i;
 if(list) second_calls++; else first_calls++;
 remove_entry(mem,list,i);
 /* stdcall/thiscall stack cleanup and deliberately clobbered volatile regs. */
 esp+=8; eax=0xdead0001; ecx=0xdead0002; edx=0xdead0003;
}
#undef RECOMP_ABI_CALL
#define RECOMP_ABI_CALL(a,f) mock_call(a)
/* GENERATED_BODY */
static void reference(void) {
 unsigned capacity=R(0x1054);
 for(unsigned i=0;i<R(0x10b0);i++) {
  unsigned p=R(0x1070+4*i), old=R(p+8), next=(old+1)%capacity;
  if(next==0) first_wrap++;
  R(p+8)=next;
  if(next==R(p+4)) R(p+4)=(R(p+4)+1)%capacity;
  unsigned data=R(p+16);
  for(unsigned j=0;j<3;j++) R(data+12*next+4*j)=R(data+12*old+4*j);
  if(R(p+12)!=next && R(p+12)!=old) {
   expected_log[expected_n++]=i; remove_entry(expected,0,i);
  }
 }
 for(unsigned i=0;i<R(0x10b4);) {
  unsigned p=R(0x1090+4*i), next=(R(p+4)+1)%capacity;
  if(next==0) second_wrap++;
  R(p+4)=next;
  if(next==R(p+8)) {
   expected_log[expected_n++]=0x10000|i; remove_entry(expected,1,i);
  } else i++;
 }
}
int main(void) {
 unsigned cases=0;
 for(unsigned cap=1;cap<=5;cap++)
 for(unsigned n0=0;n0<=8;n0++) for(unsigned n1=0;n1<=8;n1++)
 for(unsigned seed=0;seed<32;seed++) {
  memset(mem,0,sizeof(mem)); actual_n=expected_n=0;
  ecx=0x1000;esp=0xf000;ebx=0x1234;esi=0x5678;edi=0x9abc;
  MEM32(0x1054)=cap; MEM32(0x10b0)=n0;MEM32(0x10b4)=n1;
  for(unsigned i=0;i<16;i++) {
   unsigned p=0x2000+32*i,data=0x3000+128*i;
   MEM32(0x1070+4*i)=p;
   MEM32(p+4)=(seed+i)%cap; MEM32(p+8)=(seed/3+i*2)%cap;
   MEM32(p+12)=(seed/7+i*3)%cap; MEM32(p+16)=data;
   for(unsigned j=0;j<3*cap;j++) MEM32(data+4*j)=0x100000*i+j+1;
  }
  memcpy(expected,mem,sizeof(mem)); reference(); sub_00014870();
  if(esp!=0xf004 || ebx!=0x1234 || esi!=0x5678 || edi!=0x9abc) return 1;
  if(memcmp(mem,expected,0xe000) || actual_n!=expected_n ||
     memcmp(actual_log,expected_log,actual_n*sizeof(uint32_t))) {
   fprintf(stderr,"Mismatch cap=%u n0=%u n1=%u seed=%u\n",cap,n0,n1,seed); return 2;
  }
  cases++;
 }
 if(!first_wrap || !second_wrap || !first_calls || !second_calls) return 3;
 printf("%u matrix cases PASS; wraps=%u/%u callbacks=%u/%u\n",cases,first_wrap,second_wrap,first_calls,second_calls);
 return 0;
}
