#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include "tinyalloc.h"
//my define
#define DEBUG 1
#define max_count 16
//tinyalloc define
#ifndef TA_ALIGN
#define TA_ALIGN 8
#endif

#ifndef TA_BASE
#define TA_BASE 0x1337000
#endif

#ifndef TA_HEAP_START
#define TA_HEAP_START (TA_BASE + sizeof(Heap))
#endif

#ifndef TA_HEAP_LIMIT
#define TA_HEAP_LIMIT (1 << 24)
#endif

#ifndef TA_HEAP_BLOCKS
#define TA_HEAP_BLOCKS 256
#endif

#ifndef TA_SPLIT_THRESH
#define TA_SPLIT_THRESH 16
#endif

#ifdef TA_DEBUG
extern void print_s(char *);
extern void print_i(size_t);
#else
#define print_s(X)
#define print_i(X)
#endif

typedef struct Block Block;

struct Block {
    void *addr;
    Block *next;
    size_t size;
};

typedef struct {
    Block *free;   // first free block
    Block *used;   // first used block
    Block *fresh;  // first available blank block
    size_t top;    // top free addr
    Block blocks[TA_HEAP_BLOCKS];
} Heap;

static Heap *heap = (Heap *)TA_BASE;

/**
 * If compaction is enabled, inserts block
 * into free list, sorted by addr.
 * If disabled, add block has new head of
 * the free list.
 */
static void insert_block(Block *block) {
#ifndef TA_DISABLE_COMPACT
    Block *ptr  = heap->free;
    Block *prev = NULL;
    while (ptr != NULL) {
        if ((size_t)block->addr <= (size_t)ptr->addr) {
            print_s("insert");
            print_i((size_t)ptr);
            break;
        }
        prev = ptr;
        ptr  = ptr->next;
    }
    if (prev != NULL) {
        if (ptr == NULL) {
            print_s("new tail");
        }
        prev->next = block;
    } else {
        print_s("new head");
        heap->free = block;
    }
    block->next = ptr;
#else
    block->next = heap->free;
    heap->free  = block;
#endif
}

#ifndef TA_DISABLE_COMPACT
static void release_blocks(Block *scan, Block *to) {
    Block *scan_next;
    while (scan != to) {
        print_s("release");
        print_i((size_t)scan);
        scan_next   = scan->next;
        scan->next  = heap->fresh;
        heap->fresh = scan;
        scan->addr  = 0;
        scan->size  = 0;
        scan        = scan_next;
    }
}

static void compact() {
    Block *ptr = heap->free;
    Block *prev;
    Block *scan;
    while (ptr != NULL) {
        prev = ptr;
        scan = ptr->next;
        while (scan != NULL &&
               (size_t)prev->addr + prev->size == (size_t)scan->addr) {
            print_s("merge");
            print_i((size_t)scan);
            prev = scan;
            scan = scan->next;
        }
        if (prev != ptr) {
            size_t new_size =
                (size_t)prev->addr - (size_t)ptr->addr + prev->size;
            print_s("new size");
            print_i(new_size);
            ptr->size   = new_size;
            Block *next = prev->next;
            // make merged blocks available
            release_blocks(ptr->next, prev->next);
            // relink
            ptr->next = next;
        }
        ptr = ptr->next;
    }
}
#endif

bool ta_init() {
    heap->free   = NULL;
    heap->used   = NULL;
    heap->fresh  = heap->blocks;
    heap->top    = TA_HEAP_START;
    Block *block = heap->blocks;
    size_t i     = TA_HEAP_BLOCKS - 1;
    while (i--) {
        block->next = block + 1;
        block++;
    }
    block->next = NULL;
    return true;
}

bool ta_free(void *free) {
    Block *block = heap->used;
    Block *prev  = NULL;
    while (block != NULL) {
        if (free == block->addr) {
            if (prev) {
                prev->next = block->next;
            } else {
                heap->used = block->next;
            }
            insert_block(block);
#ifndef TA_DISABLE_COMPACT
            compact();
#endif
            return true;
        }
        prev  = block;
        block = block->next;
    }
    return false;
}

