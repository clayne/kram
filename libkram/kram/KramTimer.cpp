// kram - Copyright 2020-2022 by Alec Miller. - MIT License
// The license and copyright notice shall be included
// in all copies or substantial portions of the Software.

#include "KramTimer.h"

#if 1

#if KRAM_WIN
#include <windows.h>
#elif KRAM_MAC || KRAM_IOS
#include <mach/mach_time.h>
#endif

namespace kram {

using namespace NAMESPACE_STL;

#if KRAM_WIN

static double queryPeriod()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    
    // convert from nanos to seconds
    return 1.0 / double(frequency.QuadPart);
};

static uint64_t queryCounter()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
};

#elif KRAM_IOS || KRAM_MAC

static double queryPeriod()
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    
    // https://eclecticlight.co/2020/11/27/inside-m1-macs-time-and-logs/
    // On macOS Intel, nanosecondsPerTick are 1ns (1/1)
    // On macOS M1, nanosecondsPerTick are 41.67ns (num/denom = 125/3)
    double period = (double)timebase.numer / timebase.denom;
    period *= 1e-9; // convert to seconds
    
    return period;
}

static uint64_t queryCounter()
{
    // increment when app sleeps
    // return mach_continuous_time();
    
    // no increment when app sleeps
    return mach_absolute_time();
}

#endif

static const double gQueryPeriod = queryPeriod();
static const uint64_t gStartTime = queryCounter();

double currentTimestamp()
{
    uint64_t delta = queryCounter() - gStartTime;
    return (double)delta * gQueryPeriod;
}

} // namespace kram

#else

/*
// see sources here
// https://codebrowser.dev/llvm/libcxx/src/chrono.cpp.html
// but steady on macOS uses clock_gettime(CLOCK_MONOTONIC_RAW, &tp)
//   which should be mach_continuous_time()
//
// also see sources here for timers
// https://opensource.apple.com/source/Libc/Libc-1158.1.2/gen/clock_gettime.c.auto.html
// mach_continuous_time() vs. mach_absolute_time()
// https://developer.apple.com/library/archive/qa/qa1398/_index.html
 
#if USE_EASTL
#include "EASTL/chrono.h"
#else
#include <chrono>
#endif
 
namespace kram {

using namespace NAMESPACE_STL;

#if USE_EASTL
using namespace eastl::chrono;
#else
using namespace std::chrono;
#endif

// high-res  (defaults to steady or system in libcxx)
//using myclock = high_resolution_clock;
//using myclock = system_clock;
using myclock = steady_clock;

static const myclock::time_point gStartTime = myclock::now();

double currentTimestamp()
{
    auto t = myclock::now();
    duration<double, std::milli> timeSpan = t - gStartTime;
    double count = (double)timeSpan.count() * 1e-3;
    return count;
}

}  // namespace kram
*/

#endif

