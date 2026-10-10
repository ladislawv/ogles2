; libogles2 for vbcc: calls through ogles2.library's vectors (vasm, Motorola
; syntax). Same functions as lvo.s, which is the GCC version.
	section	code,code

	xref	_OGLES2Base

; const struct OGLES2Dispatch *OGLES2GetDispatchTableLVO(void)   -30
	xdef	_OGLES2GetDispatchTableLVO
_OGLES2GetDispatchTableLVO:
	move.l	a6,-(sp)
	move.l	_OGLES2Base,a6
	move.l	a6,d0
	beq.s	.none
	jsr	-30(a6)
.none:	move.l	(sp)+,a6
	rts

; ULONG OGLES2PrivateLogFlushLVO(void)   -36, private
	xdef	_OGLES2PrivateLogFlushLVO
_OGLES2PrivateLogFlushLVO:
	move.l	a6,-(sp)
	move.l	_OGLES2Base,a6
	move.l	a6,d0
	beq.s	.none
	jsr	-36(a6)
.none:	move.l	(sp)+,a6
	rts
