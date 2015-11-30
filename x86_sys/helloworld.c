#include <linux/linkage.h>
#include <linux/kernel.h>

#include <linux/mman.h>
#include <asm/pgtable_32.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

#include "mymm.h"

void follow_pte(struct mm_struct * mm, unsigned long address, pte_t * entry)
{
    pgd_t * pgd = pgd_offset(mm, address);

    // printk("follow_pte() for %lx\n", address);

    entry->pte = 0;
    if (!pgd_none(*pgd) && !pgd_bad(*pgd)) {
        pud_t * pud = pud_offset(pgd, address);
        struct vm_area_struct * vma = find_vma(mm, address);

        // printk(" pgd = %lx\n", pgd_val(*pgd));

        if (pud_none(*pud)) {
            // printk("  pud = empty\n");
            return;
        }
        if (pud_huge(*pud) && vma->vm_flags & VM_HUGETLB) {
            entry->pte = pud_val(*pud);
            printk("  pud = huge\n");
            return;
        }

        if (!pud_bad(*pud)) {
            pmd_t * pmd = pmd_offset(pud, address);

            // printk("  pud = %lx\n", pud_val(*pud));

            if (pmd_none(*pmd)) {
                // printk("   pmd = empty\n");
                return;
            }
            if (pmd_huge(*pmd) && vma->vm_flags & VM_HUGETLB) { // 
                entry->pte = pmd_val(*pmd);
                printk("   pmd = huge\n");
                return;
            }
            if (pmd_trans_huge(*pmd)) { // important
                entry->pte = pmd_val(*pmd);
                printk("   pmd = trans_huge\n");
                return;
            }
            if (!pmd_bad(*pmd)) {
                pte_t * pte = pte_offset_map(pmd, address);

                // printk("   pmd = %lx\n", pmd_val(*pmd));

                if (!pte_none(*pte)) {
                    entry->pte = pte_val(*pte);
                    // printk("    pte = %lx\n", pte_val(*pte));
                } else {
                    // printk("    pte = empty\n");
                }
                pte_unmap(pte);
            }
        }
    }
}


unsigned long vm2phy(struct mm_struct* mm, unsigned long virt)
{
    // struct vm_area_struct *vma = find_vma(mm, virt);
    // struct page* p = follow_page(vma, virt, 0);
    // return page_to_phys(*p);
    pte_t pte2;
    follow_pte(mm, virt, &pte2);

    return pte_pfn(pte2);
}




void print_vma_list(unsigned int pid, struct page_mm_data* arr)
{
    struct task_struct* task;
    task = pid_task(find_get_pid(pid), PIDTYPE_PID);
    struct vm_area_struct *vma = 0;
    unsigned long vpage;
    unsigned int count=0;
    if (task->mm && task->mm->mmap)
        for (vma = task->mm->mmap; vma; vma = vma->vm_next) 
        {
            //printk(KERN_INFO "vma->vm_start: %p, vm_end: %p\n", vma->vm_start, vma->vm_end);
            for (vpage = vma->vm_start; vpage < vma->vm_end; vpage += PAGE_SIZE)
            {
                // printk(KERN_INFO "");
                unsigned long phy = vm2phy(task->mm, vpage);
                // unsigned long phy = follow_page(vma, vpage, 0);


                if(phy != 0)
                {
                    // count++;
                    struct page_mm_data s = {};
                    // printk(KERN_INFO "phy: %p\n", phy);
                    s.va = vpage;
                    s.pa = phy;
                    if(count < MY_MM_DATA_MAX)
                        copy_to_user(arr+count, &s, sizeof(struct page_mm_data));
                    else
                        printk(KERN_INFO "Buffer exhusted\n");
                    count++;
                    // arr[count].va = vpage;
                    // arr[count++].pa = phy;
                }
                ;
            }
        }
    printk(KERN_INFO "phy map count: %u\n", count);
    //printk(KERN_INFO "0x6e910000: %p\n", vm2phy(task->mm, 0x6e910000));
    //printk(KERN_INFO "0x6e900000: %p\n", vm2phy(task->mm, 0x6e900000));


}

asmlinkage int sys_helloworld(unsigned int pid, void __user *buf) {
	printk(KERN_EMERG "Syscall By leepupu");
	print_vma_list(pid, (struct page_mm_data*)buf);
	return 1;
}

