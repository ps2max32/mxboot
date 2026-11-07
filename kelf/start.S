# mxboot's phase 1: the entry. As maxsdk's crt0.S without its runtime: clear
# the BSS, SetupThread (the kernel fills `args` with argc and argv, as ExecPS2
# passed them), then main(argc, argv).

	.set noreorder
	.set noat
	.section .text.start, "ax", @progbits
	.globl __start
	.type __start, @function
	.ent __start
__start:
	la $2, __bss_start
	la $3, __bss_end
1:	sltu $1, $2, $3
	beq $1, $0, 2f
	nop
	sq $0, 0($2)
	b 1b
	addiu $2, $2, 16
2:
	# SetupThread(gp, stack, stack_size, args, root): returns the stack pointer
	la $4, _gp
	la $5, __stack
	la $6, __stack_size
	la $7, args
	la $8, exit_thread
	move $28, $4
	li $3, 0x3c
	syscall
	move $29, $2
	la $2, args
	lw $4, 0($2)        # argc
	jal main
	addiu $5, $2, 4     # argv
	b .
	nop
	.end __start

	.type exit_thread, @function
exit_thread:
	li $3, 35
	syscall
	b .
	nop
