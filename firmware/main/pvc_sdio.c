#include "pvc_sdio.h"
#include "bsp/m5stack_core_s3.h"

void pvc_sd_lock(void)   { bsp_display_lock(0); }   /* 0 = 无限等待 */
void pvc_sd_unlock(void) { bsp_display_unlock(); }
