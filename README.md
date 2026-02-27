# x86 OS Kernel

A functional 32-bit operating system kernel built from scratch in C and x86 Assembly, running on emulated x86 hardware via QEMU. Implements the core subsystems of a Unix-style OS — from bare-metal hardware interaction up through a working shell.

> No starter code was used for the implementation. The project was built incrementally across four stages, each layer depending on the correctness of the one below it.

---

## Demo

> *(QEMU boot screen followed by shell prompt with supported commands)*


---

## Architecture Overview

The kernel is organized into four layers, built and tested in order:

```
[ Shell / User Interface    ]
[ System Call Interface     ]
[ Scheduler / Process Mgmt ]
[ Filesystem (HFS + VFS)   ]
[ Memory Management        ]
[ Hardware Drivers / IDT   ]
[ x86 Hardware (QEMU)      ]
```

---

## What I Built

### 1. Hardware Interaction Layer
**Files:** `init/isr.s`, `init/idt.c`, `init/interrupts.c`, `driver/vga.c`, `driver/kbd.c`, `driver/timer.c`

The foundation of the kernel. Implemented the Interrupt Descriptor Table (IDT) with 256 entries covering CPU exceptions (division by zero, page faults, general protection faults) and hardware IRQs. Each interrupt has an assembly wrapper in `isr.s` that saves the full CPU state onto the stack, constructing an `interrupt_context_t` trap frame before dispatching to the C handler.

- **VGA driver** — direct memory-mapped writes to `0xB8000`, hardware cursor control via CRTC I/O ports
- **PS/2 keyboard driver** — scancode capture via IRQ1, scancode-to-keycode translation, modifier key state tracking (Shift, Caps Lock, Ctrl, Alt), ring buffer for buffered input
- **PIT timer driver** — programmed the Intel 8253/8254 at a configurable frequency using the 1.193182 MHz base clock; drives the scheduler tick
- **Interrupt dispatcher** — generic `register_interrupt_handler()` / `unregister_interrupt_handler()` API so device drivers can hook into any IRQ without touching IDT setup

Getting interrupts right was harder than expected — early on, missing EOI acknowledgments to the PIC caused IRQs to fire once and then silently stop, which took a while to diagnose.

---

### 2. Memory Management
**Files:** `mm/kmm.c`, `mm/vmm.c`, `mm/kheap.c`

The most complex subsystem. Three components that build on each other:

#### Physical Memory Manager (KMM)
Bitmap-based frame allocator. Parses the BIOS E820 memory map at boot to determine usable RAM, initializes a bitmap where each bit represents one 4KB frame, and marks kernel code, the bitmap itself, and low memory (0–1MB) as reserved. Exposes `kmm_frame_alloc()` and `kmm_frame_free()`.

#### Virtual Memory Manager (VMM)
Two-level x86 paging (page directory → page table → physical frame). The virtual address space layout:

```
0x00000000 – 0x000FFFFF   Identity map (hardware access, VGA buffer)
0x00100000 – 0xBFFFFFFF   User space
0xC0000000 – 0xFFFFFFFF   Physmap (all physical RAM mapped here)
```

The physmap design means the kernel never needs temporary mappings to access physical frames — any physical address `P` is always accessible at virtual address `P + 0xC0000000`.

**This was the hardest part of the entire project.** The VMM went through several rounds of debugging:

- **Wrong address conversion** — early code was passing physical addresses directly to functions expecting virtual addresses (and vice versa), causing immediate page faults on the first memory access after enabling paging. The fix was being rigorous about `PHYS_TO_VIRT()` / `VIRT_TO_PHYS()` macros at every boundary.
- **TLB not flushed** — after remapping pages, the CPU was still using cached stale translations. Added `invlpg` calls after every page table modification.
- **Uncleared page directory** — newly allocated page directory frames contained garbage data that the MMU was interpreting as valid present entries, causing spurious page faults and in some cases triple faults that reset the CPU. Fixed by zeroing every newly allocated paging structure with `memset` before use.

Triple faults in particular are brutal to debug because QEMU just resets silently — figuring out that the cause was uncleared page directories took significant time with GDB attached to QEMU.

Also implemented `vmm_clone_pagedir()` for `fork()` semantics — shallow-copies kernel mappings (shared across all processes) and deep-copies user mappings (isolated per process).

#### Kernel Heap Allocator (KHEAP)
Buddy system allocator over a fixed virtual memory region. Splits blocks into power-of-two sizes on allocation and coalesces adjacent free buddies on deallocation. Exposes `kmalloc()`, `kfree()`, and `krealloc()`. Handles invalid and double frees safely via a magic number header on each allocation.

