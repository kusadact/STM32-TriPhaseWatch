#ifndef BUSCOMM_GCC_COMPAT_H
#define BUSCOMM_GCC_COMPAT_H

/* ARMCC spellings that still appear in the vendor BSP headers. */
#ifndef __align
#define __align(n) __attribute__((aligned(n)))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif

#endif /* BUSCOMM_GCC_COMPAT_H */
