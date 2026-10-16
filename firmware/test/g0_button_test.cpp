// Host test for the G0 button logic. Run: clang++ -std=c++20 -I main test/g0_button_test.cpp -o /tmp/g0t && /tmp/g0t
#include "g0_button.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int main(){
  { // start on press when idle, nothing more while held
    G0Button b; auto o=b.update(true,G0Context::Idle,0); CHECK(o.event==G0Event::Start);
    o=b.update(true,G0Context::Recording,100); CHECK(o.event==G0Event::None && !o.holding); // recorder started, button still down
    o=b.update(true,G0Context::Recording,2000); CHECK(o.event==G0Event::None && !o.holding); // no stop from the same press
    b.update(false,G0Context::Recording,2100); }
  { // hold stops after 1 s
    G0Button b; b.update(true,G0Context::Recording,0); auto o=b.update(true,G0Context::Recording,500);
    CHECK(o.holding && o.event==G0Event::None && o.hold_progress>0.49f && o.hold_progress<0.51f);
    o=b.update(true,G0Context::Recording,999); CHECK(o.event==G0Event::None);
    o=b.update(true,G0Context::Recording,1000); CHECK(o.event==G0Event::Stop && !o.holding);
    o=b.update(true,G0Context::Busy,1033); CHECK(o.event==G0Event::None);
    o=b.update(true,G0Context::Recording,1500); CHECK(o.event==G0Event::None && !o.holding); // no second stop
    o=b.update(false,G0Context::Busy,1600); CHECK(o.event==G0Event::None && !o.hint); }
  { // early release cancels and shows the hint for 1.5 s
    G0Button b; b.update(true,G0Context::Recording,0); b.update(true,G0Context::Recording,300);
    auto o=b.update(false,G0Context::Recording,400); CHECK(o.event==G0Event::None && o.hint);
    o=b.update(false,G0Context::Recording,1800); CHECK(o.hint);
    o=b.update(false,G0Context::Recording,1900); CHECK(!o.hint);
    // and recording is not stopped by a later short tap
    o=b.update(true,G0Context::Recording,5000); o=b.update(false,G0Context::Recording,5050); CHECK(o.event==G0Event::None && o.hint); }
  { // short tap while recording: hint, no stop
    G0Button b; b.update(true,G0Context::Recording,100); auto o=b.update(false,G0Context::Recording,140); CHECK(o.event==G0Event::None && o.hint); }
  { // press during busy is ignored
    G0Button b; auto o=b.update(true,G0Context::Busy,0); CHECK(o.event==G0Event::None);
    o=b.update(true,G0Context::Idle,50); CHECK(o.event==G0Event::None); b.update(false,G0Context::Idle,60);
    o=b.update(true,G0Context::Idle,100); CHECK(o.event==G0Event::Start); }
  { // recorder stops by itself (error) while held: no stop event, no stuck ring
    G0Button b; b.update(true,G0Context::Recording,0); auto o=b.update(true,G0Context::Idle,400); CHECK(!o.holding && o.event==G0Event::None); }
  { // timer wrap-around
    G0Button b; uint32_t t=0xFFFFFF00u; b.update(true,G0Context::Recording,t); auto o=b.update(true,G0Context::Recording,t+1000); CHECK(o.event==G0Event::Stop); }
  puts("g0 tests ok"); }
