#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <asm/paravirt.h>

#include <linux/pid.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

#include <linux/mman.h>
#include <asm/pgtable_32.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include "mymm.h"

unsigned long **sys_call_table;
unsigned long original_cr0;

//extern struct task_struct *find_task_by_vpid(pid_t nr);

/* Multipliers for offsets within the PTEs */
#define PTE_LEVEL_MULT (PAGE_SIZE)
#define PMD_LEVEL_MULT (PTRS_PER_PTE * PTE_LEVEL_MULT)
#define PUD_LEVEL_MULT (PTRS_PER_PMD * PMD_LEVEL_MULT)
#define PGD_LEVEL_MULT (PTRS_PER_PUD * PUD_LEVEL_MULT)

int pmd_huge(pmd_t pmd)
{
        return !!(pmd_val(pmd) & _PAGE_PSE);
}

int pud_huge(pud_t pud)
{
        return !!(pud_val(pud) & _PAGE_PSE);
}


enum address_markers_idx {
    USER_SPACE_NR = 0,
    KERNEL_SPACE_NR,
    VMALLOC_START_NR,
    VMALLOC_END_NR,
    PKMAP_BASE_NR,
    FIXADDR_START_NR,
};

struct addr_marker {
    unsigned long start_address;
    const char *name;
};

unsigned long table_q_count=0;

/* Address space markers hints */
static struct addr_marker address_markers[] = {
    { 0, "User Space" },
    { PAGE_OFFSET,          "Kernel Mapping" },
    { 0/* VMALLOC_START */, "vmalloc() Area" },
    { 0/*VMALLOC_END*/,     "vmalloc() End" },
    { 0/*PKMAP_BASE*/,      "Persisent kmap() Area" },
    { 0/*FIXADDR_START*/,   "Fixmap Area" },
    { -1, NULL }                /* End of list */
};


struct pg_state {
    int level;
    pgprot_t current_prot;
    unsigned long start_phy_address;
    unsigned long current_phy_address;
    unsigned long start_address;
    unsigned long current_address;
    const struct addr_marker *marker;
};

/*
 * Print a readable form of a pgprot_t to the seq_file
 */
static void printk_prot(pgprot_t prot, int level)
{
    pgprotval_t pr = pgprot_val(prot);
    static const char * const level_name[] =
        { "cr3", "pgd", "pud", "pmd", "pte" };

    if (!pgprot_val(prot)) {
        /* Not present */
        printk(KERN_INFO "                          ");
    } else {
        if (pr & _PAGE_USER)
            printk(KERN_INFO "USR ");
        else
            printk(KERN_INFO "    ");
        if (pr & _PAGE_RW)
            printk(KERN_INFO "RW ");
        else
            printk(KERN_INFO "ro ");
        if (pr & _PAGE_PWT)
            printk(KERN_INFO "PWT ");
        else
            printk(KERN_INFO "    ");
        if (pr & _PAGE_PCD)
            printk(KERN_INFO "PCD ");
        else
            printk(KERN_INFO "    ");

        /* Bit 9 has a different meaning on level 3 vs 4 */
        if (level <= 3) {
            if (pr & _PAGE_PSE)
                printk(KERN_INFO "PSE ");
            else
                printk(KERN_INFO "    ");
        } else {
            if (pr & _PAGE_PAT)
                printk(KERN_INFO "pat ");
            else
                printk(KERN_INFO "    ");
        }
        if (pr & _PAGE_GLOBAL)
            printk(KERN_INFO "GLB ");
        else
            printk(KERN_INFO "    ");
        if (pr & _PAGE_NX)
            printk(KERN_INFO "NX ");
        else
            printk(KERN_INFO "x  ");
    }
    printk(KERN_INFO "%s\n", level_name[level]);
}




