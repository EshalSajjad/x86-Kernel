#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <utils.h>
// #define LOG_MOD_NAME 	"PRC"
// #define LOG_MOD_ENABLE  1
// #include <log.h>

#include <string.h>
#include <proc/process.h>
#include <proc/tss.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <init/gdt.h>
#include <mem.h>

#define KSTACK_SIZE (2 * VMM_PAGE_SIZE)
#define DEFAULT_TIMESLICE 10
// #define DEFAULT_TIMESLICE 100
#define USER_STACK_TOP 0xC0000000

static process_t *current_proc = NULL;
static thread_t *current_thread = NULL;
static uint32_t next_pid = 1;
static uint32_t next_tid = 1;

static thread_t *ready_queue_head = NULL;
static thread_t *ready_queue_tail = NULL;
static process_t *process_list = NULL;

static volatile uint32_t debug_tick_count = 0;

// HELPERSS
static uint32_t alloc_pid(void){ 
    return next_pid++; 
}
static uint32_t alloc_tid(void){ 
    return next_tid++; 
}
static void add_to_process_list(process_t *proc){
    proc->next = process_list;
    process_list = proc;
}
static void remove_from_process_list(process_t *proc){
    process_t **prev = &process_list;
    while(*prev){
        if(*prev == proc){
            *prev = proc->next;
            break;
        }
        prev = &(*prev)->next;
    }
}
static void remove_thread_from_process(thread_t *thread){
    if(!thread || !thread->proc){
        return;
    }
    
    thread_t **prev = &thread->proc->thread_list;
    while(*prev){
        if(*prev == thread){
            *prev = thread->next;
            break;
        }
        prev = &(*prev)->next;
    }
}
static void remove_from_ready_queue(thread_t *thread){
    if(!thread){
        return;
    }
    
    if(ready_queue_head == thread){
        ready_queue_head = thread->next;
        if(ready_queue_tail == thread){
            ready_queue_tail = NULL;
        }
    }
    else{
        thread_t *curr = ready_queue_head;
        while(curr && curr->next != thread){
            curr = curr->next;
        }
        if(curr){
            curr->next = thread->next;
            if(ready_queue_tail == thread){
                ready_queue_tail = curr;
            }
        }
    }
    thread->next = NULL;
}

// PROCESSES
void process_create(process_t* process, const char* name, int32_t priority){
    if(!process){
        return;
    }
    memset(process, 0, sizeof(process_t));
    
    process->pid = alloc_pid();
    if(name){
        strncpy(process->name, name, sizeof(process->name) - 1);
    }
    process->priority = priority;
    process->exit_code = 0;
    process->page_dir = NULL;
    process->main_thread = NULL;
    process->thread_list = NULL;
    process->next = NULL;
    add_to_process_list(process);
}

void process_destroy(process_t* process){
    if(!process){
        return;
    }
    if(process == current_proc){
        return;
    }
    remove_from_process_list(process);
    
    thread_t *thread = process->thread_list;
    while(thread){
        thread_t *next = thread->next;
        thread_destroy(thread);
        thread = next;
    }
    
    if(process->page_dir && process->page_dir != vmm_get_kerneldir()){
        pagedir_t *current_dir = vmm_get_current_pagedir();
        if(current_dir == process->page_dir){
            vmm_switch_pagedir(vmm_get_kerneldir());
        }
        process->page_dir = NULL;
    }
    memset(process, 0, sizeof(process_t));
}

int32_t process_spawn(const char* filename){
    if(!filename){
        return -1;
    }
    heap_t *heap = get_kernel_heap();
    if(!heap){
        return -1;
    }
    process_t *proc = kmalloc(heap, sizeof(process_t));
    if(!proc){
        return -1;
    }
    process_create(proc, filename, 0);
    
    proc->page_dir = vmm_create_address_space();
    if(!proc->page_dir){
        kfree(heap, proc);
        return -1;
    }
    
    void *entry_point;
    int32_t result = elf_load(filename, proc->page_dir, &entry_point);
    if(result < 0 || !entry_point){
        process_destroy(proc);
        kfree(heap, proc);
        return result;
    }
    
    thread_t *main_thread = thread_create(proc, entry_point, NULL);
    if(!main_thread){
        process_destroy(proc);
        kfree(heap, proc);
        return -1;
    }
    proc->main_thread = main_thread;
    scheduler_post(main_thread);
    return (int32_t)proc->pid;
}

