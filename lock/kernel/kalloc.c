// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct Keme{
  struct spinlock lock;
  struct run *freelist;
};
//为每个cpu分配一个空闲链表以及锁
struct Keme kmems[NCPU];

void
kinit()
{
  //初始化锁
  for(int i = 0; i < NCPU; i++){
    initlock(&kmems[i].lock, "kmem");
      // we build a single freelist to kmems[0] firstly 
      if (i == 0) 
        freerange(end, (void*)PHYSTOP);
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu_id = cpuid(); 
  pop_off();

  //对每个cpu进行上锁，然后释放空闲页 
  acquire(&kmems[cpu_id].lock);
  r->next = kmems[cpu_id].freelist;
  kmems[cpu_id].freelist = r;
  release(&kmems[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cpu_id = cpuid();
  pop_off();

  acquire(&kmems[cpu_id].lock); 

  r = kmems[cpu_id].freelist;
  
  // 如果当前空闲链表有空闲页
  if(r){
    kmems[cpu_id].freelist = r->next;
    release(&kmems[cpu_id].lock);
    memset((char*)r, 5, PGSIZE); // fill with junk
    return (void*)r;
  }

  // 没有从其它cpu偷
  else{
    release(&kmems[cpu_id].lock);
    for(int i = 0; i < NCPU; i++){
      // 避免 race condition
      if (i != cpu_id){ 
        acquire(&kmems[i].lock);
        if(i == NCPU - 1 && kmems[i].freelist == 0){  
          release(&kmems[i].lock);
          return (void*)0;
        }
        if(kmems[i].freelist){
          struct run *to_alloc = kmems[i].freelist; 
          kmems[i].freelist = to_alloc->next;
          release(&kmems[i].lock);
          memset((char*)to_alloc, 5, PGSIZE); // fill with junk
          return (void*)to_alloc;
        }
        release(&kmems[i].lock); 
      }
    }
  }

  // Returns 0 if the memory cannot be allocated. 
  return (void*)0;
}
