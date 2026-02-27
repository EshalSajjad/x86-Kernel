#include "../include/interrupts.h"
#include <stdio.h>
#include "init/idt.h"
#include <string.h>

idt_entry_t idt_entries[256];
idt_ptr_t idt_ptr;

void create_idt_entry (idt_entry_t* entry, uint32_t isr_addr, uint16_t selector, uint8_t type_attr){
    entry->isrAddr_low = isr_addr & 0xFFFF; //last 16 bits
    entry->selector = selector;
    entry->zero = 0;
    entry->type_bitfive = type_attr;
    entry->isrAddr_high = (isr_addr >> 16) & 0xFFFF;
}

/*first, initialize the idt table with 256 NULL vals,
then initialise the first 32 interrupts (cpu exceptions),
then load*/
void idt_init (){
    // idt_ptr_t idt_ptr;
    idt_ptr.limit = (sizeof(idt_entry_t) * 256) - 1;
    idt_ptr.base = (uint32_t) &idt_entries;
    create_idt_entry(&idt_entries[0], (uint32_t)isr0, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[1], (uint32_t)isr1, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_TRAP);
    create_idt_entry(&idt_entries[2], (uint32_t)isr2, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[3], (uint32_t)isr3, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_TRAP);
    create_idt_entry(&idt_entries[4], (uint32_t)isr4, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_TRAP);
    create_idt_entry(&idt_entries[5], (uint32_t)isr5, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[6], (uint32_t)isr6, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[7], (uint32_t)isr7, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[8], (uint32_t)isr8, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[9], (uint32_t)isr9, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[10], (uint32_t)isr10, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[11], (uint32_t)isr11, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[12], (uint32_t)isr12, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[13], (uint32_t)isr13, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[14], (uint32_t)isr14, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[15], (uint32_t)isr15, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[16], (uint32_t)isr16, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[17], (uint32_t)isr17, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[18], (uint32_t)isr18, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[19], (uint32_t)isr19, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[20], (uint32_t)isr20, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[21], (uint32_t)isr21, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[22], (uint32_t)isr22, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[23], (uint32_t)isr23, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[24], (uint32_t)isr24, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[25], (uint32_t)isr25, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[26], (uint32_t)isr26, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[27], (uint32_t)isr27, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[28], (uint32_t)isr28, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[29], (uint32_t)isr29, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[30], (uint32_t)isr30, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[31], (uint32_t)isr31, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);

    create_idt_entry(&idt_entries[32], (uint32_t)irq0, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[33], (uint32_t)irq1, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[34], (uint32_t)irq2, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[35], (uint32_t)irq3, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[36], (uint32_t)irq4, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[37], (uint32_t)irq5, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[38], (uint32_t)irq6, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[39], (uint32_t)irq7, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[40], (uint32_t)irq8, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[41], (uint32_t)irq9, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[42], (uint32_t)irq10, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[43], (uint32_t)irq11, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[44], (uint32_t)irq12, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[45], (uint32_t)irq13, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[46], (uint32_t)irq14, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);
    create_idt_entry(&idt_entries[47], (uint32_t)irq15, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL0 | IDT_GATE_TYPE_32_INT);

    memset(&idt_entries[48], 0, sizeof(idt_entry_t) * (256 - 48));

    create_idt_entry(&idt_entries[128], (uint32_t)isr128, 0x08, IDT_ATTR_PRESENT | IDT_ATTR_DPL3 | IDT_GATE_TYPE_32_TRAP);
    
    load_idt((uint32_t)&idt_ptr);
}