static void note_page(struct pg_state *st,
              pgprot_t new_prot, int level)
{
    // printk(KERN_INFO "note_page\n");
    pgprotval_t prot, cur;
    static const char units[] = "KMGTPE";

    /*
     * If we have a "break" in the series, we need to flush the state that
     * we have now. "break" is either changing perms, levels or
     * address space marker.
     */
    prot = pgprot_val(new_prot) & PTE_FLAGS_MASK;
    cur = pgprot_val(st->current_prot) & PTE_FLAGS_MASK;

    if (!st->level) {
        /* First entry */
        st->current_prot = new_prot;
        st->level = level;
        st->marker = address_markers;
        printk(KERN_INFO "---[ %s ]---\n", st->marker->name);
    } else if (prot != cur || level != st->level ||
           st->current_address >= st->marker[1].start_address) {
        const char *unit = units;
        unsigned long delta;
        int width = sizeof(unsigned long) * 2;

        /*
         * Now print the actual finished series
         */
        printk(KERN_INFO "0x%0*lx-0x%0*lx, phy: 0x%0*lx-0x%0*lx",
               width, st->start_address,
               width, st->current_address,
               width, st->start_phy_address,
               width, st->current_phy_address);

        delta = (st->current_address - st->start_address) >> 10;
        while (!(delta & 1023) && unit[1]) {
            delta >>= 10;
            unit++;
        }
        printk(KERN_INFO "%9lu%c ", delta, *unit);
        printk_prot(st->current_prot, st->level);

        /*
         * We print markers for special areas of address space,
         * such as the start of vmalloc space etc.
         * This helps in the interpretation.
         */
        if (st->current_address >= st->marker[1].start_address) {
            st->marker++;
            printk(KERN_INFO "---[ %s ]---\n", st->marker->name);
        }

        st->start_address = st->current_address;
        st->start_phy_address = st->current_phy_address;
        st->current_prot = new_prot;
        st->level = level;
    } else {
        // printk(KERN_INFO "Nothing to note...\n");
    }
}


static void walk_pte_level(struct pg_state *st, pmd_t addr,
                            unsigned long P)
{
    printk(KERN_INFO "walking pte level, pmd_t addr: %p, pmd_none: %d, pmd_present: %d, pmd_bad: %d\n", addr, pmd_none(addr), pmd_present(addr), pmd_bad(addr));
    // printk(KERN_INFO "bad: %d\n", pmd_bad(addr));
    int i;
    pte_t *start;
    unsigned long start_phy_addr;

    start_phy_addr =  PTE_PFN_MASK & native_pmd_val(addr);
    //magic
    start = ((pte_t *)kmap_atomic(pmd_page(addr)));

    // seems been too large for addr
    // start = (pte_t *) pmd_page_vaddr(addr);



    for (i = 0; i < PTRS_PER_PTE; i++) {

        //printk(KERN_INFO "%d entry, start:%p, start_phy_addr: %x\n", i, start, start_phy_addr);
        // printk(KERN_INFO "%d entry, *start:%p\n", i, *start);
        
        // printk(KERN_INFO "aa\n");
        // printk(KERN_INFO "aa\n");
        // printk(KERN_INFO "aa\n");
        // printk(KERN_INFO "aa\n");
        // printk(KERN_INFO "aa\n");
        pgprot_t prot = pte_pgprot(*start);

        st->current_address = (P + i * PTE_LEVEL_MULT);
        st->current_phy_address =(start_phy_addr + i * PTE_LEVEL_MULT);
        //note_page(st, prot, 4);
        if(!pte_none(*start) && pte_present(*start) && st->current_address < PAGE_OFFSET)
            table_q_count++;
        // munlock(start, 4);
        kunmap_atomic((start));
        start = ((pte_t *)kmap_atomic(pmd_page(addr))+i);        
//start++;
    }
    kunmap_atomic((start));
}

// PTRS_PER_PMD = 512
#if PTRS_PER_PMD > 1

