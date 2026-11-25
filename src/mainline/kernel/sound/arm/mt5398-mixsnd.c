// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCM playback through the MT5398 audio DSP that the stock LG firmware leaves running
 * across the nckexec warm boot (LG NetCast TVs).
 *
 * The stock MediaTek driver plays PCM (ALSA-style "PlayPCM", GStreamer's mtkaudiosink)
 * by mixing it on the ARM, resampled to 48 kHz stereo S16, into one of nine "MixSnd"
 * channels of the audio DSP: a small ring in the DSP's DRAM window plus a 64-byte control
 * block in DSP shared memory. The DSP plays from the read pointer up to the write
 * pointer and holds there (RP == WP is idle), so nothing on the host side has to keep
 * running: this driver takes the place of the stock feed thread for one channel.
 *
 * Control block (one per channel, 0x40 bytes apart), DSP addresses are 24-bit offsets
 * into the DSP window (phys 0x3e000000 on this board), stored shifted left by 8:
 *   +0x00 write pointer (host writes, 256-byte granularity)
 *   +0x04 ring start   +0x08 ring end
 *   +0x0c gain << 8    (host writes, takes effect at once; 0x20000 = 0 dB, 0 = mute)
 *   +0x10 DSP counter  (ticks while the DSP runs)
 *   +0x20 read pointer (DSP writes)
 *
 * The DSP firmware, its clocks, the internal codec and the speaker amplifier are all
 * left as the stock kernel set them up. Nothing here touches the audio registers. Probe
 * refuses if the control block doesn't describe the ring from the DT, or if the DSP
 * counters are not moving.
 *
 * The stock driver leaves each channel's gain (+0x0c) at the TV's volume level, which can
 * be very low (about -37 dB on the test TV). "Master Playback Volume"/"Switch" set it for our
 * channel: 1 dB steps, at most unity, VOL_START at probe so that a boot is never loud.
 *
 * The ALSA buffer is normal RAM (mmap works, any period size); a soft hrtimer copies
 * whole 256-byte units into the DSP ring and moves the write pointer.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#define CH_STRIDE	0x40
#define CH_WP		0x00
#define CH_START	0x04
#define CH_END		0x08
#define CH_GAIN		0x0c
#define CH_COUNTER	0x10
#define CH_RP		0x20
#define NR_CH		9

#define UNIT		256			/* WP granularity in bytes (64 frames) */
#define FRAME		4			/* S16_LE stereo */
#define TICK_NS		(5 * NSEC_PER_MSEC)

#define VOL_MAX		60			/* 0 dB; 0 = mute, v = v - 60 dB */
#define VOL_START	42			/* -18 dB */

struct mixsnd {
	struct device *dev;
	void __iomem *ctrl;		/* all NR_CH control blocks */
	void __iomem *ch;		/* our channel's block */
	void __iomem *ring;
	phys_addr_t ring_phys;
	u32 ring_size;
	u32 window;			/* phys base of the DSP's 16 MB window */

	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	bool running;
	u32 wp;				/* ring offset of the next write */
	snd_pcm_uframes_t abs;		/* frames handed to the DSP, mod boundary */
	snd_pcm_uframes_t period_acc;

	unsigned int vol;		/* 0..VOL_MAX; under card->controls_rwsem */
	bool on;
};

static u32 dsp_to_off(struct mixsnd *ms, u32 word)
{
	return (ms->window + (word >> 8)) - (u32)ms->ring_phys;
}

static u32 ring_rp(struct mixsnd *ms)
{
	u32 off = dsp_to_off(ms, readl(ms->ch + CH_RP));

	return off < ms->ring_size ? off : ms->wp;	/* garbage: treat as empty */
}

static void set_wp(struct mixsnd *ms, u32 off)
{
	ms->wp = off;
	/* writel's barrier puts the ring data in DRAM before the DSP sees the pointer */
	writel((((u32)ms->ring_phys + off) - ms->window) << 8, ms->ch + CH_WP);
}

/* bytes the DSP still has to play */
static u32 queued(struct mixsnd *ms)
{
	return (ms->wp - ring_rp(ms) + ms->ring_size) % ms->ring_size;
}

/* copy len bytes to ring offset off, wrapping at the ring end; src NULL writes silence */
static void ring_put(struct mixsnd *ms, u32 off, const void *src, u32 len)
{
	while (len) {
		u32 n = min(len, ms->ring_size - off);

		if (src) {
			memcpy_toio(ms->ring + off, src, n);
			src += n;
		} else {
			memset_io(ms->ring + off, 0, n);
		}
		off = (off + n) % ms->ring_size;
		len -= n;
	}
}

/* move what the application has written into the DSP ring; stream lock held */
static void feed(struct mixsnd *ms)
{
	struct snd_pcm_runtime *rt = ms->substream->runtime;
	u32 buf = frames_to_bytes(rt, rt->buffer_size);
	snd_pcm_sframes_t pending;
	u32 q, room, bytes, pad = 0, off, first, n;

	/* frames written by the application and not yet handed to the DSP */
	pending = rt->control->appl_ptr - ms->abs;
	if (pending < 0)
		pending += rt->boundary;
	if (pending <= 0)
		return;

	/* one unit stays free: WP must never catch up with RP from behind */
	q = queued(ms);
	room = q + UNIT < ms->ring_size ? (ms->ring_size - q - UNIT) & ~(UNIT - 1) : 0;
	/* under one ALSA buffer per call, or the pointer could come back unchanged */
	room = min_t(u32, room, round_down(frames_to_bytes(rt, rt->buffer_size - 1), UNIT));
	bytes = min_t(u32, room, frames_to_bytes(rt, pending)) & ~(UNIT - 1);
	if (!bytes) {
		/* less than a unit left: only at the end of a drain, padded with silence */
		if (rt->state != SNDRV_PCM_STATE_DRAINING || room < UNIT)
			return;
		bytes = frames_to_bytes(rt, pending);
		pad = UNIT - bytes;
	}

	off = frames_to_bytes(rt, ms->abs % rt->buffer_size);
	first = min(bytes, buf - off);
	ring_put(ms, ms->wp, rt->dma_area + off, first);
	ring_put(ms, (ms->wp + first) % ms->ring_size, rt->dma_area, bytes - first);
	if (pad)
		ring_put(ms, (ms->wp + bytes) % ms->ring_size, NULL, pad);
	set_wp(ms, (ms->wp + bytes + pad) % ms->ring_size);

	n = bytes_to_frames(rt, bytes);
	ms->abs = (ms->abs + n) % rt->boundary;
	ms->period_acc += n;
	if (ms->period_acc >= rt->period_size) {
		ms->period_acc %= rt->period_size;
		snd_pcm_period_elapsed_under_stream_lock(ms->substream);
	}
}

static enum hrtimer_restart mixsnd_tick(struct hrtimer *t)
{
	struct mixsnd *ms = container_of(t, struct mixsnd, timer);
	struct snd_pcm_substream *ss = ms->substream;
	unsigned long flags;
	bool again;

	snd_pcm_stream_lock_irqsave(ss, flags);
	if (ms->running)
		feed(ms);
	again = ms->running;
	snd_pcm_stream_unlock_irqrestore(ss, flags);
	if (!again)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ns_to_ktime(TICK_NS));
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware mixsnd_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 2048,		/* ~10.7 ms: two timer ticks */
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = 64,
};

