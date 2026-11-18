// ThreadOS: a thread is SYS64_CLONE (83), as on the PC
// (src/thread/x86_64/clone.s), with a struct (kernel64/proc64.h)
// rather than Linux's registers:
//
//   entry, stack, tls, parent_tid, clear_tid
//
// __clone(func, stack, flags, arg, ptid, tls, ctid)
//         x0,   x1,    w2,    x3,  x4,   x5,  x6
//
// Anything that is not a thread (CLONE_THREAD) is posix_spawn's
// vfork-like child: a fork, which runs func(arg) on its own copy
// (__tos_clone_process). For a thread, func and arg go on the new
// stack, and the new thread starts at 1: below, where it takes them
// off and calls func(arg); what func returns is the thread's exit code
// (SYS64_EXIT, 2, which ends only the calling thread). The thread
// pointer (TPIDR_EL0) is the kernel's to set: it is what 'tls' is.

.text
.global __clone
.hidden __clone
.type   __clone,%function
__clone:
	tbnz w2,#16,3f            // CLONE_THREAD
	mov x1,x3                 // arg
	b __tos_clone_process
3:	and x1,x1,#-16
	stp x0,x3,[x1,#-16]!      // func, arg
	sub sp,sp,#48
	adr x9,1f
	stp x9,x1,[sp]            // entry, stack
	stp x5,x4,[sp,#16]        // tls, parent_tid
	str x6,[sp,#32]           // clear_tid
	mov x0,sp
	mov x8,#83
	svc #0
	add sp,sp,#48
	tbz x0,#63,2f
	mov x0,#-11               // -EAGAIN: musl turns every failure into it
2:	ret
1:	mov x29,#0
	mov x30,#0
	ldp x1,x0,[sp],#16        // func, arg; the stack is sixteen-aligned
	blr x1
	mov w0,w0                 // the code, as SYS64_EXIT takes it
	mov x8,#2
	svc #0
	brk #0
