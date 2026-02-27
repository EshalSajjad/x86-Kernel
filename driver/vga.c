#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <utils.h>
#include <driver/vga.h>

//Combines foreground and background colors into a single color attribute byte and returns it.
uint8_t vga_entry_color (enum vga_color fg, enum vga_color bg){
    uint8_t color = bg << 4 | fg;
    return color;
}

//Combines the provided character to display, along with the color attribute byte, and returns a single 2-byte number. Note that vga entry t is a simple typedef for the type uint16 t (defined in vga.h).
vga_entry_t vga_entry (unsigned char uc, uint8_t color){
    vga_entry_t topbyte = color << 8;
    vga_entry_t bottombyte = uc;
    return topbyte | bottombyte;
}

//Moves the hardware cursor to the specified position on the VGA text mode screen.
void vga_move_cursor_to (uint8_t x, uint8_t y){
    uint16_t pos = y * VGA_WIDTH + x;
    
    outb(0x0F, 0x3D4);
    outb((uint8_t)(pos & 0xFF), 0x3D5);

    outb(0x0E, 0x3D4);
    outb((uint8_t)((pos >> 8) & 0xFF), 0x3D5);
}

//Puts a VGA entry at row x and column y. (Note that with this notation, x=0, y=0 refers to the top left corner on the screen).
void vga_putentry_at (vga_entry_t entry, uint8_t x, uint8_t y){
    vga_entry_t* screen = (vga_entry_t*) VGA_ADDRESS;
    screen[y * VGA_WIDTH + x] = entry;
}

//A helper function that returns a pointer to the start of the VGA frame buffer.
vga_entry_t* vga_get_screen_buffer (void){
    return (vga_entry_t*) VGA_ADDRESS;
}