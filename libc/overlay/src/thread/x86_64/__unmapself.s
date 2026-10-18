/* ThreadOS: only a detached thread ending reaches this, and there are
   no such threads (see clone.s) */
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	ud2