static int mixsnd_open(struct snd_pcm_substream *ss)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	ss->runtime->hw = mixsnd_hw;
	ms->substream = ss;
	return snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int mixsnd_close(struct snd_pcm_substream *ss)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&ms->timer);
	return 0;
}

static int mixsnd_prepare(struct snd_pcm_substream *ss)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	ms->abs = 0;
	ms->period_acc = 0;
	/* continue after whatever the DSP still has queued; WP is ours alone */
	ms->wp = round_down(dsp_to_off(ms, readl(ms->ch + CH_WP)), UNIT) % ms->ring_size;
	return 0;
}

static int mixsnd_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ms->running = true;
		/* first feed right away (not here: no period_elapsed from inside trigger) */
		hrtimer_start(&ms->timer, 0, HRTIMER_MODE_REL_SOFT);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* the timer callback takes the stream lock: no waiting here (see sync_stop);
		 * what is already in the DSP ring (<= 42 ms) plays out */
		ms->running = false;
		hrtimer_try_to_cancel(&ms->timer);
		return 0;
	}
	return -EINVAL;
}

static int mixsnd_sync_stop(struct snd_pcm_substream *ss)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&ms->timer);
	return 0;
}

static snd_pcm_uframes_t mixsnd_pointer(struct snd_pcm_substream *ss)
{
	struct mixsnd *ms = snd_pcm_substream_chip(ss);

	ss->runtime->delay = bytes_to_frames(ss->runtime, queued(ms));
	return ms->abs % ss->runtime->buffer_size;
}

