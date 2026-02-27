#include <proc/tss.h>
#include <stdint.h>
#include <string.h>

static tss_t tss;

tss_t* tss_get_global(void){
    return &tss;
}
void tss_update_esp0(uint32_t esp0){
    tss.esp0 = esp0;
}
void tss_flush(uint16_t selector){
    asm volatile ("ltr %0" : : "r"(selector));
}