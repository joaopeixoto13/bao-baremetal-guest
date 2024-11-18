/** 
 * Bao, a Lightweight Static Partitioning Hypervisor 
 *
 * Copyright (c) Bao Project (www.bao-project.org), 2019-
 *
 * Authors:
 *      João Peixoto <joaopeixotooficial@gmail.com>
 *
 * Bao is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free
 * Software Foundation, with a special exception exempting guest code from such
 * license. See the COPYING file in the top-level directory for details. 
 *
 */

#ifndef VIRTIO_MEMORY_POOL_H
#define VIRTIO_MEMORY_POOL_H

#include <stdint.h>

/**
 * @struct virtio_memory_pool 
 * @brief VirtIO memory pool used to allocate memory for the the VirtIO I/O buffers
 */
struct virtio_memory_pool {
    char* base;                 /**< Base address of the memory pool */
    unsigned long size;         /**< Size of the memory pool */
    unsigned long offset;       /**< Offset of the next available memory */
};

/**
 * @brief Initialize the VirtIO memory pool
 * @param pool VirtIO memory pool to initialize
 * @param base Base address of the memory pool
 * @param size Length of the memory to allocate
 */
static inline void virtio_memory_pool_init(struct virtio_memory_pool* pool, char* base, unsigned long size)
{
    pool->base = base;
    pool->size = size;
    pool->offset = 0;
}

/**
 * @brief Allocate memory from the VirtIO memory pool
 * @param pool VirtIO memory pool
 * @param alloc_size Size of the memory to allocate
 * @return Returns a pointer to the allocated memory, or NULL if the allocation failed
 */
static inline char* virtio_memory_pool_alloc(struct virtio_memory_pool* pool, unsigned long alloc_size) {
    /** Check if the requested allocation size is larger than the pool size */
    if (alloc_size > pool->size) {
        return NULL;
    }

    /** Check if there is enough space from the current offset to the end of the pool */
    if (pool->offset + alloc_size <= pool->size) {
        char *ptr = pool->base + pool->offset;
        pool->offset += alloc_size;
        return ptr;
    }

    /** If we reached the end of the pool, wrap around (circular buffer behavior) */
    if (alloc_size <= pool->offset) {
        pool->offset = 0;
        char *ptr = pool->base + pool->offset;
        pool->offset += alloc_size;
        return ptr;
    }

    /** No sufficient contiguous space available */
    return NULL;
}

#endif