static const struct snd_pcm_ops mixsnd_ops = {
	.open = mixsnd_open,
	.close = mixsnd_close,
	.prepare = mixsnd_prepare,
	.trigger = mixsnd_trigger,
	.sync_stop = mixsnd_sync_stop,
	.pointer = mixsnd_pointer,
};

/* volume step -> DSP gain: 0x20000 * 10^((v - 60) / 20), 0 = mute */
static const u32 vol_gain[VOL_MAX + 1] = {
	0x00000, 0x00093, 0x000a5, 0x000b9, 0x000d0, 0x000e9, 0x00106, 0x00125, 0x00149, 0x00171,
	0x0019e, 0x001d1, 0x0020a, 0x00249, 0x00291, 0x002e1, 0x0033b, 0x003a0, 0x00411, 0x00490,
	0x0051f, 0x005bf, 0x00672, 0x0073b, 0x0081d, 0x0091b, 0x00a37, 0x00b76, 0x00cdc, 0x00e6e,
	0x01031, 0x0122b, 0x01462, 0x016df, 0x019a9, 0x01ccb, 0x0204e, 0x0243f, 0x028ab, 0x02da2,
	0x03333, 0x03973, 0x04075, 0x04852, 0x05126, 0x05b0c, 0x06628, 0x0729f, 0x0809c, 0x0904d,
	0x0a1e9, 0x0b5aa, 0x0cbd5, 0x0e4b4, 0x1009c, 0x11feb, 0x1430d, 0x16a78, 0x196b2, 0x1c852,
	0x20000,
};

static const DECLARE_TLV_DB_SCALE(vol_tlv, -6000, 100, 1);

/* the DSP reads the word itself, no command needed (the stock driver does the same) */
static void set_gain(struct mixsnd *ms)
{
	writel(ms->on ? vol_gain[ms->vol] << 8 : 0, ms->ch + CH_GAIN);
}

static int vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = VOL_MAX;
	return 0;
}

static int vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct mixsnd *ms = snd_kcontrol_chip(kc);

	v->value.integer.value[0] = ms->vol;
	return 0;
}

static int vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct mixsnd *ms = snd_kcontrol_chip(kc);
	long val = v->value.integer.value[0];

	if (val < 0 || val > VOL_MAX)
		return -EINVAL;
	if (val == ms->vol)
		return 0;
	ms->vol = val;
	set_gain(ms);
	return 1;
}

static int mute_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct mixsnd *ms = snd_kcontrol_chip(kc);

	v->value.integer.value[0] = ms->on;
	return 0;
}

static int mute_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *v)
{
	struct mixsnd *ms = snd_kcontrol_chip(kc);
	bool on = v->value.integer.value[0];

	if (on == ms->on)
		return 0;
	ms->on = on;
	set_gain(ms);
	return 1;
}

static const struct snd_kcontrol_new mixsnd_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = vol_info,
		.get = vol_get,
		.put = vol_put,
		.tlv.p = vol_tlv,
	}, {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = mute_get,
		.put = mute_put,
	},
};

/* the stock DSP state this driver relies on: the ring from the DT, counters moving */
static int mixsnd_check_dsp(struct mixsnd *ms, u32 chan)
{
	u32 start = readl(ms->ch + CH_START), end = readl(ms->ch + CH_END);
	u32 before[NR_CH], i, moving = 0;

	if (ms->window + (start >> 8) != ms->ring_phys ||
	    ms->window + (end >> 8) != ms->ring_phys + ms->ring_size) {
		dev_err(ms->dev, "channel %u describes ring %08x..%08x, not %pa+%x: DSP not in the stock state\n",
			chan, ms->window + (start >> 8), ms->window + (end >> 8),
			&ms->ring_phys, ms->ring_size);
		return -ENODEV;
	}
	for (i = 0; i < NR_CH; i++)
		before[i] = readl(ms->ctrl + i * CH_STRIDE + CH_COUNTER);
	msleep(30);
	for (i = 0; i < NR_CH; i++)
		moving += readl(ms->ctrl + i * CH_STRIDE + CH_COUNTER) != before[i];
	if (!moving) {
		dev_err(ms->dev, "DSP counters are not moving: the audio DSP is not running\n");
		return -ENODEV;
	}
	dev_info(ms->dev, "DSP running (%u/%u channel counters moving), channel %u ring %pa+%x, rp %x wp %x\n",
		 moving, NR_CH, chan, &ms->ring_phys, ms->ring_size,
		 ring_rp(ms), dsp_to_off(ms, readl(ms->ch + CH_WP)));
	return 0;
}

