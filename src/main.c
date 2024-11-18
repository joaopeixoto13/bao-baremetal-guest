/** 
 * Bao, a Lightweight Static Partitioning Hypervisor 
 *
 * Copyright (c) Bao Project (www.bao-project.org), 2019-
 *
 * Authors:
 *      Jose Martins <jose.martins@bao-project.org>
 *      Sandro Pinto <sandro.pinto@bao-project.org>
 *
 * Bao is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free
 * Software Foundation, with a special exception exempting guest code from such
 * license. See the COPYING file in the top-level directory for details. 
 *
 */

#include <core.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h> 
#include <cpu.h>
#include <wfi.h>
#include <spinlock.h>
#include <plat.h>
#include <irq.h>
#include <uart.h>
#include <timer.h>

#define TIMER_INTERVAL (TIME_S(1))

spinlock_t print_lock = SPINLOCK_INITVAL;

#ifdef DEMO_VIRTIO

#include <virtio_console.h>

#define VIRTIO_CONSOLE_RX_IRQ_ID (52)

/*
* The shared memory region must be within the range of the bare-metal RAM region.
* In other words, must be within PLAT_MEM_BASE and PLAT_MEM_BASE + PLAT_MEM_SIZE.
* This stems from the fact that the shared memory region is used by both the bare-metal
* and the Linux backend guest, and for that reason the memory region msut be cache-coherent.
* 
* Example:
* For the ZCU102 platform, the bare-metal RAM region is from 0x20000000 to 0x28000000.
* However, we could define a shared memory region from 0x0 to 0x80000000 (2GB) if we
* compile this guest with STD_ADDR_SPACE=y.
*/
static char* const shmem_base = (char*)0x50000000;
static const long mmio_base = 0xa003e00;

static struct virtio_console console;

void virtio_console_rx_handler() {
    char* msg = virtio_console_receive(&console);
    if (msg != NULL) {
        printf("Bare-metal received a new message: %s\n", msg);
    }
}

void virtio_init(void)
{
    spin_lock(&print_lock);
    printf("Initializing virtio console ...\n");
    spin_unlock(&print_lock);

    if (!virtio_console_init(&console, shmem_base, mmio_base)) {
        spin_lock(&print_lock);
        printf("virtio console initialization failed!\n");
        spin_unlock(&print_lock);
        while(1) wfi();
    } else {
        spin_lock(&print_lock);
        printf("virtio console initialized\n");
        spin_unlock(&print_lock);
    }

    irq_set_handler(VIRTIO_CONSOLE_RX_IRQ_ID, virtio_console_rx_handler);
    irq_set_prio(VIRTIO_CONSOLE_RX_IRQ_ID, IRQ_MAX_PRIO);
    irq_enable(VIRTIO_CONSOLE_RX_IRQ_ID);
}

#endif

#ifdef DEMO_IPC

#define SHMEM_IRQ_ID (52)

char* const baremetal_message = (char*)0x70000000;
char* const zephyr_message    = (char*)0x70002000;
const size_t shmem_channel_size = 0x2000;

void shmem_update_msg(int irq_count) {
    sprintf(baremetal_message, "Bao baremetal guest received %d uart interrupts!\n",
        irq_count);
}

char* strnchr(const char* s, size_t n, char c) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) {
            return (char*)s + i;
        }
    }
    return NULL;
}

void shmem_handler() {
    zephyr_message[shmem_channel_size-1] = '\0';
    char* end = strchr(zephyr_message, '\n');
    if (end != NULL) {
        *end = '\0';
    }
    printf("message from zephyr: %s\n", zephyr_message);
}

void shmem_init() {
    memset(baremetal_message, 0, shmem_channel_size);
    memset(zephyr_message, 0, shmem_channel_size);
    shmem_update_msg(0);
    irq_set_handler(SHMEM_IRQ_ID, shmem_handler);
    irq_set_prio(SHMEM_IRQ_ID, IRQ_MAX_PRIO);
    irq_enable(SHMEM_IRQ_ID);
}

#endif

void uart_rx_handler(){
    static int irq_count = 0;
    printf("cpu%d: %s %d\n",get_cpuid(), __func__, ++irq_count);
    uart_clear_rxirq();
#ifdef DEMO_IPC
        shmem_update_msg(irq_count);
#endif
}

void ipi_handler(){
    printf("cpu%d: %s\n", get_cpuid(), __func__);
    irq_send_ipi(1ull << (get_cpuid() + 1));
}

void timer_handler(){
#ifdef DEMO_VIRTIO
    virtio_console_transmit(&console, "Hello from the bare-metal guest, Bao!\r\n");
#else
    printf("cpu%d: %s\n", get_cpuid(), __func__);
#endif    
    timer_set(TIMER_INTERVAL);
    irq_send_ipi(1ull << (get_cpuid() + 1));
}

void main(void){

    static volatile bool master_done = false;

    if(cpu_is_master()){
        spin_lock(&print_lock);
        printf("Bao bare-metal test guest\n");
        spin_unlock(&print_lock);

#ifdef DEMO_VIRTIO
        virtio_init();
#endif

        irq_set_handler(UART_IRQ_ID, uart_rx_handler);
        irq_set_handler(TIMER_IRQ_ID, timer_handler);
        irq_set_handler(IPI_IRQ_ID, ipi_handler);

        uart_enable_rxirq();

        timer_set(TIMER_INTERVAL);
        irq_enable(TIMER_IRQ_ID);
        irq_set_prio(TIMER_IRQ_ID, IRQ_MAX_PRIO);

#ifdef DEMO_IPC
        shmem_init();
#endif

        master_done = true;
    }

    irq_enable(UART_IRQ_ID);
    irq_set_prio(UART_IRQ_ID, IRQ_MAX_PRIO);
    irq_enable(IPI_IRQ_ID);
    irq_set_prio(IPI_IRQ_ID, IRQ_MAX_PRIO);

    while(!master_done);
    spin_lock(&print_lock);
    printf("cpu %d up\n", get_cpuid());
    spin_unlock(&print_lock);

    while(1) wfi();
}
