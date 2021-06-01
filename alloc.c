#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <setjmp.h>
#include "alloc.h"

typedef struct header {
    size_t flags;
    size_t size;
    struct header *next_free;
} Header;

typedef struct gc_heap {
    Header *slot;
    size_t size;
} GC_Heap;

struct root_range {
    void * start;
    void * end;
};

#define HEADER_SIZE ((size_t) sizeof(Header))
#define ALIGN(x,a) (((x) + (a - 1)) & ~(a - 1))
#define NEXT_HEADER(x) ((Header *)((size_t)(x+1) + x->size))
#define PTRSIZE ((size_t) sizeof(void *))
#define WSIZE 4
#define DSIZE 8
#define HEAP_LIMIT 10000
#define TINY_HEAP_SIZE 0x4000
#define ROOT_RANGES_LIMIT 1000

// flags的操作
#define FL_ALLOC 0x1
#define FL_MARK 0x2
#define FL_SET(x,f) (((Header*)x)->flags |= f)
#define FL_UNSET(x,f) (((Header*)x)->flags &= ~(f))
#define FL_TEST(x,f) (((Header*)x)->flags & f)

static struct root_range root_ranges[ROOT_RANGES_LIMIT];
static size_t root_ranges_used = 0;
static void *stack_start = NULL;
static void *stack_end = NULL;
static GC_Heap *hit_cache = NULL; // 这里设置了hit_cache，主要是为了增加搜索速度
static Header *free_list = NULL;
static GC_Heap gc_heaps[HEAP_LIMIT];
static size_t gc_heaps_used = 0;

Header* add_heap(size_t size)
{
    void* p;
    Header* align_p;

    // 判断gc已使用的堆块数量是否超出边界
    if (gc_heaps_used >= HEAP_LIMIT) {
        perror("OutOfMemory Error");
        abort();
    }

    if (size < TINY_HEAP_SIZE)
        size = TINY_HEAP_SIZE;
    
    //if ((p = sbrk(size)) == (void*)-1) 
    /*
    *   为什么不按上面的进行sbrk呢，因为按GC的理论，可能存在不止一块sbrk出来的内存作为空闲堆块。也就是说如果申请的size超过我们sbrk出来的堆块，我们需要再次或多次进行sbrk。
    *   如果我们按照上述的进行分配，在我们的代码里会造成一个问题。假设我们第一次sbrk出来size为0x4000，范围在0x0x555555584000-0x555555588000，第二次sbrk时会从0x555555588000开始直接分配
    *   这就导致了我们两次sbrk出来的两块空闲堆块会完全合并，第一，这不符合GC书中对于空闲堆块的描述，书中是以块作为最大计量单位的，我们这样设计后会导致最终无论多少次sbrk都只有一块堆块；
    *   第二，两块堆块合并后，在我们的代码设计中（mini_gc_malloc）我们对于第二块堆块是完全做不到任何管理的，也就是说第二块堆块甚至更多的堆块的Header我们是无法操作的，我们只能操作合并后的第一块也是唯一的
    *   一块堆块的Header，不仅造成浪费，还会使后续的GC出现严重的索引问题，导致不能正确完成GC（主要是在gc_sweep的代码设计上，利用了gc_heaps[HEAP_LIMIT]，具体可以详细看该函数的代码设计）
    */
    if((p = sbrk(size + PTRSIZE + HEADER_SIZE)) == (void *)-1)
    	return NULL;

    // 进行地址对齐
    align_p = gc_heaps[gc_heaps_used].slot = (Header *)ALIGN((size_t)p, PTRSIZE);
    gc_heaps[gc_heaps_used].size = size;
    align_p->next_free = align_p;
    align_p->size = size;
    gc_heaps_used++;

    return align_p;
}

Header *grow(size_t size)
{
    Header *cp, *up;

    if (!(cp = add_heap(size)))
        return NULL;

    up = (Header*)cp;
    mini_gc_free((void*)(up+1)); // 这里应该是为了把它与free_list进行链接，不然直接返回的话就不在free_list可控范围内了？
    return free_list;
}

