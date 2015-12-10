#include <stdio.h>
#include <stdlib.h>
#include "../mymm.h"

#include <syscall.h>
#include <unistd.h>
#include <limits.h> // PATH_MAX
#include <sys/types.h>

#define REPEAT_TIME 1

struct pa_interval {
    unsigned long s;
    unsigned long e;
    struct pa_interval* nxt;
    unsigned long huge_size;
};
struct vma_t {
    unsigned long s;
    unsigned long e;
    unsigned long size;
    unsigned long rss;
    unsigned long rss_huge;
    char filename[PATH_MAX+1];
    struct vma_t* nxt;
    struct pa_interval* pais;
};


struct vma_t* head = 0;
unsigned int tmp_pfn[MY_MM_DATA_MAX];

void load_maps(unsigned int pid, struct vma_t **head) {
    char path_buf[64];
    sprintf(path_buf, "/proc/%u/smaps", pid);
    // printf("path: %s\n", path_buf);
    FILE* fp = fopen(path_buf, "r");
    unsigned s, e;
    char line[PATH_MAX+256], perm[5], dev[6], mappath[PATH_MAX], labelbuf[128];
    struct vma_t *cur_vma, *pre_vma;
    unsigned long size, inode, foo;
    while(fgets(line, 256, fp)) {
        int n = sscanf(line, "%lX-%lX %4s %lx %5s %ld %s", &s, &e, perm, &foo, dev, &inode, mappath);
        if(n!=7) {
            // printf("Invalid line %u %s %d\n", n, mappath, mappath[0]);
            if(n == 6)
                mappath[0] = 0;
            else
                continue;
        }
        unsigned long rss=0, rss_huge=0;
        fgets(line, 256, fp);//Size:
        fgets(line, 256, fp);//RSS:
        sscanf(line, "%s %u", labelbuf, &rss);
        
        fgets(line, 256, fp);//PSS:
        fgets(line, 256, fp);//SharedClean
        fgets(line, 256, fp);//SharedDirty
        fgets(line, 256, fp);//PrivateClean
        fgets(line, 256, fp);//PrivateDirty
        fgets(line, 256, fp);//Referenced
        fgets(line, 256, fp);//Anon
        fgets(line, 256, fp);//AnonHugePage
        sscanf(line, "%s %u", labelbuf, &rss_huge);
        fgets(line, 256, fp);//Swap
        fgets(line, 256, fp);//KernelPageSize
        fgets(line, 256, fp);//MMUPAGESIZE
        fgets(line, 256, fp);//Locked
        fgets(line, 256, fp);//VMFlags


        

        cur_vma = (struct vma_t*)malloc(sizeof(struct vma_t));
        // printf("%p-%p: %s\n", s, e, mappath);
        cur_vma->s = s;
        cur_vma->e = e;
        strcpy(cur_vma->filename, mappath);
        cur_vma->size = 0;
        cur_vma->nxt = 0;
        cur_vma->pais = 0;
        cur_vma->rss = rss;
        cur_vma->rss_huge = rss_huge;
        if(*head == 0)
            *head = cur_vma;
        else
            pre_vma->nxt = cur_vma;
        pre_vma = cur_vma;
    }
}

int cmp(const void* a, const void* b) {
    return ( *(int*)a > *(int*)b );
}

void count_size(struct vma_t *head, struct page_mm_data *page_data) {
    struct vma_t *cur_vma = head;
    unsigned int counter = 0;
    for(;cur_vma;cur_vma = cur_vma->nxt) {
        if(page_data[counter].va == 0 && page_data[counter].pa == 0)
            break;
        memset(tmp_pfn, 0, sizeof(tmp_pfn));
        unsigned int pfn_counter = 0;
        while(page_data[counter].va && page_data[counter].pa && page_data[counter].va < cur_vma->e && page_data[counter].va >= cur_vma->s ) {
            tmp_pfn[pfn_counter++] = page_data[counter].pa;
            cur_vma->size++;
            counter++;
        }
        qsort(tmp_pfn, pfn_counter, sizeof(unsigned int), cmp);

        int i;

        int mm = 0;
        int huge_size = 0;
        // printf("fucking here: %p, %p\n", tmp_pfn[1] , tmp_pfn[2]);
        /*for(i=0;i<pfn_counter;i++) {
            if(i>0 && tmp_pfn[i] == tmp_pfn[i-1]) {
                huge_size++;
                if(mm==0)
                    printf(" huge and pass...\n");
                mm = 1;
                continue;
            }
            if(mm == 1)
                printf("huge_size: %u\n", huge_size+1);
            printf("%p ", tmp_pfn[i]);
            huge_size = 0;
            mm=0;
        }
        printf("\n");

        printf("vma size is: %u, pfn_counter: %u\n", cur_vma->size, pfn_counter);*/


        if(cur_vma->size != pfn_counter) {
            printf("[error] size mismatch\n");
            exit(-1);
        }
        if(pfn_counter > 0) {
            struct pa_interval *pai = (struct pa_interval*)malloc(sizeof(struct pa_interval));
            unsigned int page_size = 1; // pfn 1 means 1 page
            cur_vma->pais = pai;
            pai->s = tmp_pfn[0];
            pai->e = tmp_pfn[0] + page_size;
            pai->nxt = 0;
            pai->huge_size = 1;
            unsigned int is_huge = 0;
            unsigned int huge_size = 0;

            for(i=1;i<pfn_counter;i++) {

                if(tmp_pfn[i] - tmp_pfn[i-1] == page_size) {
                    pai->e = tmp_pfn[i]+page_size;
                } else if(tmp_pfn[i] == tmp_pfn[i-1]) {
                    huge_size++;
                    pai->huge_size ++;

                    pai->e = tmp_pfn[i]+page_size*(huge_size+1);
                    is_huge = 1;
                } else { // new interval
                    // printf("%x\n", tmp_pfn[i] - tmp_pfn[i-1]);
                    if(tmp_pfn[i] - tmp_pfn[i-1] == page_size*(huge_size+1) && is_huge) {
                        pai->e = tmp_pfn[i]+page_size;
                        pai->huge_size ++;
                        is_huge = 0;
                        huge_size = 0;
                        continue;
                    }
                    if(huge_size+1 != 1 && huge_size+1 != 512) {
                        printf("strange huge_size: %p\n", huge_size+1);
                        // exit(0);
                    }
                    struct pa_interval *nxt = (struct pa_interval*)malloc(sizeof(struct pa_interval));
                    nxt->nxt = 0;
                    huge_size = 0;
                    nxt->s = tmp_pfn[i];
                    nxt->e = tmp_pfn[i]+page_size;
                    nxt->huge_size = 1;
                    pai->nxt = nxt;
                    pai = nxt;
                    is_huge = 0;
                }
            }
        }
    }
}

