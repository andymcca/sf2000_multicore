.set noreorder
.globl __start
__start:
	j	load_and_run_core	# 0x80001500
	nop
	j	hook_sys_watchdog_reboot	# 0x80001508
	nop
# 0x80001510
	mfc0	$ra, $14	# EPC
# curiously enough this core actually supports ehb but we won't use it atm
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	j	hook_exception_handler
	srl	$a0, $k1, 2	# masked Cause from INT_General_Exception_Hdlr
	nop
# 0x80001540
	j	hook_switch_tv
	nop
# 0x80001548
	j	hook_rotate
	nop
# 0x80001550
	j	hook_lcd_init
	nop