// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld
/*
 * The allocator's data structure is a free list of physical memory pages that
 * are available for allocation. Each free page's list element is a struct run.
 * It store each free page's run structure in the free list itself, since there's
 * nothing else stored here.
 */
struct run {
  struct run *next;
};

/*
 * The list and lock are wrapped in a struct to make clear
 * the that the lock protects the fields in the struct 
 */
struct {
  struct spinlock lock; /*The free list of physical memory is protected by a apinlock*/
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases. 
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
/*the reason for having two calls is that for much of main one can not use locks or memory above 4MB*/

/*The call to kinit1() sets up for lock-less allocation in the first 4MB*/
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

/*The call to kinit2() enables locking and arranges for more memory to be allocatable*/
void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

/*freerange() add memory to the free list via per-page calls to kfree*/
void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  /*
   * This will cause code that uses memory after freeing it(uses "dangling references")
   * to read garbage instead of the old valid contents; hopefully that will cause
   * such code to break faster
   */
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v; 
  r->next = kmem.freelist; /*to record the old start of the free list in r->next*/
  kmem.freelist = r; /*set the free list equal to r*/
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
/*
 * kalloc() removes and returns the first element in the free list
 */
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

