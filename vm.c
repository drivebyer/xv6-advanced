#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  /*
    只是设置当前CPU到GDT
    将c->gdt的地址（注意是虚拟地址）加载到GDTR寄存器中
  */
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
/*
 * walkpgdir() mimics the actions of the x86 paging hardware
 * as it looks up the PTE for a virtual address
 * 
 * 首先用VA的高10位找到PDE, 如果entry的P flag为0, 说明这个entry对应的page-table page不存在, 在不存在的情况下,
 * 如果alloc=1, 那么就通过kalloc()分配一个physical page用作page-table page, 并把它的物理地址赋值给PDE,
 * 然后再用VA的中间10位去找到这个刚分配的page-table page上面的PTE
 */
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  /*page directory entry, kmap数组是va的来源*/
  pde = &pgdir[PDX(va)]; /*uses the upper 10 bits of the virtual address to find the PDE's address*/
  if(*pde & PTE_P){ /*如果if成立, */
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde)); /*PTE_ADDR extracts the PPN(即page-table page在内存中的物理基地址) from PDE, P2V() adds 0x80000000, since PTE holds physical address*/
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0) /*如果not exsits, alloc a page-table page*/
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    /*
     * 设置page directory entry的值, 这个entry值的PPN指向的是page-table page的基地址, 即kalloc()得到的虚拟地址
     * 因为是在kernel里, 所以直接将虚拟地址减去KERNBASE(V2P)就能得到对应的物理地址
     * 可以看到PDE设置了PTE_U标志位, 而PTE并没有设置这个标志位
     */
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)]; /*获得VA对应的PTE的地址*/
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
/*完成VA+size到PA+size范围内的映射, 即向相应的PTE中赋值*/
int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0) /*walkpgdir()中已经设置好PDE*/
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    /*
     * pte = &pgtab[PTX(va)]; 向虚拟地址中间10位指定的PTE中填充数据
     */
    *pte = pa | perm | PTE_P; /*将PTE与提供好的物理地址PA相联系, 至此已经完成了VA到PA的映射*/
    if(a == last)
      break;
    /*接着处理下一页*/
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt; /*e.g. KERNBASE*/
  uint phys_start; /*e.g. 0*/
  uint phys_end; /*e.g. EXTMEM*/
  int perm; /*e.g. PTE_W*/
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
/*
 * 在内核执行时候切换页表是可行的, 因为每个进程页表的内核部分的映射都是一样的
 * setupkvm() 只设置内核部分的页表内容, 不涉及到user memory的映射
 */
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;
  /*
   * 分配一个物理页保存Page Directory, 返回的是这个物理页的VA
   * 注意这里只创建了Page Directory, 还没有创建对应的Page-Table page
   */
  if((pgdir = (pde_t*)kalloc()) == 0) 
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");

  /*
   * 开始使用kmap里描述了内核的映射关系[VA <-> PA]来建立Page-Table page及其entry
   * 注意: 是已知VA与PA的对应关系的前提下, 来建立PTE, 这样建立出的PTE自然就描述了该VA与PA的关系
   */
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) { 
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
/*在这之前我们使用的是entrypgdir中关于内核简单(加减KERNBASE)的映射*/
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
/*
  将kpgdir的物理地址加载进cr3寄存器
*/
void
switchkvm(void)
{
  /*在切换之前我们还是使用的entrypgdir中的映射, 所以虚拟地址减去KERNBASE就是物理地址*/
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
/* tell the hardware to start using the target process's page table
 * 
 * set %cr3 with new page table
 * also flushed some MMU caches so it will see new PTEs
 * 注意 pushcli() 和 popcli() 的使用，在执行特定代码段时，不接受中断
 */
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  /* set up a task state segment SEG_TSS that 
   * instructs the hardware to execute system calls 
   * and interrupt on the process's kernel stack
   */
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  /*
   * +-----+ <- cpu->ts.esp0 ------+
   * |  ss |                       |
   * +-----+                       |
   * | esp |                       |
   * +-----+                       |
   * |eflag|                       |
   * +-----+                       |
   * | ..  |                       KSTACKSIZE
   * +-----+                       |
   * | edi |                       |
   * +-----+ <- esp                |
   * |empty|                       |
   * +-----+                       |
   * | ..  |                       |
   * +-----+ <- p->kstack    ------+
   */
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE; /*此时esp0指向内核栈最高位置处，即起始位置处*/
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3); 

  // 切换到目标进程的页目录，CR3寄存器存放的是页目录的物理地址
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
/* 
 * inituvm() copy the binary into the new process's memory
 * and allocate one page of physical memory, 
 * and maps virtual address zero to that physical memory,
 * and copy the binary to that page
 */
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");

  /* 分配一页(4KB)的物理内存，来保存init二进制文件 */
  mem = kalloc(); 
  
  memset(mem, 0, PGSIZE); 

  /* 将init二进制文件所在的物理区域，映射到虚拟内存从0开始的空间 */
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U); 
  
  /*将二进制文件拷贝到这一页物理内存上*/
  memmove(mem, init, sz); 
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
/*
 * 
 */
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0) /*find the physical address of the alocated memory at which to write each page of the ELF segment*/
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n) /*read from the file*/
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE) /*check that the virtual address requested is below KERNBASE*/
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);

    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){ 
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

