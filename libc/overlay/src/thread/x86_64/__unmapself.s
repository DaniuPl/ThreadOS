/* ThreadOS: a detached thread ending. Upstream unmaps its own stack and
   exits without touching memory again. Here munmap only files the range
   for reuse (src/threados/syscall.c), so the range is filed as "free
   once this thread is gone" instead, and the next mmap checks. The C
   function runs on the very stack it files, which is safe for exactly
   that reason: nobody gets it before the thread has ended. */
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	jmp __tos_unmapself
