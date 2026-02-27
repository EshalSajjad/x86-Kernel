KERNEL_CODE_SEGMENT = 0x08      /* Kernel code segment offset in the GDT */
KERNEL_DATA_SEGMENT = 0x10      /* Kernel data segment offset in the GDT */

.extern    interrupt_dispatch   /* Declare the external C handler for ISRs */

//macro 1 - with no error code
.macro ISR_NO_ERROR_CODE num
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common_handler
.endm

//macro 2 - with error code
.macro ISR_ERROR_CODE num
isr\num:
    pushl $\num
    jmp isr_common_handler
.endm


ISR_NO_ERROR_CODE 0
ISR_NO_ERROR_CODE 1
ISR_NO_ERROR_CODE 2
ISR_NO_ERROR_CODE 3
ISR_NO_ERROR_CODE 4
ISR_NO_ERROR_CODE 5
ISR_NO_ERROR_CODE 6
ISR_NO_ERROR_CODE 7
ISR_ERROR_CODE 8
ISR_NO_ERROR_CODE 9
ISR_ERROR_CODE 10
ISR_ERROR_CODE 11
ISR_ERROR_CODE 12
ISR_ERROR_CODE 13
ISR_ERROR_CODE 14
ISR_NO_ERROR_CODE 15
ISR_NO_ERROR_CODE 16
ISR_ERROR_CODE 17
ISR_NO_ERROR_CODE 18
ISR_NO_ERROR_CODE 19
ISR_NO_ERROR_CODE 20
ISR_ERROR_CODE 21
ISR_NO_ERROR_CODE 22
ISR_NO_ERROR_CODE 23
ISR_NO_ERROR_CODE 24
ISR_NO_ERROR_CODE 25
ISR_NO_ERROR_CODE 26
ISR_NO_ERROR_CODE 27
ISR_NO_ERROR_CODE 28
ISR_ERROR_CODE 29
ISR_ERROR_CODE 30
ISR_NO_ERROR_CODE 31

//irqs cont
.globl isr0
.globl isr1
.globl isr2
.globl isr3
.globl isr4
.globl isr5
.globl isr6
.globl isr7
.globl isr8
.globl isr9
.globl isr10
.globl isr11
.globl isr12
.globl isr13
.globl isr14
.globl isr15
.globl isr16
.globl isr17
.globl isr18
.globl isr19
.globl isr20
.globl isr21
.globl isr22
.globl isr23
.globl isr24
.globl isr25
.globl isr26
.globl isr27
.globl isr28
.globl isr29
.globl isr30
.globl isr31

.globl irq0
irq0:
    pushl $0
    pushl $32
    jmp isr_common_handler

.globl irq1
irq1:
    pushl $0
    pushl $33
    jmp isr_common_handler

.globl irq2
irq2:
    pushl $0
    pushl $34
    jmp isr_common_handler

.globl irq3
irq3:
    pushl $0
    pushl $35
    jmp isr_common_handler

.globl irq4
irq4:
    pushl $0
    pushl $36
    jmp isr_common_handler

.globl irq5
irq5:
    pushl $0
    pushl $37
    jmp isr_common_handler

.globl irq6
irq6:
    pushl $0
    pushl $38
    jmp isr_common_handler

.globl irq7
irq7:
    pushl $0
    pushl $39
    jmp isr_common_handler

.globl irq8
irq8:
    pushl $0
    pushl $40
    jmp isr_common_handler

.globl irq9
irq9:
    pushl $0
    pushl $41
    jmp isr_common_handler

.globl irq10
irq10:
    pushl $0
    pushl $42
    jmp isr_common_handler

.globl irq11
irq11:
    pushl $0
    pushl $43
    jmp isr_common_handler

.globl irq12
irq12:
    pushl $0
    pushl $44
    jmp isr_common_handler

.globl irq13
irq13:
    pushl $0
    pushl $45
    jmp isr_common_handler

.globl irq14
irq14:
    pushl $0
    pushl $46
    jmp isr_common_handler

.globl irq15
irq15:
    pushl $0
    pushl $47
    jmp isr_common_handler

.globl isr128
isr128:
    pushl $0
    pushl $128
    jmp isr_common_handler


/* Interrupt handlers entry points will look something like this:
    .globl isr0
isr0:
    pushl $0 # push dummy error code
    pushl $0 # push interrupt number
    jmp isr_common_handler # jump instead of a call


     */


/* We then call the macros to put in place handlers for all exceptions. Note
    that the definitions for all 256 entries will be provided on the per
    need basis. */

// -- ADD THE ASSEMBLY ENTRY POINTS HERE FOR THE 32 EXCEPTIONS -- //

// CPU exceptions


// reserved interrupts from 22 to 31


// The following are the IRQ definitions for the PIC generated interrupts.
// master pic IRQs are from 0 to 7, and slave pic IRQs are from 8 to 15.


// interrupt 0x80 is used for system calls, so we configure the entry for that.

/**
 * @brief Common ISR handler that saves the state, calls the C handler, 
 *          and restores the state. This function is called by all ISRs to
 *          handle the interrupt. It saves the registers, sets up the segment
 *          registers, calls the C handler (interrupt_dispatch), and restores
 *          the state before returning.
 */

.type isr_common_handler, @function
isr_common_handler:
    
    /* 1.save1 the GP registers */
    /* in the order: eax, ecx, edx, ebx, old esp, ebp, esi, edi */
    /* 2. save the data segment register */
    /* 3. Call the C handler for the interrupt dispatching */   
    /* clean up the stack and restore the state */
    /* restore the gp registers */
    /* stack cleanup, must be restored to the state before the ISR. Done because
        the iret instruction expects the previously automatically pushed fields:
        ss, eflags, cs, ip. These are pushed automatically by the CPU when an
        interrupt occurs. */
    /* Return from the interrupt, restoring the flags and segments */

    pusha
    push %ds
    call interrupt_dispatch
    pop %ds
    popa
    add $8, %esp
    iret