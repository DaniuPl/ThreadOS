// ThreadOS: a detached thread ending, as src/thread/x86_64/__unmapself.s:
// munmap only files the range for reuse, so the range is filed as
// "free once this thread is gone" and the C function runs on the very
// stack it files.
.text
.global __unmapself
.type   __unmapself,%function
__unmapself:
	b __tos_unmapself
