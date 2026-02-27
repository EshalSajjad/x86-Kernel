#include "../include/mm/kmm.h"
#include <../include/mem.h>
#include <stdint.h>
#include <stddef.h>

static uint32_t* memory_bitmap = NULL;
static uint32_t total_frames = 0;
static uint32_t used_frames = 0;
static uint32_t bitmap_size = 0;
extern uint32_t kernel_start;
extern uint32_t kernel_end;

//make used frames
static void mark_frame(uint32_t frame){
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    memory_bitmap[idx] |= (1 << bit);
}

//make free frames
static void unmark_frame(uint32_t frame){
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    memory_bitmap[idx] &= ~(1 << bit);
}

//check if a bit is set basically checking is a frame is used
static bool isframe_used(uint32_t frame){
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    return (memory_bitmap[idx] & (1 << bit)) != 0;
}

//first free frame
static uint32_t find_first_free(void){
    for(uint32_t i = 0; i < bitmap_size; i++){
        if(memory_bitmap[i] != 0xFFFFFFFF){
            for(uint32_t j = 0; j < 32; j++){
                uint32_t frame = i * 32 + j;
                if(frame >= total_frames){
                    return (uint32_t)-1;
                }
                if(!isframe_used(frame)){
                    return frame;
                }
            }
        }
    }
    return (uint32_t)-1;
}

void kmm_setup_memory_region(uint32_t base, uint32_t size, bool is_reserved){
    uint32_t start_frame = base / _KMM_BLOCK_SIZE;
    uint32_t end_frame = (base + size) / _KMM_BLOCK_SIZE;
    
    for(uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++){
        if(is_reserved){
            mark_frame(frame);
            used_frames++;
        }
        else{
            unmark_frame(frame);
            used_frames--;
        }
    }
}

void kmm_init(void){
    e801_memsize_t* memsize = (e801_memsize_t*)MEM_SIZE_LOC;
    uint32_t total_memory = (1024 * 1024);
    total_memory += memsize->memLow * 1024;
    total_memory += memsize->memHigh * 64 * 1024;
    
    total_frames = total_memory / _KMM_BLOCK_SIZE;
    bitmap_size = (total_frames + 31) / 32;
    
    uint32_t kernel_start_virt = (uint32_t)&kernel_start;
    uint32_t kernel_end_virt = (uint32_t)&kernel_end;
    uint32_t kernel_start_phys = (uint32_t)VIRT_TO_PHYS(&kernel_start);
    uint32_t kernel_end_phys = (uint32_t)VIRT_TO_PHYS(&kernel_end);

    uint32_t kernel_end_virt_aligned = (kernel_end_virt + _KMM_BLOCK_ALIGNMENT - 1) & ~(_KMM_BLOCK_ALIGNMENT - 1);
    memory_bitmap = (uint32_t*)kernel_end_virt_aligned;  //access through virtual
    uint32_t bitmap_phys = (uint32_t)VIRT_TO_PHYS(kernel_end_virt_aligned);
    
    for(uint32_t i = 0; i < bitmap_size; i++){
        memory_bitmap[i] = 0xFFFFFFFF;
    }
    used_frames = total_frames;
    uint32_t* entry_count_ptr = (uint32_t*)MEM_MAP_ENTRY_COUNT_LOC;
    uint32_t entry_count = *entry_count_ptr;
    e820_entry_t* memory_map = (e820_entry_t*)MEM_MAP_LOC;
    
    for(uint32_t i = 0; i < entry_count; i++){
        e820_entry_t* entry = &memory_map[i];
        if(entry->type == 1){
            kmm_setup_memory_region(entry->baseLow, entry->lengthLow, false);
        }
    }
    
    kmm_setup_memory_region(0, 0x100000, true);  //first 1MB
    
    uint32_t kernel_size = kernel_end_phys - kernel_start_phys;
    kmm_setup_memory_region(kernel_start_phys, kernel_size, true);  //kernel
    
    uint32_t bitmap_memory_size = bitmap_size * sizeof(uint32_t);
    bitmap_memory_size = (bitmap_memory_size + _KMM_BLOCK_SIZE - 1) & ~(_KMM_BLOCK_SIZE - 1);
    kmm_setup_memory_region(bitmap_phys, bitmap_memory_size, true);  //bitmap
}

void* kmm_frame_alloc(void){
    uint32_t frame = find_first_free();
    
    while(frame != (uint32_t)-1 && frame < 256){
        mark_frame(frame);
        used_frames++;
        frame = find_first_free();
    }
    
    if(frame == (uint32_t)-1){
        return NULL;
    }
    
    mark_frame(frame);
    used_frames++;
    return (void*)(frame * _KMM_BLOCK_SIZE);
}

void kmm_frame_free(void* phys_addr){
    if(phys_addr == NULL){
        return;
    }
    
    uint32_t addr = (uint32_t)phys_addr;
    uint32_t frame = addr / _KMM_BLOCK_SIZE;
    
    if(frame >= total_frames || frame < 256){
        return;
    }
    
    if(!isframe_used(frame)){
        return;  //frame wasn't allocated
    }
    
    //free
    unmark_frame(frame);
    used_frames--;
}

uint32_t kmm_get_total_frames(void){
    return total_frames;
}

uint32_t kmm_get_used_frames(void){
    return used_frames;
}