void* mini_gc_malloc(size_t size)
{
    size_t asize;           //调整堆块大小，为头和尾留空间
    Header *p, *prevp;
    size_t do_gc = 0;

    if (size == 0)
        return NULL;
    asize = ALIGN(size, PTRSIZE);

    // free_list还未初始化
    if ((prevp = free_list) == NULL) {
        if (!(p = add_heap(TINY_HEAP_SIZE))) {
            return NULL;
        }
        prevp = free_list = p;
    }
    for (p = prevp->next_free; ; prevp = p, p = p->next_free){
        if (p->size >= (asize + HEADER_SIZE)) {
            if (p->size == (asize + HEADER_SIZE)) // 如果要malloc的size大小恰好符合搜索到的空闲堆块
                prevp->next_free = p->next_free; // 将这个空闲堆块从单链表中解链，使前一块堆块的next_free指向该堆块的next_free
            else {
                // 搜索到的空闲堆块size大于要malloc的size大小，就从该空闲堆块中分割出size大小的堆块
                p->size -= (asize + HEADER_SIZE);
                p = NEXT_HEADER(p); // 这里是为了不再动p的size，所以选择从这块堆块的尾部进行切割
                p->size = asize;
            }
            free_list = prevp;
            FL_SET(p,FL_ALLOC);
            return (void*)(p+1); // 因为p为struct Header，所以指针向后移动0x18位
        }
        if (p == free_list) {// 因为这是一个循环单链表,所以如果p == free_list就代表着一次循环遍历已经结束，该gc了
            if (!do_gc){
                garbage_collect();
                do_gc = 1;
            }else if ((p = grow(asize + 2*HEADER_SIZE)) == NULL) // 到这里已经把单链表循环完了，也没找到空闲堆块size大于要请求size的情况，这时候就向内核申请所需要大小的堆块
                return NULL;
        }
    }
}

void mini_gc_free(void *ptr)
{
    Header *target, *hit;

    target = (Header *)ptr - 1; // 找到heap的header

    // 找到ptr在free_list中属于那块堆块
    for (hit = free_list; !(target > hit && target < hit->next_free); hit = hit->next_free)
        // 如果遍历完了free_list还是没找到对应的堆块
        if (hit >= hit->next_free && (target > hit || target < hit->next_free))
            break;

    // target的下一个临近的堆块恰好对应free_list->next_free，对其进行合并，但还未链接到链表上
    if (NEXT_HEADER(target) == hit->next_free) { // 如果已经分配出去的target的下一个堆块地址刚好对应free_list的next_free，就将target与free_list->next_free合并
        // 合并
        target->size += (hit->next_free->size + HEADER_SIZE);
        target->next_free = hit->next_free->next_free;
    }else{ // 否则的话就直接插入（这里表现的是target的末尾不对应free_list的next_free，所以就直接链表插入）
        target->next_free = hit->next_free; 
    }
    // 如果hit的下一个临近的堆块就是target，直接进行合并并链接
    if (NEXT_HEADER(hit) == target) {
        hit->size += (target->size + HEADER_SIZE);
        hit->next_free = target->next_free;
    }else{
        hit->next_free = target;
    }
    free_list = hit;
    target->flags = 0;
}

void mini_gc_malloc_and_free()
{
    void* p1, *p2, *p3;
    // malloc check
    p1 = (void*)mini_gc_malloc(0x17);
    p2 = (void*)mini_gc_malloc(0x19);
    p3 = (void*)mini_gc_malloc(0x23);

    // free check
    mini_gc_free(p1);
    mini_gc_free(p2);
    mini_gc_free(p3);

    // grow check
    p1 = mini_gc_malloc(TINY_HEAP_SIZE+0x80);
    mini_gc_free(p1);
}

// 主要是搜索栈中是否有留存的堆地址
GC_Heap *is_pointer_to_heap(void *ptr)
{
    size_t i;

    // 如果hit_cache里有内容，且ptr在hit_cache所储存的那块堆块中
    if (hit_cache && ((void*)hit_cache->slot) <= ptr && ((char*)hit_cache->slot + hit_cache->size) >= ptr ) {
        return hit_cache;
    }

    for (i = 0; i < gc_heaps_used; i++) { // 遍历你向内核申请的所有堆块
        if ((void*)gc_heaps[i].slot <= ptr && ((char*)gc_heaps[i].slot + gc_heaps[i].size) >= ptr ) {
            hit_cache = &gc_heaps[i];
            return &gc_heaps[i];
        }
    }
    return NULL;
}

// 主要是为了在已获取的那块堆块(block)中找到ptr所在的堆块(heap)
Header *get_header(GC_Heap *gh, void *ptr)
{
    Header *p, *block_end, *pnext;

    block_end = (Header*)((char*)gh->slot + gh->size);
    for (p = gh->slot; p < block_end; p = pnext) {
        pnext = NEXT_HEADER(p);
        if ((void*)(p+1) <= ptr && ptr < (void*)pnext) {
            return p;
        }
    }
    return NULL;
}

