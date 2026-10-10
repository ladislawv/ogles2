| libogles2.a: calls through ogles2.library's vectors (GCC, stack ABI).
	.text
	.even
	.extern	_OGLES2Base

| const struct OGLES2Dispatch *OGLES2GetDispatchTableLVO(void)   -30
	.globl	_OGLES2GetDispatchTableLVO
_OGLES2GetDispatchTableLVO:
	move.l	a6,-(sp)
	move.l	_OGLES2Base,a6
	move.l	a6,d0
	beq.s	1f
	jsr	-30(a6)
1:	move.l	(sp)+,a6
	rts

| ULONG OGLES2PrivateLogFlushLVO(void)   -36, private
	.globl	_OGLES2PrivateLogFlushLVO
_OGLES2PrivateLogFlushLVO:
	move.l	a6,-(sp)
	move.l	_OGLES2Base,a6
	move.l	a6,d0
	beq.s	1f
	jsr	-36(a6)
1:	move.l	(sp)+,a6
	rts
