#include "mmu.h"

  # vectors.S sends all traps here.
.globl alltraps
alltraps:
  # Build trap frame.
  pushl %ds
  pushl %es
  pushl %fs
  pushl %gs
  pushal
  
  # Set up data segments.
  # set up processor to run kernel C code, trap()
  # SEG_KDATA = 2
  movw $(SEG_KDATA<<3), %ax
  movw %ax, %ds
  movw %ax, %es

  # Call trap(tf), where tf=%esp
  pushl %esp       # create an argument for trap(tf)
  call trap
  addl $4, %esp

  # Return falls through to trapret...
  # Trapret use pop instuction to restore registers from 
  # the trap frame just as swtch did with the kernel context
.globl trapret
trapret:
  popal            # restore the general registers
  popl %gs
  popl %fs
  popl %es
  popl %ds
  addl $0x8, %esp  # skip over trapno and errcode
  iret             # iret pop %cs, %eip, %flags, %esp and %ss from the stack