static int mixsnd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct snd_pcm *pcm;
	struct mixsnd *ms;
	u32 chan;
	int ret, i;

	ms = devm_kzalloc(dev, sizeof(*ms), GFP_KERNEL);
	if (!ms)
		return -ENOMEM;
	ms->dev = dev;

	if (of_property_read_u32(dev->of_node, "netcast,channel", &chan) || chan >= NR_CH)
		return dev_err_probe(dev, -EINVAL, "bad netcast,channel\n");
	if (of_property_read_u32(dev->of_node, "netcast,dsp-window", &ms->window))
		return dev_err_probe(dev, -EINVAL, "missing netcast,dsp-window\n");

	/* plain ioremap, no request_mem_region: this is DSP memory, not ours */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl");
	if (!res || resource_size(res) < NR_CH * CH_STRIDE)
		return dev_err_probe(dev, -EINVAL, "bad ctrl region\n");
	ms->ctrl = devm_ioremap(dev, res->start, resource_size(res));
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ring");
	if (!res || resource_size(res) % UNIT)
		return dev_err_probe(dev, -EINVAL, "bad ring region\n");
	ms->ring_phys = res->start;
	ms->ring_size = resource_size(res);
	ms->ring = devm_ioremap_wc(dev, res->start, ms->ring_size);
	if (!ms->ctrl || !ms->ring)
		return -ENOMEM;
	ms->ch = ms->ctrl + chan * CH_STRIDE;

	ret = mixsnd_check_dsp(ms, chan);
	if (ret)
		return ret;

	hrtimer_init(&ms->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	ms->timer.function = mixsnd_tick;

	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "MT5398", THIS_MODULE, 0, &ms->card);
	if (ret)
		return ret;
	strscpy(ms->card->driver, "MT5398", sizeof(ms->card->driver));
	strscpy(ms->card->shortname, "TV speakers", sizeof(ms->card->shortname));
	snprintf(ms->card->longname, sizeof(ms->card->longname),
		 "MT5398 audio DSP, MixSnd channel %u", chan);

	ret = snd_pcm_new(ms->card, "MT5398 MixSnd", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = ms;
	strscpy(pcm->name, "TV speakers", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &mixsnd_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	strscpy(ms->card->mixername, "MT5398 MixSnd", sizeof(ms->card->mixername));
	for (i = 0; i < ARRAY_SIZE(mixsnd_controls); i++) {
		ret = snd_ctl_add(ms->card, snd_ctl_new1(&mixsnd_controls[i], ms));
		if (ret)
			return ret;
	}
	dev_info(dev, "channel gain: stock left 0x%x, now 0x%x\n",
		 readl(ms->ch + CH_GAIN) >> 8, vol_gain[VOL_START]);
	ms->vol = VOL_START;
	ms->on = true;
	set_gain(ms);

	ret = snd_card_register(ms->card);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, ms);
	return 0;
}

static const struct of_device_id mixsnd_of_match[] = {
	{ .compatible = "netcast,mt5398-mixsnd" },
	{ }
};
MODULE_DEVICE_TABLE(of, mixsnd_of_match);

static struct platform_driver mixsnd_driver = {
	.probe = mixsnd_probe,
	.driver = {
		.name = "mt5398-mixsnd",
		.of_match_table = mixsnd_of_match,
	},
};
module_platform_driver(mixsnd_driver);

MODULE_DESCRIPTION("MT5398 audio DSP MixSnd PCM playback (LG NetCast)");
MODULE_LICENSE("GPL");
