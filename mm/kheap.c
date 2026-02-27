#include <mm/kheap.h>
#include <mm/vmm.h>
#include <string.h>
// #include <log.h>

#define BUDDY_MIN_ORDER 5
#define BUDDY_MAX_ORDER 20
#define BUDDY_MAGIC 0xDEADBEEF

typedef struct{
    uint32_t size;
    uint32_t magic;
} alloc_block_hdr;

typedef struct _free_block_hdr{
    struct _free_block_hdr* next;
    struct _free_block_hdr* prev;
} free_block_hdr;

typedef struct{
    uintptr_t base;
    size_t size;
    uint32_t min_order;
    uint32_t max_order;
    free_block_hdr* free_lists[BUDDY_MAX_ORDER + 1];
} buddy_state_t;

heap_t kernel_heap;

//helpers
static inline uint32_t log2_floor(size_t val){
    uint32_t order = 0;
    while(val > 1){
        val >>= 1;
        order++;
    }
    return order;
}

static inline size_t order_to_size(uint32_t order){
    return 1UL << order;
}

static inline uintptr_t get_buddy_addr(uintptr_t base, uintptr_t block, size_t size){
    return base + ((block - base) ^ size);
}

static void list_push(free_block_hdr** head, free_block_hdr* node){
    node->next = *head;
    node->prev = NULL;
    if(*head){
        (*head)->prev = node;
    }
    *head = node;
}

static void list_remove(free_block_hdr** head, free_block_hdr* node){
    if(node->prev){
        node->prev->next = node->next;
    }
    else{
        *head = node->next;
    }
    if(node->next){
        node->next->prev = node->prev;
    }
}

static free_block_hdr* list_pop(free_block_hdr** head){
    free_block_hdr* node = *head;
    if(node){
        *head = node->next;
        if(*head){
            (*head)->prev = NULL;
        }
    }
    return node;
}

static void buddy_init(buddy_state_t* state, uintptr_t base, size_t size, uint32_t min_order, uint32_t max_order){
    state->base = base;
    state->size = size;
    state->min_order = min_order;
    state->max_order = max_order;
    
    for(uint32_t i = 0; i <= BUDDY_MAX_ORDER; i++){
        state->free_lists[i] = NULL;
    }
    free_block_hdr* initial_block = (free_block_hdr*)base;
    initial_block->next = NULL;
    initial_block->prev = NULL;
    state->free_lists[max_order] = initial_block;
}

void kheap_init(heap_t *heap, void *start, size_t size, size_t max_size, bool is_supervisor, bool is_readonly){
    //align heap start to page boundary
    uintptr_t aligned_start = ((uintptr_t)start + 0xFFF) & ~0xFFF;
    
    //align to minimum block size
    size_t min_block_size = order_to_size(BUDDY_MIN_ORDER);
    aligned_start = (aligned_start + min_block_size - 1) & ~(min_block_size - 1);
    
    //size to multiple of minimum block
    size_t usable_size = size - (aligned_start - (uintptr_t)start);
    usable_size = (usable_size / min_block_size) * min_block_size;
    
    //initialize heap desc
    heap->start = aligned_start;
    heap->end = aligned_start + usable_size;
    heap->max_size = usable_size;
    heap->is_supervisor = is_supervisor;
    heap->is_readonly = is_readonly;
    
    //allocate buddy state at the start of the heap
    buddy_state_t* state = (buddy_state_t*)aligned_start;
    aligned_start += sizeof(buddy_state_t);
    usable_size -= sizeof(buddy_state_t);
    
    //realign
    aligned_start = (aligned_start + min_block_size - 1) & ~(min_block_size - 1);
    
    //calc max order
    uint32_t max_order = log2_floor(usable_size);
    if(max_order > BUDDY_MAX_ORDER){
        max_order = BUDDY_MAX_ORDER;
    }
    
    //initialize buddy allocator
    buddy_init(state, aligned_start, usable_size, BUDDY_MIN_ORDER, max_order);
    heap->state = state;
    
    //map the virt mem region
    pagedir_t* pdir = vmm_get_kerneldir();
    uint32_t flags = PTE_PRESENT | PTE_WRITABLE;
    if(!is_supervisor){
        flags |= PTE_USER;
    }

    if(!vmm_alloc_region(pdir, (void*)heap->start, heap->max_size, flags)){
        // LOG_ERROR("Failed to map heap region at 0x%x", heap->start);
        return;
    }    
    // LOG_DEBUG("Heap initialized: start=0x%x, size=%u KB", aligned_start, usable_size / 1024);
}