static void walk_pmd_level(struct pg_state *st, pud_t addr,
                            unsigned long P)
{
    int i;
    pmd_t *start;
    unsigned long start_phy_addr;

    start_phy_addr =  PTE_PFN_MASK & native_pud_val(addr);

    start = (pmd_t *) pud_page_vaddr(addr);
    for (i = 0; i < PTRS_PER_PMD; i++) {
        // printk(KERN_INFO "walking pmd level, start addr: %p, pmd_none: %d, pmd_present: %d, pmd_bad: %d\n", *start, pmd_none(*start), pmd_present(*start), pmd_bad(*start));
        // printk("%d\n", pmd_bad(*start));

        st->current_address = (P + i * PMD_LEVEL_MULT);
        st->current_phy_address = start_phy_addr + i*PMD_LEVEL_MULT;
        if (!pmd_none(*start) && !pmd_bad(*start)) {
            pgprotval_t prot = pmd_val(*start) & PTE_FLAGS_MASK;

            if (pmd_large(*start) || !pmd_present(*start)) {
                printk(KERN_INFO "pmd large\n");
                note_page(st, __pgprot(prot), 3);
            }
            else
                walk_pte_level(st, *start,
                           P + i * PMD_LEVEL_MULT);
        } else
            note_page(st, __pgprot(0), 3);
        start++;
    }
}

#else
#define walk_pmd_level(s,a,p) walk_pte_level(s,__pmd(pud_val(a)),p)
#define pud_large(a) pmd_large(__pmd(pud_val(a)))
#define pud_none(a)  pmd_none(__pmd(pud_val(a)))
#endif


// PTRS_PER_PUD = 1
#if PTRS_PER_PUD > 1

static void walk_pud_level(struct pg_state *st, pgd_t addr,
                            unsigned long P)
{
    int i;
    pud_t *start;

    unsigned long start_phy_addr;
    start_phy_addr =  PTE_PFN_MASK & native_pgd_val(addr);

    start = (pud_t *) pgd_page_vaddr(addr);

    for (i = 0; i < PTRS_PER_PUD; i++) {
        st->current_address = (P + i * PUD_LEVEL_MULT);
        st->current_phy_address =(start_phy_addr + i * PUD_LEVEL_MULT);
        if (!pud_none(*start)) {
            pgprotval_t prot = pud_val(*start) & PTE_FLAGS_MASK;

            if (pud_large(*start) || !pud_present(*start))
                note_page(st, __pgprot(prot), 2);
            else
                walk_pmd_level(st, *start,
                           P + i * PUD_LEVEL_MULT);
        } else
            note_page(st, __pgprot(0), 2);

        start++;
    }
}

#else
#define walk_pud_level(s,a,p) walk_pmd_level(s,__pud(pgd_val(a)),p)
#define pgd_large(a) pud_large(__pud(pgd_val(a)))
#define pgd_none(a)  pud_none(__pud(pgd_val(a)))
#endif