static Block *alloc_block(size_t num) {
    Block *ptr  = heap->free;
    Block *prev = NULL;
    size_t top  = heap->top;
    num         = (num + TA_ALIGN - 1) & -TA_ALIGN;
    while (ptr != NULL) {
        const int is_top = ((size_t)ptr->addr + ptr->size >= top) && ((size_t)ptr->addr + num <= TA_HEAP_LIMIT);
        if (is_top || ptr->size >= num) {
            if (prev != NULL) {
                prev->next = ptr->next;
            } else {
                heap->free = ptr->next;
            }
            ptr->next  = heap->used;
            heap->used = ptr;
            if (is_top) {
                print_s("resize top block");
                ptr->size = num;
                heap->top = (size_t)ptr->addr + num;
#ifndef TA_DISABLE_SPLIT
            } else if (heap->fresh != NULL) {
                size_t excess = ptr->size - num;
                if (excess >= TA_SPLIT_THRESH) {
                    ptr->size    = num;
                    Block *split = heap->fresh;
                    heap->fresh  = split->next;
                    split->addr  = (void *)((size_t)ptr->addr + num);
                    print_s("split");
                    print_i((size_t)split->addr);
                    split->size = excess;
                    insert_block(split);
#ifndef TA_DISABLE_COMPACT
                    compact();
#endif
                }
#endif
            }
            return ptr;
        }
        prev = ptr;
        ptr  = ptr->next;
    }
    // no matching free blocks
    // see if any other blocks available
    size_t new_top = top + num;
    if (heap->fresh != NULL && new_top <= TA_HEAP_LIMIT) {
        ptr         = heap->fresh;
        heap->fresh = ptr->next;
        ptr->addr   = (void *)top;
        ptr->next   = heap->used;
        ptr->size   = num;
        heap->used  = ptr;
        heap->top   = new_top;
        return ptr;
    }
    return NULL;
}

void *ta_alloc(size_t num) {
    Block *block = alloc_block(num);
    if (block != NULL) {
        return block->addr;
    }
    return NULL;
}

static void memclear(void *ptr, size_t num) {
    size_t *ptrw = (size_t *)ptr;
    size_t numw  = (num & -sizeof(size_t)) / sizeof(size_t);
    while (numw--) {
        *ptrw++ = 0;
    }
    num &= (sizeof(size_t) - 1);
    uint8_t *ptrb = (uint8_t *)ptrw;
    while (num--) {
        *ptrb++ = 0;
    }
}

void *ta_calloc(size_t num, size_t size) {
    num *= size;
    Block *block = alloc_block(num);
    if (block != NULL) {
        memclear(block->addr, num);
        return block->addr;
    }
    return NULL;
}

static size_t count_blocks(Block *ptr) {
    size_t num = 0;
    while (ptr != NULL) {
        num++;
        ptr = ptr->next;
    }
    return num;
}

size_t ta_num_free() {
    return count_blocks(heap->free);
}

size_t ta_num_used() {
    return count_blocks(heap->used);
}

size_t ta_num_fresh() {
    return count_blocks(heap->fresh);
}

bool ta_check() {
    return TA_HEAP_BLOCKS == ta_num_free() + ta_num_used() + ta_num_fresh();
}

char* mmaped_area;
char* addrs[max_count];

char* init(){
    char *mmaped = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    //init TA_BASE
    mmap((void *)0x1337000, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if((__int64_t)mmaped == -1){
        perror("mmap error");
        _exit(0);
    } 
    return mmaped;
}

int create(){
    //create a chunk
    int size;
#ifdef DEBUG
    printf("plz input the size:\n");
    read(STDIN_FILENO, mmaped_area, 0x100);
#endif
    size = atoi(mmaped_area);
    char *chunk = ta_alloc(size);
    for(int i = 0; i < max_count; ++i){
        if(addrs[i] == NULL){
            addrs[i] = chunk;
        }
    }
    return 0;
}

int delete(){
    //delete a chunk
    int idx;
#ifdef DEBUG
    read(STDIN_FILENO, mmaped_area, 0x100);
#endif
    idx = atoi(mmaped_area);
    ta_free(addrs[idx]);
    addrs[idx] = NULL; 
    return 0;
}

int edit(){
    //edit the content of the chunk
    int idx;
#ifdef DEBUG
    read(STDIN_FILENO, mmaped_area, 0x100);
#endif
    idx = atoi(mmaped_area);
    if(DEBUG){
        read(STDIN_FILENO, addrs[idx], 0x100);
    }
    return 0;
}

int main(){
    int choice;
    mmaped_area = init();
    ta_init();
#ifdef DEBUG
    printf("mmapped area : %p\n", mmaped_area);
#endif
    while(1){
#ifdef DEBUG
        //read from stdin for testing
        read(STDIN_FILENO, mmaped_area, 0x1000);
#endif
        sscanf(mmaped_area, "%d", &choice);
        switch(choice){
            case 1:
                create();
            case 2:
                delete();
            case 3:
                edit();
            default:
                break;
        }
    }
    return 0;
}