---

### 3. Process Management & Scheduling
**Files:** `proc/process.c`, `proc/tss.c`, `proc/elf.c`, `proc/syscall.c`

#### ELF Loader
Parses ELF32 executable headers, validates the magic number and architecture, iterates over `PT_LOAD` segments, allocates virtual pages, and copies segment data into the process address space. Zeroes the BSS region where `p_memsz > p_filesz`.

#### Task State Segment (TSS)
Maintains the kernel stack pointer (`esp0`) for ring-0/ring-3 privilege transitions. Updated on every context switch via `tss_update_esp0()` so that interrupts occurring in user mode always land on the correct kernel stack.

#### Scheduler
Preemptive round-robin scheduler driven by the PIT timer interrupt. Each thread gets a fixed time quantum; on expiry, `scheduler_tick()` saves the current trap frame, selects the next ready thread, and switches to its saved context by updating `ESP` to point at its trap frame and executing `iret`.

Context switching works by treating the saved `interrupt_context_t` on each thread's kernel stack as the restore point — switching threads is literally just changing which stack the CPU pops its registers from on `iret`.

Process/thread lifecycle: `READY → RUNNING → READY` (preempted) or `RUNNING → TERMINATED`. Supports `process_spawn()` (load ELF from VFS), `process_fork()` (clone address space via `vmm_clone_pagedir()`), and `process_exit()`.

---

### 4. Filesystem & VFS
**Files:** `fs/hfs.c`, `include/fs/hfs.h`

#### HFS (custom filesystem, modeled on ext4 structure)
On-disk layout:

```
[ Superblock | Block Bitmap | Inode Bitmap | Inode Blocks ... | Data Blocks ... ]
```

- **Superblock** — stores total block/inode counts, bitmap locations, inode table start, data block start, and a magic number for integrity checks
- **Block bitmap** — one bit per block, tracking free/used data blocks
- **Inode bitmap** — one bit per inode
- **Inodes** — store file size, directory flag, direct block pointers, and a single indirect pointer block for larger files
- **Directory blocks** — store filename-to-inode mappings for path resolution

Implemented the full VFS interface: `hfs_format()`, `hfs_mount()`, `hfs_unmount()`, `hfs_create()`, `hfs_mkdir()`, `hfs_remove()` (recursive), `hfs_open()`, `hfs_close()`, `hfs_read()`, `hfs_write()`. Integrated into a VFS layer that abstracts the underlying filesystem from the rest of the kernel, so the ELF loader and shell use the same `vfs_open()` / `vfs_read()` calls regardless of filesystem type.

---

### 5. Terminal & Shell
**Files:** `init/tty.c`, `init/shell.c`

TTY layer sits between the keyboard/VGA drivers and user-facing code — handles line buffering, echo, backspace, newline, auto-scroll when the cursor hits the bottom of the 80×25 VGA buffer. System call interface (`int 0x80`) routes `read` and `write` syscalls through the TTY.

Shell implements a simple command loop: read a line, tokenize, dispatch to a handler. Supported commands: `help`, `echo`, `clear`, `color`, `bgcolor`, `repeat`, `exit`.

---

## Build & Run

```bash
# Dependencies (Ubuntu/Debian)
sudo apt install build-essential gcc-multilib qemu-system-x86 gdb make

# Build
make clean && make

# Run in QEMU
make qemu

# Debug with GDB
make qemu-dbg
# then in a second terminal:
gdb -ex "target remote :1234"
```

---

## Technical Stack

| Component | Technology |
|---|---|
| Language | C (32-bit), x86 Assembly |
| Emulator | QEMU (qemu-system-i386) |
| Debugger | GDB with QEMU remote debugging |
| Build | GNU Make, gcc-multilib, binutils |
| Architecture | x86 (IA-32), protected mode |

---

## Challenges & Lessons

The VMM was by far the hardest component — not because the concepts are difficult, but because bugs manifest as silent triple faults or subtly wrong memory reads that only surface much later. The discipline of being explicit about every physical/virtual address boundary, flushing the TLB after every mapping change, and zeroing every newly allocated paging structure is not optional — any one of those missed will eventually cause a fault that's extremely hard to trace back to its origin.

GDB attached to QEMU (`make qemu-dbg`) was essential. Being able to set breakpoints inside the page fault handler, inspect CR2 for the faulting address, and walk the page directory manually made the difference between hours and days of debugging.

The other key lesson was building incrementally and testing each layer before moving to the next — the scheduler depends on the VMM being correct, the filesystem depends on the heap being correct, and so on. A bug in a lower layer will cause confusing failures in everything above it.
