# Initial process execs /init.
# This code runs in user space.

#include "syscall.h"
#include "traps.h"


# exec(init, argv)
.globl start
start:
  pushl $argv # 将exec()的参数压到用户栈上
  pushl $init
  pushl $0  // where caller pc would be
  movl $SYS_exec, %eax    # 准备好系统调用号
  int $T_SYSCALL    # 执行int指令，将
  # 如果成功的话, exec()不会返回
  # above all is asking the kernel to run the exec system call
  # it running the program named by $init, which is point to "/inin\0" below

# for(;;) exit(); exec()如果不成功, 就会返回到这里, 永远循环下去
exit:
  movl $SYS_exit, %eax
  int $T_SYSCALL
  jmp exit

# char init[] = "/init\0";
init:
  .string "/init\0"

# char *argv[] = { init, 0 };
.p2align 2
argv:
  .long init
  .long 0

