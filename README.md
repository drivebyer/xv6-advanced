# xv6-advanced
MIT xv6 with more feture

Goal:
- Modify xv6 so that the pages for the kernel are shared among processes, which
reduces memory consumption.
- Modify xv6 so that when a user program dereferences a null pointer, it will receive a fault. That is, modify xv6 so that virtual address 0 isn’t mapped for user programs
- How would you improve xv6’s memory layout if xv6 where running on a 64-bit
processor?
-  Write a driver for a disk that supports the SATA standard (search for SATA on
the Web). Unlike IDE, SATA isn’t obsolete. Use SATA’s tagged command queuing to
issue many commands to the disk so that the disk internally can reorder commands to
obtain high performance.
- Add simple driver for an Ethernet card.
- Implement a subset of Pthreads in xv6. That is, implement a user-level thread
library so that a user process can have more than 1 thread and arrange that these
threads can run in parallel on different processors. Come up with a design that correctly handles a thread making a blocking system call and changing its shared address
space.
- Implement semaphores in xv6. You can use mutexes but do not use sleep and
wakeup. Replace the uses of sleep and wakeup in xv6 with semaphores. Judge the result.
