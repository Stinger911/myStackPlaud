// Host test for the volume mapping. Run: clang++ -std=c++20 -I main test/volume_test.cpp -o /tmp/vt && /tmp/vt
#include "volume.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int main(){
  const int cap=0x97;
  CHECK(volume_dac_reg(cap,10)==cap);                 // level 10 is the current ceiling
  CHECK(volume_dac_reg(cap,9)==cap-6);                // 3 dB per step (0.5 dB per register step)
  for(int l=1;l<=10;l++) CHECK(volume_dac_reg(cap,l)>volume_dac_reg(cap,l-1<1?0:l-1) || l==1);
  CHECK(volume_dac_reg(cap,0)==cap-60);
  CHECK(volume_dac_reg(0x10,1)==0);                   // never below register 0
  CHECK(volume_clamp(-3)==0 && volume_clamp(14)==10);
  for(int l=0;l<=10;l++) CHECK(volume_dac_reg(cap,l)<=cap); // never above the cap
  puts("volume tests ok"); }
