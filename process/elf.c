#include <proc/elf.h>
#include <mm/kheap.h>

bool elf_check_hdr(elf_header_t* hdr){
    uint32_t* elf_magic_num = (uint32_t*)hdr->e_ident;
    if((*elf_magic_num != ELF_MAGIC) || (hdr->e_ident[4] != ELF_CLASS_32) || (hdr->e_ident[5] != ELF_DATA_LSB) || (hdr->e_ident[6] != ELF_VERSION_CURRENT) || 
    (hdr->e_type != ELF_TYPE_EXEC && hdr->e_type != ELF_TYPE_DYN) || (hdr->e_machine != ELF_MACHINE_X86)){
        return false;
    }

    return true;
}

int32_t elf_load_seg(file_t* file, pagedir_t* dir, elf_phdr_t* phdr){
    uintptr_t vaddr_start = phdr->p_vaddr & ~(VMM_PAGE_SIZE - 1);
    size_t total_size = (phdr->p_vaddr + phdr->p_memsz) - vaddr_start;
    
    uint32_t flags = PTE_PRESENT | PTE_USER;
    if(phdr->p_flags & ELF_PF_W){
        flags |= PTE_WRITABLE;
    }
    if(!vmm_alloc_region(dir, (void*)vaddr_start, total_size, flags)){
        return -1;
    }
    
    if(phdr->p_filesz > 0){
        file->f_offset = phdr->p_offset;
        int32_t bytes_read = vfs_read(file, (void*)phdr->p_vaddr, phdr->p_filesz);
        if(bytes_read != (int32_t)phdr->p_filesz){
            return -1;
        }
    }
    
    if(phdr->p_memsz > phdr->p_filesz){
        void* bss_start = (void*)(phdr->p_vaddr + phdr->p_filesz);
        size_t bss_size = phdr->p_memsz - phdr->p_filesz;
        memset(bss_start, 0, bss_size);
    }
    return 0;
}

int32_t elf_load(const char* path, pagedir_t* dir, void** entry_point){
    file_t* file = vfs_open(path, 0);
    if(file == NULL){
        return -1;
    }

    elf_header_t* e_hdr = malloc(sizeof(elf_header_t));
    if(!e_hdr){
        return -1;
    }
    
    file->f_offset = 0;
    int32_t read_bytes = vfs_read(file, e_hdr, sizeof(elf_header_t));
    if(read_bytes != (int32_t)sizeof(elf_header_t)){
        free(e_hdr);
        return -1;
    }

    bool is_hdr_valid = elf_check_hdr(e_hdr);
    if(!is_hdr_valid){
        free(e_hdr);
        return -1;
    }

    elf_phdr_t phdr;
    for(uint16_t i = 0; i < e_hdr->e_phnum; i++){
        file->f_offset = e_hdr->e_phoff + (i * sizeof(elf_phdr_t));
        read_bytes = vfs_read(file, &phdr, sizeof(elf_phdr_t));
        if(read_bytes != (int32_t)sizeof(elf_phdr_t)){
            free(e_hdr);
            return -1;
        }

        if(phdr.p_type != ELF_PT_LOAD){
            continue;
        }

        int32_t seg_load_status = elf_load_seg(file, dir, &phdr);
        if(seg_load_status != 0){
            free(e_hdr);
            return -1;
        }
    }
    
    *entry_point = (void*)e_hdr->e_entry;
    free(e_hdr);
    return 0;
}