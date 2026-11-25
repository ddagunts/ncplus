// SPDX-License-Identifier: GPL-2.0
/*
 * xpadone: Xbox One / Series X|S wired gamepads (GIP protocol) for the LG NetCast 3.0.13 kernel.
 *
 * The TV's kernel has xpad built in, but that 3.0-era driver predates Xbox One support, so GIP pads
 * (interface class ff/47/d0) are left unclaimed. This is a small separate driver for just those pads.
 * The packet layout follows upstream drivers/input/joystick/xpad.c (xpadone_process_packet).
 * Rumble is not implemented. Struct offsets used here were checked against LG's kernel (see probe).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <asm/unaligned.h>

#define GIP_CMD_ACK          0x01
#define GIP_CMD_ANNOUNCE     0x02
#define GIP_CMD_POWER        0x05
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_INPUT        0x20
#define GIP_OPT_ACK          0x10
#define GIP_OPT_INTERNAL     0x20
#define GIP_PWR_ON           0x00

#define XO_OUT_SIZE 64

struct xpadone {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct input_dev *dev;
	char phys[64];
	char name[128];

	struct urb *irq_in;
	unsigned char *idata;
	dma_addr_t idata_dma;
	int isize;

	struct urb *irq_out;
	unsigned char *odata;
	dma_addr_t odata_dma;
	spinlock_t olock;
	bool out_busy;
	bool dead;
	u8 seq;
	/* one pending packet, sent when the out URB frees up */
	unsigned char pend[XO_OUT_SIZE];
	int pend_len;
};

/* Caller holds olock. */
static int xo_submit_locked(struct xpadone *xo, const u8 *pkt, int len)
{
	int err;
	if (xo->dead)
		return -ENODEV;
	if (xo->out_busy) {
		memcpy(xo->pend, pkt, len);
		xo->pend_len = len;
		return 0;
	}
	memcpy(xo->odata, pkt, len);
	xo->irq_out->transfer_buffer_length = len;
	err = usb_submit_urb(xo->irq_out, GFP_ATOMIC);
	if (err == 0)
		xo->out_busy = true;
	else
		dev_warn(&xo->intf->dev, "xpadone: out submit failed: %d\n", err);
	return err;
}

static void xo_send(struct xpadone *xo, const u8 *pkt, int len)
{
	unsigned long flags;
	spin_lock_irqsave(&xo->olock, flags);
	xo_submit_locked(xo, pkt, len);
	spin_unlock_irqrestore(&xo->olock, flags);
}

static void xo_power_on(struct xpadone *xo)
{
	u8 pkt[5] = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 0x01, GIP_PWR_ON };
	unsigned long flags;
	spin_lock_irqsave(&xo->olock, flags);
	pkt[2] = xo->seq++;
	xo_submit_locked(xo, pkt, sizeof pkt);
	spin_unlock_irqrestore(&xo->olock, flags);
}

