## GC和普通堆管理器的区别

以下只是个人的浅显理解，如有错误还望斧正。

从编程语言的角度来看，GC就相当于自动帮你计算堆块的生命周期，当某块堆块的生命周期结束时，GC会自动将其清理。

从更底层的角度来看，GC相当于一个自动检测内存中堆块的使用情况的检测器，拿Ubuntu的堆管理器举例，GC的申请堆块只是对malloc，alloc的封装，释放堆块也只是对free的封装，本质上对于堆块的申请释放都是由glibc管理。GC的主要工作是自动对结束生命周期的堆块进行识别，并将其放入自己预设的空闲链表，最后统一由free进行释放。

简而言之，还是拿Ubuntu举例，相当于GC是glibc的上层，识别了哪些是活动对象哪些是非活动对象。

## 标记清除法

由标记阶段和清除阶段构成。标记阶段是把所有**活动对象**都做上标记；清除阶段是把那些没有标记的对象，也就是非活动对象回收的阶段。

因为我自己撰写的GC就是以最简单的标记清除法来实现的，所以我就借用我的GC来展现标记清除法。先提出一点，我自己撰写的并不是很严谨的参考GC和普通堆管理器之间的关系来设计，也就是说对于堆块的申请和释放并不是交由malloc，free函数来实现的，而是很大程度上参考了malloc lab，自行在GC里写了个最简易的堆管理，弱化了二者之间的关系。

### 标记阶段

首先需要明确的是，从理论角度来说，标记清除法的标记是从所谓的**根**开始的，由根直接引用的对象进行第一次标记，接着进行递归，搜索间接引用的子对象对其进行标记。如下图所示：