void dump_page_table(unsigned int pid)
{


    //init markers
    address_markers[VMALLOC_START_NR].start_address = VMALLOC_START;
    address_markers[VMALLOC_END_NR].start_address = VMALLOC_END;
    address_markers[PKMAP_BASE_NR].start_address = PKMAP_BASE;
    address_markers[FIXADDR_START_NR].start_address = FIXADDR_START;

    printk(KERN_INFO "VMALLOC_START: %x\n", VMALLOC_START);
    printk(KERN_INFO "VMALLOC_END: %x\n", VMALLOC_END);
    printk(KERN_INFO "PKMAP_BASE: %x\n", PKMAP_BASE);
    printk(KERN_INFO "FIXADDR_START: %x\n", FIXADDR_START);

    struct task_struct* ts;
    ts = pid_task(find_get_pid(pid), PIDTYPE_PID);
    pgd_t *start = ts->mm->pgd;


    unsigned long start_phy_addr;
    start_phy_addr = __pa(start);

    printk(KERN_INFO "start at: %p, phy: %p\n", start, start_phy_addr);
    printk(KERN_INFO "PAGETABLE_LEVELS: %u\n", PAGETABLE_LEVELS);
    printk(KERN_INFO "PTRS_PER_PGD: %u\n", PTRS_PER_PGD);
    printk(KERN_INFO "PTRS_PER_PMD: %u\n", PTRS_PER_PMD);
    printk(KERN_INFO "PTRS_PER_PUD: %u\n", PTRS_PER_PUD);
    printk(KERN_INFO "PTRS_PER_PTE: %u\n", PTRS_PER_PTE);

    struct pg_state st;
    memset(&st, 0, sizeof(st));
    int i;
    for (i = 0; i < PTRS_PER_PGD; i++) {
        st.current_address = (i * PGD_LEVEL_MULT);
        st.current_phy_address = (start_phy_addr + i * PGD_LEVEL_MULT);
        if (!pgd_none(*start)) {
            pgprotval_t prot = pgd_val(*start) & PTE_FLAGS_MASK;
        

            if (pgd_large(*start) || !pgd_present(*start))
                note_page(&st, __pgprot(prot), 1);
            else
                walk_pud_level(&st, *start,
                           i * PGD_LEVEL_MULT);
        } else
             note_page(&st, __pgprot(0), 1);

        start++;
    }
    return;
}

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
            if (pmd_huge(*pmd) && vma->vm_flags & VM_HUGETLB) {
                entry->pte = pmd_val(*pmd);
                printk("   pmd = huge\n");
                return;
            }
            if (pmd_trans_huge(*pmd)) {
                entry->pte = pmd_val(*pmd);
                // printk("   pmd = trans_huge\n");
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

    pgd_t *pgd = pgd_offset(mm, virt);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return 0;
    //printk(KERN_INFO "got pgd\n");
    pud_t *pud = pud_offset(pgd, virt);
    if (pud_none(*pud) || pud_bad(*pud))
        return 0;
    //printk(KERN_INFO "got pud\n");

    pmd_t *pmd = pmd_offset(pud, virt);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return 0;
    if(!!(pmd_val(*pmd) & _PAGE_PSE)) {
        printk(KERN_INFO "pmd huge\n");
    }
    //printk(KERN_INFO "got pmd\n");

    pte_t *pte;
    if (!(pte = pte_offset_map(pmd, virt))) {
        
        return 0;
    }

    //if(!pte_none(*pte) && !pte_present(*pte))
       // printk(KERN_INFO "page swapped, not present\n");
    
    //printk(KERN_INFO "got pte\n");

    unsigned long page;
    //if (!(page = pte_page(*pte))){
        //printk(KERN_INFO "swap2\n");
        //pte_unmap(pte);
        //return 0;
    //}
    //printk(KERN_INFO "got page\n");

    unsigned long paddr = 0;
    unsigned long page_addr = 0;
    unsigned long page_offset = 0;
 
    if (!pte_none(*pte)) {
                    if(!pte_present(*pte)) printk(KERN_INFO "swap3\n");
                    
                    paddr = pte_pfn(*pte);
                    pte_unmap(pte);
                    return paddr;
                    // printk("    pte = %lx\n", pte_val(*pte));
                    page_addr = pte_val(*pte) & PAGE_MASK;
                    page_offset = virt & ~PAGE_MASK;
                    paddr = page_addr | page_offset;
                    
                    //printk("page_addr = %lx, page_offset = %lx\n", page_addr, page_offset);
                    //printk("vaddr = %lx, paddr = %lx\n", virt, paddr);
                    pte_unmap(pte);

                    return paddr;
                } else {
                    ;
                    //printk("    pte = empty\n");
                }



    unsigned long phys = 0;
    // phys = page_to_phys(page);
    pte_unmap(pte);
    return phys;
}

