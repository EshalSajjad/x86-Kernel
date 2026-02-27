#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../include/mm/vmm.h"
#include "../include/mm/kmm.h"
#include "../include/mem.h"
#include "../include/utils.h"
#include "../include/mm/kheap.h"
#include "../include/interrupts.h"

static pagedir_t* kernel_directory = NULL;
static pagedir_t* current_directory = NULL;

//helpers
static inline pde_t pde_create(void* phys_addr, uint32_t flags){
    return ((uint32_t)phys_addr & PDE_FRAME_MASK) | (flags & 0xFFF);
}

static inline pte_t pte_create(void* phys_addr, uint32_t flags){
    return ((uint32_t)phys_addr & PTE_FRAME_MASK) | (flags & 0xFFF);
}

pagedir_t* vmm_get_kerneldir(void){
    return kernel_directory;
}

pagedir_t* vmm_get_current_pagedir(void){
    return current_directory;
}

pagedir_t* vmm_create_address_space(void){
    //allocate physical frame for directory
    void* frame_phys = kmm_frame_alloc();
    if(!frame_phys){
        return NULL;
    }
    
    //access via physmap and initialize
    pagedir_t* dir = (pagedir_t*)PHYS_TO_VIRT(frame_phys);
    memset(dir, 0, sizeof(pagedir_t));
    return dir;
}

bool vmm_switch_pagedir(pagedir_t* new_pagedir){
    if(!new_pagedir){
        return false;
    }
    current_directory = new_pagedir;
    
    //load physical address into CR3
    uint32_t dir_phys = (uint32_t)(uintptr_t)VIRT_TO_PHYS(new_pagedir);
    asm volatile("mov %0, %%cr3" :: "r"(dir_phys) : "memory");
    return true;
}

void vmm_create_pt(pagedir_t* pdir, void* virtual, uint32_t flags){
    uint32_t pd_index = VMM_DIR_INDEX(virtual);
    
    //check if pt alr exists
    if(PDE_IS_PRESENT(pdir->table[pd_index])){
        return;
    }
    
    //allocate
    void* table_phys = kmm_frame_alloc();
    if(!table_phys){
        return;
    }
    
    //initialize via physmap
    pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT(table_phys);
    memset(table, 0, sizeof(pagetable_t));
    
    //set up pde
    uint32_t pde_flags = PDE_PRESENT;
    if (flags & PTE_WRITABLE) pde_flags |= PDE_WRITABLE;
    if (flags & PTE_USER) pde_flags |= PDE_USER;
    pdir->table[pd_index] = pde_create(table_phys, pde_flags);
}

void vmm_map_page(pagedir_t* pdir, void* virtual, void* physical, uint32_t flags){
    if(!pdir || !virtual){
        return;
    }
    
    vmm_create_pt(pdir, virtual, flags);
    
    //locate pt
    uint32_t pd_index = VMM_DIR_INDEX(virtual);
    uint32_t pt_index = VMM_TABLE_INDEX(virtual);
    
    pde_t directory_entry = pdir->table[pd_index];
    uint32_t table_phys = PDE_PTABLE_ADDR(directory_entry);
    pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT((void*)table_phys);
    
    //create and set pte
    pte_t table_entry = pte_create(physical, flags | PTE_PRESENT);
    table->table[pt_index] = table_entry;
}

void* vmm_get_phys_frame(pagedir_t* pdir, void* virtual){
    if(!pdir || !virtual){
        return NULL;
    }
    uint32_t pd_index = VMM_DIR_INDEX(virtual);
    uint32_t pt_index = VMM_TABLE_INDEX(virtual);
    pde_t directory_entry = pdir->table[pd_index];
    if(!PDE_IS_PRESENT(directory_entry)){
        return NULL;
    }
    
    uint32_t table_phys = PDE_PTABLE_ADDR(directory_entry);
    pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT((void*)table_phys);
    pte_t table_entry = table->table[pt_index];
    
    if(!PTE_IS_PRESENT(table_entry)){
        return NULL;
    }
    
    return (void*)(uintptr_t)PTE_FRAME_ADDR(table_entry);
}

void _vmm_page_fault_handler(interrupt_context_t* ctx){
    uintptr_t fault_address;
    asm volatile("mov %%cr2, %0" : "=r"(fault_address));
    for(;;){
        asm volatile("hlt");
    }
}