void* kmalloc(heap_t *heap, size_t size){
    if(!heap || size == 0){
        return NULL;
    }
    buddy_state_t* state = (buddy_state_t*)heap->state;
    
    //total req size (header + data + alignment)
    size_t total_size = size + sizeof(alloc_block_hdr);
    total_size = (total_size + 7) & ~7;
    
    //determine req order
    uint32_t order = state->min_order;
    while(order_to_size(order) < total_size && order <= state->max_order){
        order++;
    }
    if(order > state->max_order){
        return NULL;
    }
    
    //find first free
    uint32_t current_order = order;
    while(current_order <= state->max_order && !state->free_lists[current_order]){
        current_order++;
    }
    if(current_order > state->max_order){
        return NULL;
    }
    
    //split larger blocks to req order
    while(current_order > order){
        free_block_hdr* block = list_pop(&state->free_lists[current_order]);
        current_order--;
        size_t half_size = order_to_size(current_order);
        uintptr_t first_half = (uintptr_t)block;
        uintptr_t second_half = first_half + half_size;
        //add both halves to the smaller free list
        list_push(&state->free_lists[current_order], (free_block_hdr*)first_half);
        list_push(&state->free_lists[current_order], (free_block_hdr*)second_half);
    }
    
    //allocate
    free_block_hdr* block = list_pop(&state->free_lists[order]);
    alloc_block_hdr* hdr = (alloc_block_hdr*)block;
    hdr->size = order_to_size(order);
    hdr->magic = BUDDY_MAGIC;
    
    return (void*)((uintptr_t)block + sizeof(alloc_block_hdr));
}

void kfree(heap_t *heap, void *ptr){
    if(!heap || !ptr){
        return;
    }
    buddy_state_t* state = (buddy_state_t*)heap->state;
    
    //retrieve allocation header
    uintptr_t hdr_addr = (uintptr_t)ptr - sizeof(alloc_block_hdr);
    alloc_block_hdr* hdr = (alloc_block_hdr*)hdr_addr;
    
    //validate magic no
    if(hdr->magic != BUDDY_MAGIC){
        // LOG_ERROR("Invalid free: bad magic at 0x%x", ptr);
        return;
    }
    size_t block_size = hdr->size;
    uint32_t order = log2_floor(block_size);
    
    //invalidate magic to detect double-free
    hdr->magic = 0;
    uintptr_t block_addr = hdr_addr;
    while(order < state->max_order){
        uintptr_t buddy_addr = get_buddy_addr(state->base, block_addr, block_size);
        //check if buddy is free
        free_block_hdr* buddy = NULL;
        for(free_block_hdr* node = state->free_lists[order]; node; node = node->next){
            if((uintptr_t)node == buddy_addr){
                buddy = node;
                break;
            }
        }
        if(!buddy){
            break;
        }
        
        //remove buddy from free list
        list_remove(&state->free_lists[order], buddy);
        
        //merge blocks
        if(buddy_addr < block_addr){
            block_addr = buddy_addr;
        }
        order++;
        block_size *= 2;
    }
    
    //insert merged into free
    free_block_hdr* free_block = (free_block_hdr*)block_addr;
    free_block->next = NULL;
    free_block->prev = NULL;
    list_push(&state->free_lists[order], free_block);
}

void* krealloc(heap_t *heap, void *ptr, size_t new_size){
    if(!ptr){
        return kmalloc(heap, new_size);
    }
    if(new_size == 0){
        kfree(heap, ptr);
        return NULL;
    }
    //retrieve
    uintptr_t hdr_addr = (uintptr_t)ptr - sizeof(alloc_block_hdr);
    alloc_block_hdr* hdr = (alloc_block_hdr*)hdr_addr;
    
    //validate
    if(hdr->magic != BUDDY_MAGIC){
        return NULL;
    }
    
    //check if current block can fit new size
    size_t required_size = new_size + sizeof(alloc_block_hdr);
    if(required_size <= hdr->size){
        return ptr; //reuse existing
    }
    
    //allocate larger block
    void* new_ptr = kmalloc(heap, new_size);
    if(!new_ptr){
        return NULL;
    }
    
    //copy old
    size_t copy_size = hdr->size - sizeof(alloc_block_hdr);
    if(copy_size > new_size){
        copy_size = new_size;
    }
    memcpy(new_ptr, ptr, copy_size);
    
    kfree(heap, ptr);
    return new_ptr;
}

void kheap_stats(heap_t *heap){
    if(!heap){
        return;
    }
    buddy_state_t* state = (buddy_state_t*)heap->state;
    
    // LOG_DEBUG("Heap Statistics: Base: 0x%x, Size: %u KB", state->base, state->size / 1024);
    // LOG_DEBUG("Orders: %u - %u", state->min_order, state->max_order);
    
    for(uint32_t order = state->min_order; order <= state->max_order; order++){
        uint32_t count = 0;
        for(free_block_hdr* node = state->free_lists[order]; node; node = node->next){
            count++;
        }
        if(count > 0){
            // LOG_DEBUG("Order %u (%u bytes): %u free blocks", order, order_to_size(order), count);
        }
    }
}

//public wrappers
heap_t* get_kernel_heap(void){
    return &kernel_heap;
}

void* malloc(size_t size){
    return kmalloc(&kernel_heap, size);
}

void free(void *ptr){
    kfree(&kernel_heap, ptr);
}

void* realloc(void *ptr, size_t new_size){
    return krealloc(&kernel_heap, ptr, new_size);
}