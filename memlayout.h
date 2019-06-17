// Memory layout

#define EXTMEM  0x100000            // Start of extended memory, 1MB
#define PHYSTOP 0xE000000           // Top physical memory, 224MB
#define DEVSPACE 0xFE000000         // Other devices are at high addresses, 4064MB

// Key addresses for address space layout (see kmap in vm.c for layout)
#define KERNBASE 0x80000000         // First kernel virtual address, 2GB
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked

#define V2P(a) (((uint) (a)) - KERNBASE)
#define P2V(a) ((void *)(((char *) (a)) + KERNBASE))

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as P2V, but without casts