void vmm_init(void){
    register_interrupt_handler(14, _vmm_page_fault_handler);
    
    kernel_directory = vmm_create_address_space();
    if(!kernel_directory){
        for (;;) asm volatile("hlt");
    }
    
    //low 1MB
    for(uintptr_t va = IDENTITY_MAP_START; va < IDENTITY_MAP_END; va += VMM_PAGE_SIZE){
        vmm_map_page(kernel_directory, (void*)va, (void*)va, PTE_PRESENT | PTE_WRITABLE);
    }
    
    //phys mem to high virt addr
    uint32_t physical_memory = kmm_get_total_frames() * VMM_PAGE_SIZE;
    for(uintptr_t pa = 0; pa < physical_memory; pa += VMM_PAGE_SIZE){
        uintptr_t va = (uintptr_t)PHYS_TO_VIRT(pa);
        vmm_map_page(kernel_directory, (void*)va, (void*)pa, PTE_PRESENT | PTE_WRITABLE);
    }
    
    vmm_switch_pagedir(kernel_directory);
}

int32_t vmm_page_alloc(pte_t* pte, uint32_t flags){
    if(!pte){
        return -1;
    }
    if(PTE_IS_PRESENT(*pte)){
        return 0;
    }
    
    //get phys frame
    void* frame = kmm_frame_alloc();
    if(!frame){
        return -1;
    }
    *pte = pte_create(frame, flags | PTE_PRESENT);
    return 0;
}

void vmm_page_free(pte_t* pte){
    if(!pte || !PTE_IS_PRESENT(*pte)){
        return;
    }
    
    //free phys frame
    void* frame = (void*)(uintptr_t)PTE_FRAME_ADDR(*pte);
    kmm_frame_free(frame);
    *pte &= ~PTE_FRAME_MASK;
    PTE_UNSET_PRESENT(*pte);
}

bool vmm_alloc_region(pagedir_t* pdir, void* virtual, size_t size, uint32_t flags){
    if(!pdir || !virtual || size == 0){
        return false;
    }
    uintptr_t region_start = (uintptr_t)virtual;
    uintptr_t region_end = region_start + size;
    
    for(uintptr_t virt_addr = region_start; virt_addr < region_end; virt_addr += VMM_PAGE_SIZE){
        vmm_create_pt(pdir, (void*)virt_addr, flags);
        uint32_t pd_index = VMM_DIR_INDEX(virt_addr);
        if (!PDE_IS_PRESENT(pdir->table[pd_index])) {
            vmm_free_region(pdir, virtual, virt_addr - region_start);
            return false;
        }
        uint32_t table_phys = PDE_PTABLE_ADDR(pdir->table[pd_index]);
        pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT((void*)table_phys);
        uint32_t pt_index = VMM_TABLE_INDEX(virt_addr);
        pte_t* entry = &table->table[pt_index];
        
        //allocate frame
        if(vmm_page_alloc(entry, flags) != 0){
            vmm_free_region(pdir, virtual, virt_addr - region_start);
            return false;
        }
    }
    return true;
}

