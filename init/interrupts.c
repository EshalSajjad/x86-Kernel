#include "init/idt.h"
#include "../include/interrupts.h"
#include "driver/serial.h"
#include "stdio.h"
#include "string.h"
#include "utils.h"
// #include "driver/pic.h"

interrupt_service_t interrupt_handlers[256];

//context has the reg state and int no
//if no handler, default error handling
//if handler, call it with context
//if PIC intno (32-48), send EOI to PIC (using pic_send_eoi(int no))
//but it has to check if EOI is needed (if PIC intno is interrupt or not)
void interrupt_dispatch (interrupt_context_t context){
    if(interrupt_handlers[context.int_no] != NULL){
        interrupt_handlers[context.int_no](&context);
    }
    if(context.int_no >= 32 && context.int_no <= 47){
        pic_send_eoi(context.int_no);
    }
}

/*It takes the interrupt number (0-255) and a function pointer to the interrupt service
routine, then stores this mapping in the interrupt handler table.*/
void register_interrupt_handler (uint8_t int_no, interrupt_service_t routine){
    interrupt_handlers[int_no] = routine;
}

/*It takes the interrupt number and the routine pointer, then clears
the corresponding entry in the interrupt handler table.*/
void unregister_interrupt_handler (uint8_t int_no){
    interrupt_handlers[int_no] = NULL;
}

/*It returns a function pointer to the handler, or NULL if no
handler is registered for that interrupt. The data type interrupt service t is defined in
include/init/interrupts.h.*/
interrupt_service_t get_interrupt_handler (uint8_t int_no){
    if(interrupt_handlers[int_no] != NULL){
        return interrupt_handlers[int_no];
    }
    else{
        return NULL;
    }
}

//initialization of the x86 interrupt system.
//All private variables are initialized.
//pic init, idt init, sti 
void setup_x86_interrupts(){
    memset(interrupt_handlers, 0, sizeof(interrupt_service_t)* 256);
    pic_init(32, 40);
    idt_init();
    sti();
}