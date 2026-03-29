#include<linux/module.h>
#include<linux/printk.h>
#include<linux/sched.h>
#include<linux/debugfs.h>
#include<linux/seq_file.h>
#include <linux/pgtable.h>
#include <linux/pagewalk.h>
#include <linux/kprobes.h>
#include <asm/tlbflush.h>
#include<linux/mm.h>

int pidnr = 0;


static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* On x86_64, handle_mm_fault arguments are:
	 * %rdi: struct vm_area_struct *vma
	 * %rsi: unsigned long address
	 * %rdx: unsigned int flags
	 */
	struct vm_area_struct *vma = (struct vm_area_struct *)regs->di;
	unsigned long addr = regs->si;
	unsigned int flags = regs->dx;
	// Check if it's a WRITE fault on a page we marked Read-Only
	if ((flags & FAULT_FLAG_WRITE) && (vma->vm_flags & VM_WRITE)) {
		/* 1. Manually find the page
		 * 2. kmalloc a new page
		 * 3. copy_page(new_page, old_page)
		 * 4. Store your copy in a RADIX Tree - then later you can access the 
		 *    read only pages from the process and from this RADIX tree - this
		 *    will be your original process task tree.
		 *    You will have to use the VMA to mark the pages writeable when you
		 *    have return the checkpoint!
		 */
		pr_info("Intercepted COW fault at 0x%lx\n", addr);
		struct mm_struct *mm = vma->vm_mm;
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *ptep, pte;
		spinlock_t *ptl;

		/* 1. Standard Page Table Walk */
		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			return -1; 
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			return -2;
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_bad(*pud))
			return -3;
		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			return -4;
		/*2. Lock the PTE level */
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!ptep)
			return -5;
		pte = *ptep;
		if (pte_present(pte)) {
			/* 3. Set the Write bit */
			pte = pte_mkwrite_novma(pte);
			/* 4. Mark the page modified */
			pte = pte_mkdirty(pte); 

			/* 5. Update the Hardware Table */
			set_pte_at(mm, addr, ptep, pte);

			/* 6. CRITICAL: Flush the TLB for this address
			 * If you don't flush, the CPU will still think it's Read-Only
			 * and trigger an infinite loop of page faults!
			 */
			//flush_tlb_page(vma, addr);
		}
		pte_unmap_unlock(ptep, ptl);
	}
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "handle_mm_fault",
	.pre_handler = handler_pre,
};



void walk_task_vma_pages(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	unsigned long addr;


	for (addr = start; addr < end; addr += PAGE_SIZE) {
		// 1. Find the Global Directory entry
		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			pr_info("\n Could not find the global directory");
			// Jump to the next PGD boundary to save time
			addr = (addr + PGDIR_SIZE) & PGDIR_MASK;
			addr -= PAGE_SIZE; // Adjust for loop increment
			continue;
		}
		// 2. Find the 4th level (often folded into PGD)
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			pr_info("\n Could not find the 4th level page directory");
			continue;
		}
		// 3. Find the Upper Directory
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_bad(*pud)) {
			pr_info("\n Could not find the 3rd level upper directory");
			continue;
		}
		// 4. Find the Middle Directory (where Huge Pages often live)
		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd) || pmd_bad(*pmd)) {
			pr_info("\n Could not find the 2nd level middle directory");
			continue;
		}
		// 5. Lock and Map the Page Table Entry (PTE)
		// This returns a pointer to the PTE and locks the Page Table Lock (ptl)
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!ptep) {
			pr_info("\n Could not lock the pte entry ");
			continue;
		}
		pte = *ptep;
		if (pte_present(pte)) {
			unsigned long pfn = pte_pfn(pte);
			pr_info("Virtual 0x%lx -> Physical 0x%lx\n", addr, pfn << PAGE_SHIFT);
			// Only apply if it's currently writable in the PTE
			if (pte_write(*ptep)) {
				pte_t pte = pte_wrprotect(*ptep);
				set_pte_at(mm, addr, ptep, pte);
			}
		}
		// 6. Cleanup
		pte_unmap_unlock(ptep, ptl);
	}
}

static int my_pte_callback(pte_t *ptep, unsigned long addr, 
				unsigned long next, struct mm_walk *walk)
{
	pte_t pte = *ptep;
	if (pte_present(pte)) {
		// Apply your COW logic here
		pte = pte_wrprotect(pte);
		set_pte_at(walk->mm, addr, ptep, pte);
	}
	return 0;
}

static const struct mm_walk_ops my_walk_ops = {
	.pte_entry = my_pte_callback,
};



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

void protect_all_pages_of_task(struct task_struct *task)
{
	struct mm_struct *mm = task->mm;
	struct vm_area_struct *vma;

	if(!mm)
		return;

	VMA_ITERATOR(vmi, mm, 0);
	// 1. Lock the address space for the entire duration
	mmap_read_lock(mm);
	// 2. Iterate over every VMA in the task
	for_each_vma(vmi, vma) {
		// Skip special VMAs that shouldn't be COW (like VDSO or special device maps)
		if (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_DONTEXPAND))
			continue;
		// 3. Walk the page tables for this specific VMA range
		walk_task_vma_pages(vma, vma->vm_start, vma->vm_end);
		// 4. Flush the TLB for this VMA range once
		//flush_tlb_range(vma, vma->vm_start, vma->vm_end);
	}
	mmap_read_unlock(mm);
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
		debugfs_create_file("process_mem", 0444, debug_dir, task->mm, &my_fops);
	} else {
		debugfs_remove_recursive(debug_dir);
		return -1;
	}
	protect_all_pages_of_task(task);
	register_kprobe(&kp);
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
