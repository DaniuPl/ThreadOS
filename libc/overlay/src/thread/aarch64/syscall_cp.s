// ThreadOS: a cancellation point is a system call like any other,
// through __tos_syscall (as src/thread/x86_64/syscall_cp.s). The
// labels have to stay - pthread_cancel.c looks at them to tell whether
// a thread stopped inside the call.
//
// __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z)
//                  x0             x1  x2 x3 x4 x5 x6 x7
// __tos_syscall(nr, u, v, w, x, y, z)
//               x0  x1 x2 x3 x4 x5 x6

.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type __syscall_cp_asm,%function
__syscall_cp_asm:
__cp_begin:
	ldr w9,[x0]
	cbnz w9,__cp_cancel
	stp x29,x30,[sp,#-16]!
	mov x29,sp
	mov x0,x1                 // the number
	mov x1,x2                 // and six arguments, one register down
	mov x2,x3
	mov x3,x4
	mov x4,x5
	mov x5,x6
	mov x6,x7
	bl __tos_syscall
	ldp x29,x30,[sp],#16
__cp_end:
	ret
__cp_cancel:
	b __cancel
