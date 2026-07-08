#pragma once
#include <stdint.h>
void  kheap_init(void);
void *kmalloc(uint64_t size);
void  kfree(void *p);
void  kheap_usage(uint64_t *used, uint64_t *total);   /* live payload / managed bytes */
