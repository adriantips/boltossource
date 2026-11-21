#pragma once
#include <stdint.h>
void *kmalloc(uint64_t n);
void kfree(void *p);