static void xo_irq_out(struct urb *urb)
{
	struct xpadone *xo = urb->context;
	unsigned long flags;

	spin_lock_irqsave(&xo->olock, flags);
	xo->out_busy = false;
	if (urb->status && urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
		dev_dbg(&xo->intf->dev, "xpadone: out status %d\n", urb->status);
	if (xo->pend_len && !xo->dead && urb->status != -ESHUTDOWN) {
		int len = xo->pend_len;
		xo->pend_len = 0;
		xo_submit_locked(xo, xo->pend, len);
	}
	spin_unlock_irqrestore(&xo->olock, flags);
}

static void xo_process(struct xpadone *xo, const u8 *d, int len)
{
	struct input_dev *dev = xo->dev;

	if (len < 4)
		return;

	if (d[0] == GIP_CMD_ANNOUNCE) {
		/* the pad (re)announced itself: tell it to start sending input */
		xo_power_on(xo);
		return;
	}

	if (d[0] == GIP_CMD_VIRTUAL_KEY && len >= 5) {   /* Guide button */
		if (d[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
			u8 ack[13] = { GIP_CMD_ACK, GIP_OPT_INTERNAL, d[2], 0x09,
				       0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x01, 0x00, 0, 0, 0, 0 };
			xo_send(xo, ack, sizeof ack);
		}
		input_report_key(dev, BTN_MODE, d[4] & 0x01);
		input_sync(dev);
		return;
	}

	if (d[0] != GIP_CMD_INPUT || len < 18)
		return;

	input_report_key(dev, BTN_START,  d[4] & BIT(2));   /* Menu */
	input_report_key(dev, BTN_SELECT, d[4] & BIT(3));   /* View */
	input_report_key(dev, BTN_A, d[4] & BIT(4));
	input_report_key(dev, BTN_B, d[4] & BIT(5));
	input_report_key(dev, BTN_X, d[4] & BIT(6));
	input_report_key(dev, BTN_Y, d[4] & BIT(7));

	input_report_abs(dev, ABS_HAT0X, !!(d[5] & BIT(3)) - !!(d[5] & BIT(2)));
	input_report_abs(dev, ABS_HAT0Y, !!(d[5] & BIT(1)) - !!(d[5] & BIT(0)));

	input_report_key(dev, BTN_TL, d[5] & BIT(4));
	input_report_key(dev, BTN_TR, d[5] & BIT(5));
	input_report_key(dev, BTN_THUMBL, d[5] & BIT(6));
	input_report_key(dev, BTN_THUMBR, d[5] & BIT(7));

	input_report_abs(dev, ABS_Z,  get_unaligned_le16(d + 6));   /* 0..1023 */
	input_report_abs(dev, ABS_RZ, get_unaligned_le16(d + 8));
	input_report_abs(dev, ABS_X,  (s16)get_unaligned_le16(d + 10));
	input_report_abs(dev, ABS_Y,  ~(s16)get_unaligned_le16(d + 12));
	input_report_abs(dev, ABS_RX, (s16)get_unaligned_le16(d + 14));
	input_report_abs(dev, ABS_RY, ~(s16)get_unaligned_le16(d + 16));
	input_sync(dev);
}

static void xo_irq_in(struct urb *urb)
{
	struct xpadone *xo = urb->context;
	int err;

	switch (urb->status) {
	case 0:
		xo_process(xo, xo->idata, urb->actual_length);
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;   /* killed or unplugged */
	default:
		dev_dbg(&xo->intf->dev, "xpadone: in status %d\n", urb->status);
		break;
	}
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -EPERM && err != -ENODEV)
		dev_err(&xo->intf->dev, "xpadone: in resubmit failed: %d\n", err);
}

static int xo_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep_in = NULL, *ep_out = NULL;
	struct xpadone *xo;
	struct input_dev *dev;
	static const int btns[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
				    BTN_MODE, BTN_THUMBL, BTN_THUMBR };
	int i, err = -ENOMEM;

	/* GIP pads expose input on interface 0; the others are audio/security */
	if (alt->desc.bInterfaceNumber != 0)
		return -ENODEV;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
		if (!ep_in && usb_endpoint_is_int_in(ep))
			ep_in = ep;
		else if (!ep_out && usb_endpoint_is_int_out(ep))
			ep_out = ep;
	}
	if (!ep_in || !ep_out)
		return -ENODEV;

	xo = kzalloc(sizeof *xo, GFP_KERNEL);
	dev = input_allocate_device();
	if (!xo || !dev)
		goto fail_alloc;
	xo->udev = udev;
	xo->intf = intf;
	xo->dev = dev;
	spin_lock_init(&xo->olock);

	xo->isize = le16_to_cpu(ep_in->wMaxPacketSize);
	if (xo->isize < 32)
		xo->isize = 64;
	xo->idata = usb_alloc_coherent(udev, xo->isize, GFP_KERNEL, &xo->idata_dma);
	xo->odata = usb_alloc_coherent(udev, XO_OUT_SIZE, GFP_KERNEL, &xo->odata_dma);
	xo->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	xo->irq_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!xo->idata || !xo->odata || !xo->irq_in || !xo->irq_out)
		goto fail_bufs;

	usb_fill_int_urb(xo->irq_in, udev, usb_rcvintpipe(udev, ep_in->bEndpointAddress),
			 xo->idata, xo->isize, xo_irq_in, xo, ep_in->bInterval);
	xo->irq_in->transfer_dma = xo->idata_dma;
	xo->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_fill_int_urb(xo->irq_out, udev, usb_sndintpipe(udev, ep_out->bEndpointAddress),
			 xo->odata, XO_OUT_SIZE, xo_irq_out, xo, ep_out->bInterval);
	xo->irq_out->transfer_dma = xo->odata_dma;
	xo->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	snprintf(xo->name, sizeof xo->name, "Xbox One pad %04x:%04x",
		 le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct));
	usb_make_path(udev, xo->phys, sizeof xo->phys);
	strlcat(xo->phys, "/input0", sizeof xo->phys);

	dev->name = xo->name;
	dev->phys = xo->phys;
	usb_to_input_id(udev, &dev->id);
	/*
	 * LG's struct input_dev is 136 bytes larger than vanilla 3.0.13 after keybit (open/close at
	 * 500/504, dev at 560 vs 364/368/424 here). Only touch name/phys/id/evbit/keybit, which were
	 * checked against LG's built-in xpad_probe; no dev.parent, drvdata or open/close.
	 */

	__set_bit(EV_KEY, dev->evbit);
	__set_bit(EV_ABS, dev->evbit);
	for (i = 0; i < ARRAY_SIZE(btns); i++)
		__set_bit(btns[i], dev->keybit);
	input_set_abs_params(dev, ABS_X, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Y, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RX, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RY, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_RZ, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = input_register_device(dev);
	if (err)
		goto fail_bufs;
	usb_set_intfdata(intf, xo);

	/* Keep the IN URB running from probe on, so announce packets are seen without an open(). */
	err = usb_submit_urb(xo->irq_in, GFP_KERNEL);
	if (err) {
		usb_set_intfdata(intf, NULL);
		input_unregister_device(dev);
		dev = NULL;   /* freed by unregister */
		goto fail_bufs;
	}
	xo_power_on(xo);
	dev_info(&intf->dev, "xpadone: %s (%04x:%04x) connected\n", xo->name,
		 le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct));
	return 0;

