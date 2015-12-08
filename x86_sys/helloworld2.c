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

#define PTE_LEVEL_MULT (PAGE_SIZE)
#define PMD_LEVEL_MULT (PTRS_PER_PTE * PTE_LEVEL_MULT)
#define PUD_LEVEL_MULT (PTRS_PER_PMD * PMD_LEVEL_MULT)
#define PGD_LEVEL_MULT (PTRS_PER_PUD * PUD_LEVEL_MULT)


void dump_page_table2(unsigned int pid, struct page_mm_data * g_page_data)
{
    unsigned int g_page_data_count=0;

    struct task_struct* ts;
    ts = pid_task(find_get_pid(pid), PIDTYPE_PID);
    pgd_t *start = ts->mm->pgd;

    int i, j, k;
    for (i = 0; i < PTRS_PER_PGD; i++) {
        if (!pgd_none(*start)) {
            pud_t * pud = pud_offset(start, i * PGD_LEVEL_MULT);

            if (pud_none(*pud)) {
                printk("pud is none at %d\n", i);
                continue;
            }

            if (!pud_bad(*pud)) {
                pmd_t * pmd = pmd_offset(pud, i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j);
                for(j=0;j<PTRS_PER_PMD;j++, pmd++) {
                    if(pmd_none(*pmd)) {
                        // printk("pmd is none at %d\n", j);
                        continue;
                    }
                    if(i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j >= PAGE_OFFSET) break;
                    printk(KERN_INFO "now vaddr: %p\n", i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j);
                    if(pmd_huge(*pmd)) {
                        printk(KERN_INFO "pmd_huge\n");
                    }
                    if(pmd_trans_huge(*pmd)) {
                        printk(KERN_INFO "pmd_trans_huge\n");
                        int g=0;
                        pte_t pte;
                        pte.pte = pmd_val(*pmd);
                        for(g=0;g<512;g++)
                        {
                            struct page_mm_data pd = {}; 
                            pd.va = i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j+g*PTE_LEVEL_MULT;
                            pd.pa = pte_pfn(pte);
                            copy_to_user(g_page_data+(g_page_data_count++), &pd, sizeof(struct page_mm_data));
                        }
                    }
                    if(!pmd_bad(*pmd)) {
                        for(k=0;k<PTRS_PER_PTE;k++) {
                            pte_t * pte = pte_offset_map(pmd, i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j+k*PTE_LEVEL_MULT);
                            if(pte_none(*pte)) {
                                pte_unmap(pte);
                                continue;
                            }

                            struct page_mm_data pd = {};
                            
                            pd.va = i * PGD_LEVEL_MULT + PMD_LEVEL_MULT*j+k*PTE_LEVEL_MULT;
                            pd.pa = pte_pfn(*pte);
                            copy_to_user(g_page_data+(g_page_data_count++), &pd, sizeof(struct page_mm_data));
                            pte_unmap(pte);
                        }

                    }
                }
            }

        }
        start++;
    }
    return;
}

asmlinkage int sys_helloworld2(unsigned int pid, void __user *buf) {
	printk(KERN_EMERG "Syscall By leepupu");
	dump_page_table2(pid, (struct page_mm_data*)buf);
	return 1;
}