int32_t process_fork(void){
    if(!current_proc || !current_thread){
        return -1;
    }
    heap_t *heap = get_kernel_heap();
    if(!heap){
        return -1;
    }
    process_t *child = kmalloc(heap, sizeof(process_t));
    if(!child){
        return -1;
    }
    
    char child_name[32];
    strncpy(child_name, current_proc->name, 25);
    child_name[25] = '\0';
    strcpy(child_name + strlen(child_name), "_child");
    process_create(child, child_name, current_proc->priority);
    
    child->page_dir = vmm_clone_pagedir();
    if(!child->page_dir){
        kfree(heap, child);
        return -1;
    }
    thread_t *child_thread = kmalloc(heap, sizeof(thread_t));
    if(!child_thread){
        process_destroy(child);
        kfree(heap, child);
        return -1;
    }
    memcpy(child_thread, current_thread, sizeof(thread_t));
    
    child_thread->kstack = kmalloc(heap, KSTACK_SIZE);
    if(!child_thread->kstack){
        kfree(heap, child_thread);
        process_destroy(child);
        kfree(heap, child);
        return -1;
    }
    
    child_thread->kstack_size = KSTACK_SIZE;
    child_thread->kstack_top = (void*)((uintptr_t)child_thread->kstack + KSTACK_SIZE);
    interrupt_context_t *child_frame = (interrupt_context_t*)((uintptr_t)child_thread->kstack_top - sizeof(interrupt_context_t));
    memcpy(child_frame, current_thread->trap_frame, sizeof(interrupt_context_t));
    child_thread->trap_frame = child_frame;
    child_frame->eax = 0;
    current_thread->trap_frame->eax = child->pid;
    child_thread->tid = alloc_tid();
    child_thread->proc = child;
    child_thread->state = THREAD_READY;
    child_thread->timeslice = DEFAULT_TIMESLICE;
    child_thread->next = NULL;
    child_thread->next = child->thread_list;
    child->thread_list = child_thread;
    child->main_thread = child_thread;
    
    scheduler_post(child_thread);
    return (int32_t)child->pid;
}

process_t* process_find_by_pid(uint32_t pid){
    process_t *proc = process_list;
    while(proc){
        if(proc->pid == pid){
            return proc;
        }
        proc = proc->next;
    }
    return NULL;
}

void process_exit(process_t* process, int32_t status){
    if(!process){
        return;
    }
    process->exit_code = status;
    thread_t *thread = process->thread_list;
    while(thread){
        thread->state = THREAD_TERMINATED;
        thread = thread->next;
    }
    
    if(process == current_proc){
        current_thread->state = THREAD_TERMINATED;
        asm volatile("int $0x20");
    }
    else{
        process_destroy(process);
        heap_t *heap = get_kernel_heap();
        if(heap){
            kfree(heap, process);
        }
    }
}

thread_t* get_main_thread(process_t* process){
    if(!process){
        return NULL;
    }
    if(!process->main_thread){
        process->main_thread = thread_create(process, NULL, NULL);
    }
    return process->main_thread;
}

thread_t* _get_main_thread(process_t* process){
    return get_main_thread(process);
}

