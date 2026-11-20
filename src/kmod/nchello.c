/* nchello: minimal test module for the LG NetCast 3.0.13 kernel. Logs on load/unload, nothing else. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init nchello_init(void)
{
	printk(KERN_INFO "nchello: hello from NetCast+ (module loaded)\n");
	return 0;
}

static void __exit nchello_exit(void)
{
	printk(KERN_INFO "nchello: unloaded\n");
}

module_init(nchello_init);
module_exit(nchello_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NetCast+ test module");
