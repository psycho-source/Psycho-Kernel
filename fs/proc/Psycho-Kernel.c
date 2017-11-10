#include <linux/fs.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <generated/compile.h>

static int psycho_kernel_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "{\"kernel-name\": \"Psycho-Kernel_x507\","
			"\"version\": \"v7.0\","
			"\"buildtime\": \"%s\"}\n", PSYCHO_KERNEL_TIMESTAMP);
	return 0;
}

static int psycho_kernel_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, psycho_kernel_proc_show, NULL);
}

static const struct file_operations psycho_kernel_proc_fops = {
	.open		= psycho_kernel_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_psycho_kernel_init(void)
{
	proc_create("Psycho-Kernel", 0, NULL, &psycho_kernel_proc_fops);
	return 0;
}
module_init(proc_psycho_kernel_init);
