#ifndef __CONFIGURATION_H__
#define __CONFIGURATION_H__

// Adjust the CPU clock frequency here
#define CPU_FREQ_MHZ 133
#define CPU_FREQ_KHZ (CPU_FREQ_MHZ * 1000)

// Adjust the minimum time between each bit bang here
#define CLOCK_PERIOD_NS 320
#define CPU_TICKS_PER_PERIOD (int)(CLOCK_PERIOD_NS * CPU_FREQ_MHZ / 1000.0 + 0.5)

#endif // __CONFIGURATION_H__