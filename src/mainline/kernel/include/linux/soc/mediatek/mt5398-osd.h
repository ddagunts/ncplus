/* SPDX-License-Identifier: GPL-2.0-only */
/* MT5398 OSD (drivers/gpu/drm/tiny/mt5398_osd.c) calls for the video plane driver. */
#ifndef __SOC_MEDIATEK_MT5398_OSD_H
#define __SOC_MEDIATEK_MT5398_OSD_H

#include <linux/errno.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_DRM_MT5398_OSD)
int mt5398_osd_video_fullscreen(bool on);
#else
static inline int mt5398_osd_video_fullscreen(bool on) { return -ENODEV; }
#endif

#endif
