// SPDX-License-Identifier: GPL-2.0
/*
 * vdecbuf: physically contiguous buffer for the MT5398 video decoder bring-up test (mainline).
 * /dev/vdecbuf: count= buffers of size= bytes each. mmap at offset k * size maps buffer k
 * (uncached); VDECBUF_PHYS takes k and returns buffer k's physical address.
 * The registers are driven from user space (vdectest.c, /dev/mem). Not a decoder driver.
 *
 * The pages come from the page allocator with GFP_KERNEL, so they are lowmem: bank 0 on the
 * TV, the RAM the decoder is known to reach (the default CMA area sits in bank 1). They are
 * cleaned from the cache once and then only used through an uncached user mapping.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/gfp.h>
#include <linux/io.h>

#define VDECBUF_PHYS	_IOWR('v', 2, __u32)
#define MAXBUF		8

static unsigned int size = 4 << 20;	/* largest the page allocator gives without CMA */
module_param(size, uint, 0444);
MODULE_PARM_DESC(size, "buffer size in bytes (page multiple)");

static unsigned int count = 1;
module_param(count, uint, 0444);
MODULE_PARM_DESC(count, "number of buffers (1..8)");

static struct platform_device *pdev;
static struct page *pages[MAXBUF];
static dma_addr_t pa[MAXBUF];

static int vb_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start, k = vma->vm_pgoff / (size >> PAGE_SHIFT);

	if (vma->vm_pgoff % (size >> PAGE_SHIFT) || k >= count || len > size)
		return -EINVAL;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, page_to_pfn(pages[k]), len, vma->vm_page_prot);
}

static long vb_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	u32 k;

	if (cmd != VDECBUF_PHYS)
		return -ENOTTY;
	if (copy_from_user(&k, (void __user *)arg, sizeof(k)))
		return -EFAULT;
	if (k >= count)
		return -EINVAL;
	k = pa[k];
	return copy_to_user((void __user *)arg, &k, sizeof(k)) ? -EFAULT : 0;
}

static const struct file_operations vb_fops = {
	.owner = THIS_MODULE, .mmap = vb_mmap, .unlocked_ioctl = vb_ioctl,
};

static struct miscdevice vb_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "vdecbuf", .fops = &vb_fops, .mode = 0600,
};

static void vb_free(unsigned int n)
{
	while (n--) {
		dma_unmap_page(&pdev->dev, pa[n], size, DMA_BIDIRECTIONAL);
		__free_pages(pages[n], get_order(size));
	}
}

static int __init vb_init(void)
{
	unsigned int k;
	int ret;

	if (!size || size & ~PAGE_MASK || get_order(size) > MAX_PAGE_ORDER || !count || count > MAXBUF)
		return -EINVAL;
	pdev = platform_device_register_simple("vdecbuf", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto unreg;
	for (k = 0; k < count; k++) {
		pages[k] = alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN, get_order(size));
		if (!pages[k]) {
			ret = -ENOMEM;
			goto free;
		}
		/* write the zeroed lines back and drop them; from now on only the uncached mapping */
		pa[k] = dma_map_page(&pdev->dev, pages[k], 0, size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(&pdev->dev, pa[k]) || pa[k] != page_to_phys(pages[k])) {
			if (!dma_mapping_error(&pdev->dev, pa[k]))
				dma_unmap_page(&pdev->dev, pa[k], size, DMA_BIDIRECTIONAL);
			__free_pages(pages[k], get_order(size));
			ret = -EIO;
			goto free;
		}
		pr_info("vdecbuf: buffer %u: %u bytes at %pad\n", k, size, &pa[k]);
	}
	ret = misc_register(&vb_misc);
	if (ret)
		goto free;
	return 0;
free:
	vb_free(k);
unreg:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit vb_exit(void)
{
	misc_deregister(&vb_misc);
	vb_free(count);
	platform_device_unregister(pdev);
}

module_init(vb_init);
module_exit(vb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Buffer for the MT5398 video decoder bring-up test");
