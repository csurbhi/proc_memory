#include<linux/module.h>
#include<linux/printk.h>
#include<linux/mm.h>
#include<linux/sched.h>
#include<linux/debugfs.h>
#include<linux/seq_file.h>

int pidnr = 0;

/* 1. The 'Start' function: Initializes the iterator */
static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	struct mm_struct *mm = s->private; // We pass mm_struct via debugfs_create_file
	struct vm_area_struct *vma;
	unsigned long i = 0;

	// Standard VMA traversal logic
	VMA_ITERATOR(vmi, mm, 0);
	for_each_vma(vmi, vma) {
		if (i == *pos)
			return vma; // Return the VMA at the current position
		i++;
	}
	return NULL;
}

/* 2. The 'Next' function: Moves to the next VMA */
static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct mm_struct *mm = s->private;
	struct vm_area_struct *vma = v;
	(*pos)++;
	// We need to re-initialize or use a persistent iterator
	VMA_ITERATOR(vmi, mm, vma->vm_end);
	return vma_next(&vmi); // Helper to get the next VMA in the list
}

/* 3. The 'Stop' function: Cleanup */
static void my_seq_stop(struct seq_file *s, void *v) { }

/* 4. The 'Show' function: This is where you actually print the addresses */
static int my_seq_show(struct seq_file *s, void *v)
{
	struct vm_area_struct *vma = v;
	// Print the start and end address of the VMA
	seq_printf(s, "VMA: 0x%lx - 0x%lx\n", vma->vm_start, vma->vm_end);
	// Note: If you want to print every individual PAGE within this VMA:
	unsigned long addr;
	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		seq_printf(s, "  Page: 0x%lx\n", addr);
	}
	return 0;
}

/* Define the sequence operations */
static const struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next  = my_seq_next,
	.stop  = my_seq_stop,
	.show  = my_seq_show,
};

/* Boilerplate to hook seq_file into debugfs */
static int my_debug_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &my_seq_ops);
	if (!ret) {
		struct seq_file *m = file->private_data;
		m->private = inode->i_private; // Pass the mm_struct pointer
	}
	return ret;
}

static const struct file_operations my_fops = {
	.owner   = THIS_MODULE,
	.open    = my_debug_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

struct dentry * debug_dir;

static int __init procmem_init(void)
{
	printk(KERN_INFO "\n Hello World! pid: %d ", pidnr);
	debug_dir = debugfs_create_dir("proc-mem", NULL);
	if (!debug_dir) {
		pr_err("\n Could not create debugfs dir ");
	}
	struct pid *pid = find_get_pid(pidnr);
	if (!pid) {
		pr_info("\n cannot find pid info for: %d ", pidnr);
		debugfs_remove_recursive(debug_dir);
		return -ESRCH;
	}
	struct task_struct *task = get_pid_task(pid, PIDTYPE_PID);
	if (task) {
		/* task->mm is the private pointer that will be stored in the inode->i_private */
		debugfs_create_file("process_pages", 0444, debug_dir, task->mm, &my_fops);
	} else {
		debugfs_remove_recursive(debug_dir);
	}
	return 0;
}

static void __exit procmem_exit(void)
{
	debugfs_remove_recursive(debug_dir);
	pr_info("\nGoodbye world! \n");
}

module_param(pidnr, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

module_init(procmem_init);
module_exit(procmem_exit);


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Prints process memory");  
