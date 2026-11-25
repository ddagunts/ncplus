/* SPDX-License-Identifier: MIT */
/*
 * The native window our WSEGL takes (EGLNativeWindowType = struct netcast_window *).
 * The caller owns the buffers (mmapped, GPU-mappable memory, e.g. KMS dumb buffers) and shows
 * them: the WSEGL calls swap(priv, n) when the GPU has finished buffer n, from
 * eglSwapBuffers(). Buffers must stay mapped until the EGL surface is destroyed.
 */
#ifndef NETCAST_WINDOW_H
#define NETCAST_WINDOW_H

#define NETCAST_WINDOW_MAGIC 0x6e637731u	/* "ncw1" */
#define NETCAST_WINDOW_MAX_BUFFERS 4

struct netcast_window {
	unsigned int magic;
	unsigned int width, height;
	unsigned int stride;			/* pixels, multiple of 32 */
	unsigned int nbuffers;			/* 2..4 */
	unsigned int size;			/* bytes per buffer */
	void *map[NETCAST_WINDOW_MAX_BUFFERS];	/* page aligned, read/write */
	void (*swap)(void *priv, unsigned int buffer);
	void *priv;
	unsigned int no_wait;			/* test only: don't wait for the GPU before swap() */
};

#endif