![GC标记-清除算法- Clinat Blog](https://gitee.com/zhzzhz/blog_warehouse/raw/master/img/marksweep0.png)

图中是运用所谓的链表结构来进行链接的，但是我写的时候，寻思，借鉴malloc lab的写法，写一个宏定义，利用size来进行寻址，结合链表的结构，所以设计了一个堆块的头部以及对应的堆块申请释放方式

```c
typedef struct header {
    size_t flags;
    size_t size;
    struct header *next_free;
} Header;

void* mini_gc_malloc(size_t size)
{
    size_t asize;
    Header *p, *prevp;
    size_t do_gc = 0;
	......
    for (p = prevp->next_free; ; prevp = p, p = p->next_free){
        if (p->size >= (asize + HEADER_SIZE)) {
            if (p->size == (asize + HEADER_SIZE)) 
                prevp->next_free = p->next_free; 
            else {
                p->size -= (asize + HEADER_SIZE);
                p = NEXT_HEADER(p);
                p->size = asize;
            }
            free_list = prevp;
            FL_SET(p,FL_ALLOC);
            return (void*)(p+1);
        }
       ......
}
    
void mini_gc_free(void *ptr)
{
    Header *target, *hit;

    target = (Header *)ptr - 1;

    for (hit = free_list; !(target > hit && target < hit->next_free); hit = hit->next_free)
        if (hit >= hit->next_free && (target > hit || target < hit->next_free))
            break;
    if (NEXT_HEADER(target) == hit->next_free) { 
        target->size += (hit->next_free->size + HEADER_SIZE);
        target->next_free = hit->next_free->next_free;
    }else{ 
        target->next_free = hit->next_free; 
    }
    if (NEXT_HEADER(hit) == target) {
        hit->size += (hit->next_free->size + HEADER_SIZE);
        hit->next_free = target->next_free;
    }else{
        hit->next_free = target;
    }
    free_list = hit;
    target->flags = 0;
}

```

当`struct header`结构体的`next_free`已经形成了相应的链表结构时，就达成了上述图中的情况。此时还没进行标记操作，只是说这些堆块形成了一个链表结构。

接着我们继续回到`mini_gc_malloc`

```c
void* mini_gc_malloc(size_t size)
{
    size_t asize;           
    Header *p, *prevp;
    size_t do_gc = 0;

    if (size <= 0)
        return NULL;
    asize = ALIGN(size, PTRSIZE);

    ......
        if (p == free_list) {
            if (!do_gc){
                garbage_collect();
                do_gc = 1;
            }else if ((p = grow(asize + 2*HEADER_SIZE)) == NULL) 
                return NULL;
        }
    }
}
```

这里进行了类似于延迟回填的设计，因为进行的是单链表循环，所以当`p == free_list`的时候，代表着已经结束了一次循环遍历但是还是没有找到自己需要的空闲堆块，这时候才需要进行一次GC，整理一下堆块。

标记主要在`    gc_mark`函数进行，相应的调用链是`mini_gc_malloc --> garbage_collect --> gc_mark_register --> gc_mark`或者`gc_mark_range --> gc_mark`

```c
void gc_mark(void* ptr)
{
    GC_Heap *gh;
    Header *hdr;

    // mark check
    if (!(gh = is_pointer_to_heap(ptr))) return;
    if (!(hdr = get_header(gh,ptr))) return;
    if (!FL_TEST(hdr, FL_ALLOC)) return;
    if (FL_TEST(hdr, FL_MARK)) return;

    // marking
    FL_SET(hdr, FL_MARK);
    printf("mark ptr : %p, header : %p\n", ptr, hdr);

    // mark children
    gc_mark_range((void*)(hdr+1), (void*)NEXT_HEADER(hdr));
}
```

这里不得不先提到`gc_mark_register`的设计

```c
void gc_mark_register()
{
    jmp_buf env;
    size_t i;

    setjmp(env);
    for (i = 0; i < sizeof(env); i++) {
        gc_mark(((void **)env)[i]);
    }
}
```

setjmp这个特殊函数用于将寄存器的值放到局部变量（栈中），这样我们就可以对栈进行扫描，寻找里面可能保留的堆地址，对其进行标记。至于为什么要设计这样的一种函数，个人猜测是为了防止出现某些堆指针丢失，导致该堆块显式的完全消失，但是可能栈中还留存其信息。（这里想不出这种测试案例，就没有进行构造）

关于jmp_buf类型分析

```c
// setjmp.h
# if __WORDSIZE == 64
typedef long int __jmp_buf[8];
# elif defined  __x86_64__
__extension__ typedef long long int __jmp_buf[8];
# else
typedef int __jmp_buf[6];
# endif

/* Calling environment, plus possibly a saved signal mask.  */
struct __jmp_buf_tag
  {
    /* NOTE: The machine-dependent definitions of `__sigsetjmp'
       assume that a `jmp_buf' begins with a `__jmp_buf' and that
       `__mask_was_saved' follows it.  Do not move these members
       or add others before it.  */
    __jmp_buf __jmpbuf;     /* Calling environment.  */
    int __mask_was_saved;   /* Saved the signal mask?  */
    __sigset_t __saved_mask;    /* Saved signal mask.  */
  };

typedef struct __jmp_buf_tag jmp_buf[1];
```

根据动态调试，可以发现`__jmpbuf`成员保存的是一部分的寄存器，有一部分是真看不出来

```c
__jmpbuf[0] = rbx;
__jmpbuf[1] = rdx;
__jmpbuf[2] = r12;
__jmpbuf[3] = r13;
__jmpbuf[4] = r14;
__jmpbuf[5] = r15;
__jmpbuf[6] = rdx;
__jmpbuf[7] =;
```

### 清除阶段

当我们标记完所有的活动对象后，我们就进行我们的清除，清除主要是在`gc_sweep`函数中进行。因为这里我们只sbrk出来两块内存作为所谓的空闲堆块，而按GC的理论，可能存在很多块sbrk出来的内存作为空闲堆块。这里进行了简化，所以就没有做root_ranges_used的记录，不然应该在add_heap处进行记录，然后再重构一个类似的garbage_collect函数来处理不同块的内存，接着还应该设计一个真正的struct root来记录不同块里的情况。

所以这里最后的`test_garbage_collect_load_test`其实没做好，虽然逻辑上是当我们将两块sbrk出来的空闲堆块全部申请完毕后，我们会对其中一块进行GC，应该将测试案例做出一个类似于上面GC书中图的样子，struct root来进行记录，将仍在使用的堆块依次记录。（但是期末大作业真的麻了，大哥们有余力自己冲一冲）

```c
void garbage_collect()
{
    size_t i;

    // mark
    gc_mark_register();
    gc_mark_stack();

    // mark roots
    for (i = 0; i < root_ranges_used; i++) {
        gc_mark_range(root_ranges[i].start,root_ranges[i].end);
    }

    gc_sweep();
}
```

其在`gc_sweep`中针对没有标记的进行free操作

```c
void gc_sweep()
{
    size_t i;
    Header *p, *pend, *pnext;

    for (i = 0; i < gc_heaps_used; i++) {
        pend = (Header*)((char*)gc_heaps[i].slot + gc_heaps[i].size);
        for (p = gc_heaps[i].slot; p < pend; p = NEXT_HEADER(p)) {
            if (FL_TEST(p, FL_ALLOC)) { 
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
```