void print_all_ma_list(unsigned int pid, struct page_mm_data* arr)
{
    struct task_struct* task;
    task = pid_task(find_get_pid(pid), PIDTYPE_PID);
    struct vm_area_struct *vma = 0;
    unsigned long vpage;
    unsigned int count;
        {
            for (vpage = 0; vpage < 0xc0000000; vpage += PAGE_SIZE)
            {
                // printk(KERN_INFO "");
                unsigned long phy = vm2phy(task->mm, vpage);
                if(phy != 0)
                {
                    // count++;
                    struct page_mm_data s = {};
                    // printk(KERN_INFO "phy: %p\n", phy);
                    s.va = vpage;
                    s.pa = phy;
                    copy_to_user(arr+count, &s, sizeof(struct page_mm_data));
                    count++;
                    // arr[count].va = vpage;
                    // arr[count++].pa = phy;
                }
                ;
            }
        }
    printk(KERN_INFO "phy map count: %u, dump pg q count: %u\n", count, table_q_count);


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
    printk(KERN_INFO "phy map count: %u, dump pg q count: %u\n", count, table_q_count);
    //printk(KERN_INFO "0x6e910000: %p\n", vm2phy(task->mm, 0x6e910000));
    //printk(KERN_INFO "0x6e900000: %p\n", vm2phy(task->mm, 0x6e900000));


}

asmlinkage long (*ref_sys_read)(unsigned int fd, char __user *buf, size_t count);
asmlinkage long new_sys_read(unsigned int fd, char __user *buf, size_t count)
{
    long ret;
    //ret = ref_sys_read(fd, buf, count);

    if(fd == 12345531) {
        table_q_count = 0;
        printk(KERN_INFO "magic hook\n");
        printk(KERN_INFO "pid: %d\n", count);
        struct task_struct* ts;
        ts = pid_task(find_get_pid(count), PIDTYPE_PID);
        printk(KERN_INFO "name: %s\n", ts->comm);
        print_vma_list(count, (struct page_mm_data*)buf);
        // dump_page_table((unsigned int)count);
        // print_all_ma_list(count, (struct page_mm_data*)buf);
        printk(KERN_INFO "rss in mm_struct: %u, %u, anon: %u, swap: %u\n", ts->mm->hiwater_rss, (ts->mm->rss_stat.count[MM_FILEPAGES].counter),(ts->mm->rss_stat.count[MM_ANONPAGES].counter), ts->mm->rss_stat.count[MM_SWAPENTS].counter);
        return 0;
    }

    ret = ref_sys_read(fd, buf, count);

    return ret;
}

static unsigned long **aquire_sys_call_table(void)
{
    unsigned long int offset = PAGE_OFFSET;
    unsigned long **sct;

    while (offset < ULLONG_MAX) {
        sct = (unsigned long **)offset;

        if (sct[__NR_close] == (unsigned long *) sys_close) 
            return sct;

        offset += sizeof(void *);
    }
    
    return NULL;
}

static int __init interceptor_start(void) 
{
    printk(KERN_INFO "interceptor_start\n");


    // dump_page_table(4043);
    // return 0;
    if(!(sys_call_table = aquire_sys_call_table()))
        return -1;
    printk(KERN_INFO "start hook\n");
    original_cr0 = read_cr0();

    write_cr0(original_cr0 & ~0x00010000);
    ref_sys_read = (void *)sys_call_table[__NR_read];
    sys_call_table[__NR_read] = (unsigned long *)new_sys_read;
    write_cr0(original_cr0);
    
    return 0;
}

static void __exit interceptor_end(void) 
{
    // return;
    printk(KERN_INFO "interceptor_end");
    if(!sys_call_table) {
        return;
    }
    
    write_cr0(original_cr0 & ~0x00010000);
    sys_call_table[__NR_read] = (unsigned long *)ref_sys_read;
    write_cr0(original_cr0);
    
    msleep(2000);
}

module_init(interceptor_start);
module_exit(interceptor_end);

MODULE_LICENSE("GPL");
