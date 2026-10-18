/* ThreadOS: vfork is a fork - copying costs more, but everything a
   vfork child may do is also allowed in a fork child */
.global vfork
.type vfork,@function
vfork:
	jmp fork