// THREADS
thread_t* thread_create(process_t* parent_process, void* entry, void* arg){
    if(!parent_process){
        return NULL;
    }
    heap_t *heap = get_kernel_heap();
    if(!heap){
        return NULL;
    }
    thread_t *thread = kmalloc(heap, sizeof(thread_t));
    if(!thread){
        return NULL;
    }
    memset(thread, 0, sizeof(thread_t));
    thread->kstack = kmalloc(heap, KSTACK_SIZE);
    if(!thread->kstack){
        kfree(heap, thread);
        return NULL;
    }

    thread->tid = alloc_tid();
    thread->proc = parent_process;
    thread->state = THREAD_READY;
    thread->priority = parent_process->priority;
    thread->timeslice = DEFAULT_TIMESLICE;
    thread->kstack_size = KSTACK_SIZE;
    thread->kstack_top = (void*)((uintptr_t)thread->kstack + KSTACK_SIZE);

    //trapframe at top of kernel stack
    interrupt_context_t *frame = (interrupt_context_t*)((uintptr_t)thread->kstack_top - sizeof(interrupt_context_t));
    memset(frame, 0, sizeof(interrupt_context_t));

    bool is_user = (parent_process->page_dir && parent_process->page_dir != vmm_get_kerneldir());
    if(is_user){
        frame->cs = (GDT_USER_CODE_ENTRY * 8) | 3;
        frame->ds = (GDT_USER_DATA_ENTRY * 8) | 3;
        frame->ss = (GDT_USER_DATA_ENTRY * 8) | 3;
        //userstack in userspace, top at USER_STACK_TOP
        frame->useresp = USER_STACK_TOP;
    }
    else{
        //kernel thread
        frame->cs = (GDT_KERNEL_CODE_ENTRY * 8);
        frame->ds = (GDT_KERNEL_DATA_ENTRY * 8);
        frame->ss = (GDT_KERNEL_DATA_ENTRY * 8);
        frame->useresp = (uint32_t)thread->kstack_top;
    }

    frame->eip = entry ? (uint32_t)entry : (uint32_t)0;
    frame->eflags = 0x202;
    frame->eax = (uint32_t)arg;
    frame->ebp = 0;
    frame->esp = (uint32_t)&frame->ebx;
    thread->trap_frame = frame;
    thread->next = parent_process->thread_list;
    parent_process->thread_list = thread;

    if(!parent_process->main_thread){
        parent_process->main_thread = thread;
    }
    return thread;
}

int32_t thread_destroy(thread_t* thread){
    if(!thread){
        return -1;
    }
    if(thread == current_thread){
        return -1;
    }
    heap_t *heap = get_kernel_heap();
    if(!heap){
        return -1;
    }
    remove_from_ready_queue(thread);
    remove_thread_from_process(thread);
    
    if(thread->proc && thread->proc->main_thread == thread){
        thread->proc->main_thread = NULL;
    }
    if(thread->kstack){
        kfree(heap, thread->kstack);
    }
    kfree(heap, thread);
    return 0;
}

// SCHEDULER
void scheduler_init(void){
    ready_queue_head = NULL;
    ready_queue_tail = NULL;
    process_list = NULL;
    
    heap_t *heap = get_kernel_heap();
    if(!heap){
        return;
    }
    process_t *init_proc = kmalloc(heap, sizeof(process_t));
    if(!init_proc){
        return;
    }
    process_create(init_proc, "init", 0);
    init_proc->page_dir = vmm_get_kerneldir();
    
    thread_t *init_thread = kmalloc(heap, sizeof(thread_t));
    if(!init_thread){
        kfree(heap, init_proc);
        return;
    }
    memset(init_thread, 0, sizeof(thread_t));
    init_thread->tid = alloc_tid();
    init_thread->proc = init_proc;
    init_thread->state = THREAD_RUNNING;
    init_thread->priority = 0;
    init_thread->timeslice = DEFAULT_TIMESLICE;
    init_thread->kstack = kmalloc(heap, KSTACK_SIZE);
    if(!init_thread->kstack){
        kfree(heap, init_thread);
        kfree(heap, init_proc);
        return;
    }
    
    init_thread->kstack_size = KSTACK_SIZE;
    init_thread->kstack_top = (void*)((uintptr_t)init_thread->kstack + KSTACK_SIZE);
    init_thread->trap_frame = NULL;

    init_proc->main_thread = init_thread;
    init_proc->thread_list = init_thread;
    
    current_proc = init_proc;
    current_thread = init_thread;
    
    tss_update_esp0((uint32_t)init_thread->kstack_top);
}

