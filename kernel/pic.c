#include "pic.h"
#include "io.h"

#define MASTER_CMD 0x20
#define MASTER_DAT 0x21
#define SLAVE_CMD  0xA0
#define SLAVE_DAT  0xA1

/* Remap the 8259 PICs so IRQ0..15 arrive as vectors 0x20..0x2F (out of the
 * way of the CPU exception vectors 0..31). */
void pic_init(void) {
    outb(MASTER_CMD, 0x11); io_wait();   /* start init, expect ICW4 */
    outb(SLAVE_CMD,  0x11); io_wait();
    outb(MASTER_DAT, 0x20); io_wait();   /* master vector offset 0x20 */
    outb(SLAVE_DAT,  0x28); io_wait();   /* slave  vector offset 0x28 */
    outb(MASTER_DAT, 0x04); io_wait();   /* slave is at IRQ2 */
    outb(SLAVE_DAT,  0x02); io_wait();
    outb(MASTER_DAT, 0x01); io_wait();   /* 8086 mode */
    outb(SLAVE_DAT,  0x01); io_wait();
    outb(MASTER_DAT, 0x00);              /* unmask all */
    outb(SLAVE_DAT,  0x00);
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(SLAVE_CMD, 0x20);
    outb(MASTER_CMD, 0x20);
}