fail_bufs:
	usb_free_urb(xo->irq_in);
	usb_free_urb(xo->irq_out);
	if (xo->idata)
		usb_free_coherent(udev, xo->isize, xo->idata, xo->idata_dma);
	if (xo->odata)
		usb_free_coherent(udev, XO_OUT_SIZE, xo->odata, xo->odata_dma);
fail_alloc:
	if (dev)
		input_free_device(dev);
	kfree(xo);
	return err;
}

static void xo_disconnect(struct usb_interface *intf)
{
	struct xpadone *xo = usb_get_intfdata(intf);
	unsigned long flags;

	if (!xo)
		return;
	usb_set_intfdata(intf, NULL);
	spin_lock_irqsave(&xo->olock, flags);
	xo->dead = true;
	xo->pend_len = 0;
	spin_unlock_irqrestore(&xo->olock, flags);
	usb_kill_urb(xo->irq_in);
	usb_kill_urb(xo->irq_out);
	input_unregister_device(xo->dev);
	usb_free_urb(xo->irq_in);
	usb_free_urb(xo->irq_out);
	usb_free_coherent(xo->udev, xo->isize, xo->idata, xo->idata_dma);
	usb_free_coherent(xo->udev, XO_OUT_SIZE, xo->odata, xo->odata_dma);
	kfree(xo);
}

/*
 * GIP interface class/subclass/protocol, per vendor. The device class of these pads is also 0xff, and
 * usb_match_one_id() ignores interface-only matches on vendor-class devices unless the entry names a
 * vendor, so a bare USB_INTERFACE_INFO never matches (USB_VENDOR_AND_INTERFACE_INFO is 3.5+).
 * Vendor list from upstream xpad.c. Others: echo "VID PID ff" > /sys/bus/usb/drivers/xpadone/new_id
 */
#define XO_VENDOR(v) { .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (v), .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .bInterfaceSubClass = 0x47, \
	.bInterfaceProtocol = 0xd0 }
static const struct usb_device_id xo_table[] = {
	XO_VENDOR(0x03f0),   /* HP / HyperX */
	XO_VENDOR(0x044f),   /* Thrustmaster */
	XO_VENDOR(0x045e),   /* Microsoft */
	XO_VENDOR(0x0738),   /* Mad Catz */
	XO_VENDOR(0x0b05),   /* ASUS */
	XO_VENDOR(0x0e6f),   /* PDP / Afterglow */
	XO_VENDOR(0x0f0d),   /* Hori */
	XO_VENDOR(0x10f5),   /* Turtle Beach */
	XO_VENDOR(0x1532),   /* Razer */
	XO_VENDOR(0x20d6),   /* PowerA / BDA */
	XO_VENDOR(0x24c6),   /* PowerA */
	XO_VENDOR(0x294b),   /* Snakebyte */
	XO_VENDOR(0x2dc8),   /* 8BitDo */
	XO_VENDOR(0x2e24),   /* Hyperkin */
	XO_VENDOR(0x2e95),   /* SCUF */
	XO_VENDOR(0x3285),   /* Nacon */
	XO_VENDOR(0x3537),   /* GameSir */
	{ }
};
MODULE_DEVICE_TABLE(usb, xo_table);

static struct usb_driver xo_driver = {
	.name       = "xpadone",
	.probe      = xo_probe,
	.disconnect = xo_disconnect,
	.id_table   = xo_table,
};

static int __init xo_init(void)
{
	return usb_register(&xo_driver);
}

static void __exit xo_exit(void)
{
	usb_deregister(&xo_driver);
}

module_init(xo_init);
module_exit(xo_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xbox One/Series wired gamepad (GIP) driver for ncplus");