void print_data(struct vma_t *head) {
    struct vma_t *cur_vma = head;
    
    for(;cur_vma;cur_vma = cur_vma->nxt) {
        float varange = (cur_vma->e-cur_vma->s);
        float pasize = (cur_vma->size*4096.0);
        float percentage = (pasize/varange);
        printf("%p-%p: %ukb(rss: %u, huge: %u), %.3f %%   mappath: %s\n", cur_vma->s, cur_vma->e, cur_vma->size*4, cur_vma->rss, cur_vma->rss_huge, 100.0*percentage, cur_vma->filename);
        if(cur_vma->size*4 != cur_vma->rss) {
            printf("rss mismatch\n");
        }
        struct pa_interval *curpai = cur_vma->pais;
        unsigned int s = 0;
        while(curpai) {
            printf("\tpa interval: %p-%p (%p, huge_size: %u)\n", curpai->s, curpai->e, curpai->e - curpai->s, curpai->huge_size);
            s += curpai->e - curpai->s;

            curpai = curpai->nxt;
        }
        s*=4;

        printf("\t\t sum: %u kb\n", s);
        if(s != cur_vma->size*4) {
            printf("err\n");
            exit(-1);
        }

        if(percentage > 1.0) {
            printf("percentage error\n");
            exit(-1);
        }
    }
}

void free_list(struct vma_t* h) {
    if(h->nxt == 0) {
        free(h);
        return;
    }
    free_list(h->nxt);
    free(h);
}

void linux_survey_TT(int pid, char * arr) {
    read(12345531, (char*)arr, pid);
    // syscall(359,pid,(void*)arr);
}

void project1(int pid) {
    int i;
    struct page_mm_data* arr = (struct page_mm_data*)malloc(MY_MM_DATA_MAX*sizeof(struct page_mm_data));
    for(i=0;i<REPEAT_TIME;i++) {
        printf("No. %u times of linux_survey_TT call\n", i+1);
        memset(arr, 0, MY_MM_DATA_MAX*sizeof(struct page_mm_data));
        linux_survey_TT(pid,(void*)arr);
        load_maps(pid, &head);
        count_size(head, arr);
        print_data(head);
        free_list(head);
        head = 0;
        if(i+1 < REPEAT_TIME)
            sleep(120);
    }

}

int main(int argc, char** argv) {
    if(argc < 2) return -1;
    
    char buf[5000];
    struct page_mm_data* arr = (struct page_mm_data*)malloc(MY_MM_DATA_MAX*sizeof(struct page_mm_data));
    memset(arr, 0, MY_MM_DATA_MAX*sizeof(struct page_mm_data));
    int pid = atoi(argv[1]);
    printf("try pid: %d\n", pid);
    

    //read(12345531, (char*)arr, pid);
    // linux_survey_TT(pid,(void*)arr);
    project1(pid);

    // huge page finder 

    int j=0;
    int i = 0;
    /*
    for(i=0;i<MY_MM_DATA_MAX;i++){
        for(j=i+1;j<MY_MM_DATA_MAX;j++){
            if(arr[j].va == 0 && arr[j].pa == 0)
                break;
            if(arr[i].pa == arr[j].pa)
            {
                printf("va: %p, %p, pa: %p have same page, %u, %u\n", arr[i].va, arr[j].va, arr[i].pa, i, j);
            }
        }
    }*/


    return 0;
}
