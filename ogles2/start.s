| ogles2.library starts here. Run as a program by mistake, it returns -1
| (a library is not an executable); exec finds the library through the
| resident tag (ROMTag in library.c) instead.
	.text
	.globl	_ogles2_start
_ogles2_start:
	moveq	#-1,d0
	rts
