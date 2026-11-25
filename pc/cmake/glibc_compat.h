/* Linux packages: link against old glibc symbol versions so the binary runs
 * on glibc >= 2.34 (Ubuntu 22.04, Debian 12, ...) even when built on a newer
 * system. Force-included (-include) in SDL2 and the game when
 * GB_PORTABLE_GLIBC is on; compile as C17 (C23 renames strtol & co.). */
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver expf,expf@GLIBC_2.2.5");
__asm__(".symver logf,logf@GLIBC_2.2.5");
__asm__(".symver powf,powf@GLIBC_2.2.5");
__asm__(".symver exp,exp@GLIBC_2.2.5");
__asm__(".symver log,log@GLIBC_2.2.5");
__asm__(".symver pow,pow@GLIBC_2.2.5");
__asm__(".symver log2f,log2f@GLIBC_2.2.5");
__asm__(".symver exp2f,exp2f@GLIBC_2.2.5");
/* with _GNU_SOURCE, glibc >= 2.38 headers redirect these to their C23
 * variants (__isoc23_*, which also accept 0b prefixes): use the old ones */
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoll,strtoll@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoull,strtoull@GLIBC_2.2.5");
__asm__(".symver __isoc23_sscanf,sscanf@GLIBC_2.2.5");
__asm__(".symver __isoc23_vsscanf,vsscanf@GLIBC_2.2.5");
#endif
