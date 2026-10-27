#ifndef FIROZ_CONFIG_H
#define FIROZ_CONFIG_H

/* ------------------------------------------------------------------
   Build-time feature switches for the GPU bring-up work.  Everything the
   bring-up added is compiled in, but the experiments are OFF by default so
   a normal build pays no runtime cost and takes no risk.

   Turn the whole set on with:   -DFIROZ_GPU_EXPERIMENTS=1
   or switch single items below / on the compiler command line.
   ------------------------------------------------------------------ */

#ifndef FIROZ_GPU_EXPERIMENTS
#define FIROZ_GPU_EXPERIMENTS 0
#endif

/* Boot-time GPU proofs after the ring self-test: CP draws a tile, DMA fill,
   DMA blit (system RAM through the GART, nothing touches the screen). */
#ifndef FIROZ_GPU_BOOT_TESTS
#define FIROZ_GPU_BOOT_TESTS 1
#endif

/* Verified test that the CP can write the scanout framebuffer; unlocks the
   batched 2D engine (amd_gpu2d_*, gpu_hw2d_*).  Without it the 2D engine
   stays locked and every caller falls back to the CPU. */
#ifndef FIROZ_GPU_FB_TEST
#define FIROZ_GPU_FB_TEST FIROZ_GPU_EXPERIMENTS
#endif

/* Present dirty spans with CP DMA from the back buffer (needs the 32 MiB GART
   and FIROZ_GPU_FB_TEST).  Measured SLOWER than the CPU copy on Renoir (an APU
   shares one DRAM), so this stays off unless you want to experiment. */
#ifndef FIROZ_GPU_PRESENT
#define FIROZ_GPU_PRESENT FIROZ_GPU_EXPERIMENTS
#endif

/* Live GPU/frame-time HUD, F10 (re-run benchmarks), F11 (A/B), F12 (hide),
   one-time benchmarks, COM1 state log. */
#ifndef FIROZ_GPU_HUD
#define FIROZ_GPU_HUD FIROZ_GPU_EXPERIMENTS
#endif

/* rdtsc frame-time profiling inside gfx_present() and the drawing primitives
   (about 3-5% overhead).  Needed for the HUD's frame-time rows. */
#ifndef FIROZ_FRAME_PROFILE
#define FIROZ_FRAME_PROFILE FIROZ_GPU_HUD
#endif

#endif
