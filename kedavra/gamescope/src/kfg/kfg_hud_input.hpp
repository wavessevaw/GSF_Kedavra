#pragma once
#include <mutex>
#include <cmath>
#include <cstdint>
#include <chrono>

// Shared between the Wayland input thread and compositor. No input thread I/O.
namespace kfg::hud_input {
inline std::mutex mutex;
inline double left=0, top=0, width=0, height=0;
inline uint32_t mode=0, action=0;
inline uint64_t sequence=0, pressed=0, fresh=0;
inline int finger=-1;
inline double start_x=0, start_y=0;
inline bool valid=false, fired=false;
inline uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void geometry(double x, double y, double w, double h, uint32_t m) {
    std::lock_guard lock(mutex);
    left=x; top=y; width=w; height=h; mode=m; fresh=now();
    if (!m) valid=false;
    if (m==1 && valid && !fired && now()-pressed>=800000000ull) {
        action=1; ++sequence; fired=true;
    }
}
inline bool touch(int kind, double x, double y, int id) {
    std::lock_guard lock(mutex);
    const auto t=now();
    if (kind==0) {
        if (finger!=-1 || !mode || t-fresh>1000000000ull || x<left || y<top || x>=left+width || y>=top+height) return false;
        finger=id; start_x=x; start_y=y; pressed=t; valid=true; fired=false;
        return true; // Reserve this contact; no partial gesture reaches the game.
    }
    if (id!=finger) return false;
    if (kind==1 && (std::hypot(x-start_x,y-start_y)>12 || x<left || x>=left+width || y<top || y>=top+height)) valid=false;
    if (kind==2) {
        if (valid && !fired && mode && t-fresh<=1000000000ull) {
            if (mode==1 && t-pressed>=800000000ull) { action=1; ++sequence; }
            else if (mode==2) { action=2; ++sequence; }
            else if (mode==3 && start_y>=top+height*.70) { action=start_x<left+width*.5?3:4; ++sequence; }
        }
        finger=-1; valid=false;
    }
    return true;
}
inline void event(uint64_t &seq, uint32_t &value) { std::lock_guard lock(mutex); seq=sequence; value=action; }
}
