// Host test. Run: clang++ -std=c++20 -I main test/power_policy_test.cpp -o /tmp/pt && /tmp/pt
#include "power_policy.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int main(){
  CHECK(minutes_left(0)==0); CHECK(minutes_left(kBytesPerMinute-1)==0); CHECK(minutes_left(kBytesPerMinute)==1);
  CHECK(minutes_left(30ULL*1000*1000*1000)==2604); // a 32 GB card is about 43 hours
  CHECK(space_level(-1)==SpaceLevel::Ok); CHECK(space_level(15)==SpaceLevel::Ok); CHECK(space_level(14)==SpaceLevel::Low);
  CHECK(space_level(5)==SpaceLevel::Low); CHECK(space_level(4)==SpaceLevel::Stop); CHECK(space_level(0)==SpaceLevel::Stop);
  CHECK(battery_percent(4.2f)==100 && battery_percent(4.5f)==100); CHECK(battery_percent(3.3f)==0 && battery_percent(3.0f)==0);
  CHECK(battery_percent(3.7f)==32); { int p=battery_percent(3.75f); CHECK(p>=40 && p<=41); }
  CHECK(battery_percent(0.0f)==-1 && battery_percent(2.4f)==-1 && battery_percent(5.0f)==-1);
  for(float v=3.3f; v<4.2f; v+=0.01f) CHECK(battery_percent(v)<=battery_percent(v+0.01f));
  { BatteryMonitor m; CHECK(m.update(80)==BatteryLevel::Ok); CHECK(m.update(15)==BatteryLevel::Low); CHECK(m.update(16)==BatteryLevel::Ok);
    CHECK(m.update(5)==BatteryLevel::Low); CHECK(m.update(5)==BatteryLevel::Low); CHECK(m.update(5)==BatteryLevel::Stop);
    CHECK(m.update(1)==BatteryLevel::Stop); }
  { BatteryMonitor m; m.update(4); m.update(4); CHECK(m.update(40)==BatteryLevel::Ok); CHECK(m.update(4)==BatteryLevel::Low); } // a spike resets the count
  { BatteryMonitor m; m.update(4); m.update(4); CHECK(m.update(-1)==BatteryLevel::Ok); CHECK(m.update(4)==BatteryLevel::Low); }
  puts("power policy tests ok"); }