bool vmm_free_region(pagedir_t* pdir, void* virtual, size_t size){
    if(!pdir || !virtual || size == 0){
        return false;
    }
    uintptr_t region_start = (uintptr_t)virtual & ~(VMM_PAGE_SIZE - 1);
    uintptr_t region_end = ((uintptr_t)virtual + size + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
    
    for(uintptr_t virt_addr = region_start; virt_addr < region_end; virt_addr += VMM_PAGE_SIZE){
        uint32_t pd_index = VMM_DIR_INDEX(virt_addr);
        if(!PDE_IS_PRESENT(pdir->table[pd_index])){
            continue;
        }
        uint32_t table_phys = PDE_PTABLE_ADDR(pdir->table[pd_index]);
        pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT((void*)table_phys);
        uint32_t pt_index = VMM_TABLE_INDEX(virt_addr);
        
        if(PTE_IS_PRESENT(table->table[pt_index])){
            vmm_page_free(&table->table[pt_index]);
        }
        //invalid
        asm volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    }
    
    //check for empty pt and free
    uint32_t start_pd = VMM_DIR_INDEX(region_start);
    uint32_t end_pd = VMM_DIR_INDEX(region_end - 1);
    
    for(uint32_t pd_idx = start_pd; pd_idx <= end_pd; pd_idx++){
        if(!PDE_IS_PRESENT(pdir->table[pd_idx])){
            continue;
        }
        uint32_t table_phys = PDE_PTABLE_ADDR(pdir->table[pd_idx]);
        pagetable_t* table = (pagetable_t*)PHYS_TO_VIRT((void*)table_phys);
        
        //check if empty
        bool has_entries = false;
        for(uint32_t i = 0; i < VMM_PAGES_PER_TABLE; i++){
            if(PTE_IS_PRESENT(table->table[i])){
                has_entries = true;
                break;
            }
        }
        
        //free empty
        if(!has_entries){
            kmm_frame_free((void*)(uintptr_t)table_phys);
            pdir->table[pd_idx] = 0;
        }
    }
    return true;
}

pagetable_t* vmm_clone_pagetable(pagetable_t* src){
    if(!src){
        return NULL;
    }
    
    void* new_table_phys = kmm_frame_alloc();
    if(!new_table_phys){
        return NULL;
    }
    pagetable_t* new_table = (pagetable_t*)PHYS_TO_VIRT(new_table_phys);
    memset(new_table, 0, sizeof(pagetable_t));
    
    //deep copy each present entry
    for(uint32_t i = 0; i < VMM_PAGES_PER_TABLE; i++){
        pte_t source_entry = src->table[i];
        if(!PTE_IS_PRESENT(source_entry)){
            continue;
        }
        
        //allocate new frame for copied data
        void* new_frame_phys = kmm_frame_alloc();
        if(!new_frame_phys){
            //on fail
            for(uint32_t j = 0; j < i; j++){
                if(PTE_IS_PRESENT(new_table->table[j])){
                    void* frame = (void*)(uintptr_t)PTE_FRAME_ADDR(new_table->table[j]);
                    kmm_frame_free(frame);
                }
            }
            kmm_frame_free(new_table_phys);
            return NULL;
        }
        
        //copy
        void* source_data = PHYS_TO_VIRT((void*)(uintptr_t)PTE_FRAME_ADDR(source_entry));
        void* dest_data = PHYS_TO_VIRT(new_frame_phys);
        memcpy(dest_data, source_data, VMM_PAGE_SIZE);
        uint32_t entry_flags = source_entry & 0xFFF;
        new_table->table[i] = pte_create(new_frame_phys, entry_flags);
    }
    return new_table;
}

pagedir_t* vmm_clone_pagedir(void){
    if(!current_directory || !kernel_directory){
        return NULL;
    }
    
    pagedir_t* new_dir = vmm_create_address_space();
    if(!new_dir){
        return NULL;
    }
    //clone each dir entry
    for(uint32_t i = 0; i < VMM_PAGES_PER_DIR; i++){
        pde_t current_entry = current_directory->table[i];
        if(!PDE_IS_PRESENT(current_entry)){
            continue;
        }
        if(i >= 768){
            new_dir->table[i] = current_entry;
            continue;
        }
        
        //check if kernel mapping -> shallow copy
        pde_t kernel_entry = kernel_directory->table[i];
        bool is_kernel_mapping = PDE_IS_PRESENT(kernel_entry) && (PDE_PTABLE_ADDR(kernel_entry) == PDE_PTABLE_ADDR(current_entry));
        
        if(is_kernel_mapping){ //shallow
            new_dir->table[i] = current_entry;
        }
        else{ //deep
            uint32_t source_table_phys = PDE_PTABLE_ADDR(current_entry);
            pagetable_t* source_table = (pagetable_t*)PHYS_TO_VIRT((void*)source_table_phys);
            pagetable_t* cloned_table = vmm_clone_pagetable(source_table);
            if(!cloned_table){
                for(uint32_t j = 0; j < i; j++){
                    if(PDE_IS_PRESENT(new_dir->table[j])){
                        void* table_phys = (void*)(uintptr_t)PDE_PTABLE_ADDR(new_dir->table[j]);
                        kmm_frame_free(table_phys);
                    }
                }
                kmm_frame_free(VIRT_TO_PHYS(new_dir));
                return NULL;
            }
            
            //new pde w same flags
            uint32_t dir_flags = current_entry & 0xFFF;
            void* cloned_table_phys = VIRT_TO_PHYS(cloned_table);
            new_dir->table[i] = pde_create(cloned_table_phys, dir_flags);
        }
    }
    return new_dir;
}