/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_LG_MICOM_H
#define __LINUX_MFD_LG_MICOM_H

#include <linux/mutex.h>

struct i2c_client;

/* drvdata of the MICOM's I2C device, for its child platform devices */
struct lg_micom {
	struct i2c_client *client;
	struct mutex lock;	/* around a command and its answer */
};

#endif