void gc_mark_range(void *start, void *end)
{
    void *p;

    for (p = start; p < end; (size_t*)p++) {
        gc_mark((void **)p);
    }
}

void gc_mark(void* ptr)
{
    GC_Heap *gh;
    Header *hdr;

    // mark check
    if (!(gh = is_pointer_to_heap(ptr))) return;
    if (!(hdr = get_header(gh,ptr))) return;
    if (!FL_TEST(hdr, FL_ALLOC)) return; // 确保其是正在使用的堆块
    if (FL_TEST(hdr, FL_MARK)) return; // 确保其未被标记

    // marking
    FL_SET(hdr, FL_MARK);
    printf("mark ptr : %p, header : %p\n", ptr, hdr);

    // mark children
    gc_mark_range((void*)(hdr+1), (void*)NEXT_HEADER(hdr)); // 递归遍历
}

void gc_mark_register()
{
    jmp_buf env;
    size_t i;

    /*
     * 把当前栈的上下文存储到env里
     * 保存env的时候成功返回0
     * 以后可以通过调用longjmp(env, vlu);
     * 返回到最后一次注册到env里的setjmp处，并且此时setjmp返回vlu
     * jmp_buf 本质是一个类数组类型!
     */

    /* 这里设置env的主要目的就是读取该函数调用链前面的函数在栈中可能留存的堆地址
    *  but也可能真的是为了读取寄存器中指向堆块的值，只是我的测试用例没这个罢了233
    */ 
    setjmp(env);
    for (i = 0; i < sizeof(env); i++) {
        gc_mark(((void **)env)[i]);
    }
}

void gc_mark_stack()
{
    set_stack_end();
    if (stack_start > stack_end) {
        gc_mark_range(stack_end, stack_start);
    }else{
        gc_mark_range(stack_start, stack_end);
    }
}

void gc_sweep()
{
    size_t i;
    Header *p, *pend, *pnext;

    for (i = 0; i < gc_heaps_used; i++) {
        pend = (Header*)((char*)gc_heaps[i].slot + gc_heaps[i].size);
        for (p = gc_heaps[i].slot; p < pend; p = NEXT_HEADER(p)) {
            if (FL_TEST(p, FL_ALLOC)) { // 如果该堆块是使用状态，就查看是否已经标记，已标记就将标记取消以待monitor继续运行；没标记的就不是活动对象，清理它
                if (FL_TEST(p, FL_MARK)) {
                    printf("mark unset : %p\n", p);
                    FL_UNSET(p, FL_MARK);
                }else{
                    mini_gc_free(p+1);
                }
            }
        }
    }
}

void garbage_collect()
{
    size_t i;

    // mark
    gc_mark_register();
    gc_mark_stack();

    // mark roots
    /*
    *   因为这里我们只sbrk出来两块内存作为所谓的空闲堆块，而按GC的理论，可能存在很多块sbrk出来的内存作为空闲堆块。这里进行了简化，所以就没有做root_ranges_used的记录，不然应该
    *   在add_heap处进行记录，然后再重构一个类似的garbage_collect函数来处理不同块的内存，接着还应该设计一个真正的struct root来记录不同块里的情况
    */
    for (i = 0; i < root_ranges_used; i++) {
        gc_mark_range(root_ranges[i].start,root_ranges[i].end);
    }

    gc_sweep();
}

void gc_init()
{
    long dummy;

    dummy = 42;

    // 利用全局变量来记录栈开始的地方（虽然不是main里开始的栈，但是在这里开辟的也相当于起始位置了）
    stack_start = ((void *)&dummy);
}

void set_stack_end()
{
    void *tmp;
    long dummy;

    dummy = 42;
    stack_end = (void*)&dummy;
}

void test_garbage_collect()
{
    void *p;
    p = mini_gc_malloc(0x100);
    p = 0;
    garbage_collect();
}

void test_garbage_collect_load_test()
{
    void *p;
    int i;
    for (i = 0; i < 2000; i++) {
        p = mini_gc_malloc(0x100);
    }
    assert((((Header*)p)-1)->flags);
    assert(stack_start != stack_end);
}

void test()
{
    gc_init();
    mini_gc_malloc_and_free();
    test_garbage_collect();
    test_garbage_collect_load_test();
}

int main(int argc, char* argv[])
{
    test();
    return 0;
}