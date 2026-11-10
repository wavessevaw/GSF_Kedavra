#include "kfg_hud_input.hpp"
#include <cassert>
int main() {
 using namespace kfg::hud_input;
 geometry(10,10,200,80,1);
 assert(!touch(0,300,200,0));
 assert(touch(0,20,20,1));
 pressed-=800000001ull;
 geometry(10,10,200,80,1);
 assert(sequence==1 && action==1);
 assert(touch(2,0,0,1));
 geometry(10,10,200,80,3);
 assert(touch(0,30,80,2)); assert(touch(2,0,0,2));
 assert(sequence==2 && action==3);
 assert(touch(0,180,80,3)); assert(touch(2,0,0,3));
 assert(sequence==3 && action==4);
 geometry(10,10,200,80,1);
 assert(touch(0,20,20,4)); assert(touch(1,80,20,4));
 pressed-=800000001ull;
 geometry(10,10,200,80,1);
 assert(touch(2,0,0,4)); assert(sequence==3);
}