void scheduler_tick(interrupt_context_t* context){

    // LOG_P("TICK: current_tid=%u state=%d ts=%d", current_thread->tid, current_thread->state, current_thread->timeslice);
    debug_tick_count++;

    if(!current_thread){
        return;
    }
    
    // LOG_P("TICK: current_tid=%u state=%d ts=%d", current_thread->tid, current_thread->state, current_thread->timeslice);

    if(current_thread->state == THREAD_RUNNING){
        // LOG_P("TICK: Thread %u TERMINATED, switching...", current_thread->tid);
        current_thread->trap_frame = context;
    }
    
    if(current_thread->state == THREAD_TERMINATED){
        thread_t *dead = current_thread;
        process_t *dead_proc = dead->proc;
        
        if(!ready_queue_head){
            while(1){ 
                asm volatile("hlt");
            }
        }
        
        thread_t *next_thread = ready_queue_head;
        ready_queue_head = next_thread->next;
        if(!ready_queue_head){
            ready_queue_tail = NULL;
        }
        next_thread->next = NULL;
        next_thread->state = THREAD_RUNNING;
        next_thread->timeslice = DEFAULT_TIMESLICE;
        
        remove_thread_from_process(dead);
        
        if(dead_proc && !dead_proc->thread_list){
            process_destroy(dead_proc);
            heap_t *heap = get_kernel_heap();
            if(heap){
                kfree(heap, dead_proc);
            }
        }
        scheduler_switch(next_thread);
        return;
    }
    current_thread->timeslice--;

    /////debugdebugdebug
    // if(current_thread->timeslice == 0){
    //     LOG_P("TICK: Thread %u timeslice EXPIRED (ready_queue=%p)", current_thread->tid, ready_queue_head);
    // }
    
    if(current_thread->timeslice > 0 && current_thread->state == THREAD_RUNNING){
        return;
    }
    if(!ready_queue_head){
        current_thread->timeslice = DEFAULT_TIMESLICE;
        return;
    }
    // debugdebugdebug/////
    if(current_thread->state == THREAD_RUNNING){
        // LOG_P("TICK: Moving thread %u to ready queue", current_thread->tid);
        current_thread->state = THREAD_READY;
        scheduler_post(current_thread);
    }

    thread_t *next_thread = ready_queue_head;
    ready_queue_head = next_thread->next;
    if(!ready_queue_head){
        ready_queue_tail = NULL;
    }
    next_thread->next = NULL;
    next_thread->state = THREAD_RUNNING;
    next_thread->timeslice = DEFAULT_TIMESLICE;
    
    // LOG_P("TICK: About to switch to tid=%u", next_thread->tid);
    scheduler_switch(next_thread);
}

void scheduler_switch(thread_t* next_thread){

    if(!next_thread || next_thread == current_thread){
        // LOG_P("SWITCH: Aborted (same thread or null)");
        return;
    }
    
    thread_t *old_thread = current_thread;
    current_thread = next_thread;
    current_proc = next_thread->proc;
    
    if(old_thread->proc != next_thread->proc){
        if(next_thread->proc->page_dir){
            vmm_switch_pagedir(next_thread->proc->page_dir);
            // LOG_P("SWITCH: Changed page directory");
        }
    }
    tss_update_esp0((uint32_t)next_thread->kstack_top);

    //debugdebugdebug
    // interrupt_context_t *frame = next_thread->trap_frame;
    // if(!frame){
    //     LOG_P("ERROR: NULL trap_frame for tid=%u", next_thread->tid);
    //     while(1){
    //         asm volatile("hlt");
    //     }
    // }

    // LOG_P("SWITCH: About to iret to eip=0x%x", frame->eip);

    asm volatile(
        "movl %0, %%esp\n\t"
        "popl %%ds\n\t"
        "popa\n\t"
        "addl $8, %%esp\n\t"
        "iret\n\t"
        :
        : "r" (next_thread->trap_frame)
        : "memory"
    );
}

void scheduler_post(thread_t* thread){
    // LOG_P("POST: tid=%u state_before=%d", thread->tid, thread->state);
    if (!thread){
        return;
    }
    cli();
    thread->state = THREAD_READY;
    thread->next = NULL;

    if(!ready_queue_head){
        ready_queue_head = thread;
        ready_queue_tail = thread;
    }
    else{
        ready_queue_tail->next = thread;
        ready_queue_tail = thread;
    }
    sti();
}

process_t* get_current_proc(void){
    return current_proc;
}

thread_t* get_current_thread(void){
    return current_thread;
}
// debugdebugdebug
uint32_t get_debug_tick_count(void){
    return debug_tick_count;
}