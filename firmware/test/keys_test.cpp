// Host test for the keyboard logic. Run: clang++ -std=c++20 -I main test/keys_test.cpp -o /tmp/kt && /tmp/kt
#include "keys.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
static char decode_char(uint8_t ev, bool shift=false){ KeyEvent k; CHECK(tca8418_decode(ev,k)); return key_char(k.row,k.col,shift); }
int main(){
  { KeyEvent k; CHECK(!tca8418_decode(0,k)); CHECK(!tca8418_decode(0x80,k)); CHECK(!tca8418_decode(0x7F,k)); }
  // The espp mapping: key number 1 is Tr0 Tc0 -> row 0 col 0 ('`'); number 5 is Tr0 Tc4 -> row 0 col 1 ('1').
  CHECK(decode_char(0x81)=='`'); CHECK(decode_char(0x85)=='1');
  { KeyEvent k; tca8418_decode(0x81,k); CHECK(k.pressed); tca8418_decode(0x01,k); CHECK(!k.pressed); }
  // Every key of the 4x14 grid is reachable and unique.
  { bool seen[4][14]={}; int n=0; for(int e=1;e<=80;e++){ KeyEvent k; if(tca8418_decode(e,k)){ CHECK(!seen[k.row][k.col]); seen[k.row][k.col]=true; n++; } } CHECK(n==56); }
  // Tab, '=', '-', 'f' are where the keyboard has them (row 1 col 0, row 0 col 12 and 11, row 2 col 5).
  CHECK(key_char(1,0,false)=='\t'); CHECK(key_char(0,12,false)=='='); CHECK(key_char(0,12,true)=='+');
  CHECK(key_char(0,11,false)=='-'); CHECK(key_char(2,5,false)=='f'); CHECK(key_char(2,1,false)==0);
  CHECK(key_action('\t')==KeyAction::ToggleDebug);
  CHECK(key_action('=')==KeyAction::VolumeUp && key_action('+')==KeyAction::VolumeUp);
  CHECK(key_action('-')==KeyAction::VolumeDown && key_action('_')==KeyAction::VolumeDown);
  CHECK(key_action('f')==KeyAction::ToggleFiles && key_action('F')==KeyAction::ToggleFiles);
  CHECK(key_action('a')==KeyAction::None && key_action(0)==KeyAction::None);
  { KeyRepeater r; CHECK(r.press(KeyAction::VolumeUp,1000)==KeyAction::VolumeUp);
    CHECK(r.tick(1399)==KeyAction::None); CHECK(r.tick(1400)==KeyAction::VolumeUp);
    CHECK(r.tick(1519)==KeyAction::None); CHECK(r.tick(1520)==KeyAction::VolumeUp);
    r.release(KeyAction::VolumeDown); CHECK(r.tick(1700)==KeyAction::VolumeUp);
    r.release(KeyAction::VolumeUp); CHECK(r.tick(5000)==KeyAction::None); }
  { KeyRepeater r; CHECK(r.press(KeyAction::ToggleDebug,0)==KeyAction::ToggleDebug); CHECK(r.tick(9999)==KeyAction::None); // no repeat for Tab
    uint32_t t=0xFFFFFF00u; r.press(KeyAction::VolumeDown,t); CHECK(r.tick(t+400)==KeyAction::VolumeDown); } // wrap-around
  puts("keys tests ok"); }
