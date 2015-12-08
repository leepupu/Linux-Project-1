#include <stdio.h>
#include <stdlib.h>
#include "../mymm.h"

#include <syscall.h>
#include <unistd.h>
#include <limits.h> // PATH_MAX
#include <sys/types.h>

#define REPEAT_TIME 10

struct vma_t {
    unsigned long s;
    unsigned long e;
    unsigned long size;
    char filename[PATH_MAX+1];
    struct vma_t* nxt;
};

struct vma_t* head = 0;

void load_maps(unsigned int pid, struct vma_t **head) {
    char path_buf[64];
    sprintf(path_buf, "/proc/%u/maps", pid);
    // printf("path: %s\n", path_buf);
    FILE* fp = fopen(path_buf, "r");
    unsigned s, e;
    char line[PATH_MAX+256], perm[5], dev[6], mappath[PATH_MAX];
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
        cur_vma = (struct vma_t*)malloc(sizeof(struct vma_t));
        // printf("%p-%p: %s\n", s, e, mappath);
        cur_vma->s = s;
        cur_vma->e = e;
        strcpy(cur_vma->filename, mappath);
        cur_vma->size = 0;
        cur_vma->nxt = 0;

        if(*head == 0)
            *head = cur_vma;
        else
            pre_vma->nxt = cur_vma;
        pre_vma = cur_vma;
    }
}

void count_size(struct vma_t *head, struct page_mm_data *page_data) {
    struct vma_t *cur_vma = head;
    unsigned int counter = 0;
    for(;cur_vma;cur_vma = cur_vma->nxt) {
        if(page_data[counter].va == 0 && page_data[counter].pa == 0)
            break;
        while(page_data[counter].va && page_data[counter].pa && page_data[counter].va < cur_vma->e) {
            cur_vma->size++;
            counter++;
        }
    }
}

void print_data(struct vma_t *head) {
    struct vma_t *cur_vma = head;
    
    for(;cur_vma;cur_vma = cur_vma->nxt) {
        float varange = (cur_vma->e-cur_vma->s);
        float pasize = (cur_vma->size*4096.0);
        float percentage = (pasize/varange);
        printf("%p-%p: %ukb, %.3f %%   mappath: %s\n", cur_vma->s, cur_vma->e, cur_vma->size*4, 100.0*percentage, cur_vma->filename);
        if(percentage > 1.0) {
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
    syscall(360,pid,(void*)arr);
}
void raw_dump(struct page_mm_data* arr) {

int i=0;
for(i=0;i<MY_MM_DATA_MAX;i++)
    if(arr[i].va==0 && arr[i].pa ==0) break;
    printf("%p: %p\n", arr[i].va, arr[i].pa);
}

void project1(int pid) {
    int i;
    struct page_mm_data* arr = (struct page_mm_data*)malloc(MY_MM_DATA_MAX*sizeof(struct page_mm_data));
    for(i=0;i<REPEAT_TIME;i++) {
        printf("No. %u times of linux_survey_TT call\n", i+1);
        memset(arr, 0, MY_MM_DATA_MAX*sizeof(struct page_mm_data));
        linux_survey_TT(pid,(void*)arr);
        //read(12345531, (char*)arr, pid);
        raw_dump(arr);
        load_maps(pid, &head);
        count_size(head, arr);
        print_data(head);
        free_list(head);
        head = 0;
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
