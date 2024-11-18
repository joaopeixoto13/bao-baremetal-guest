#include <virtio_console.h>
#include <virtio_mmio.h>
#include <virtio_queue.h>
#include <stdio.h>
#include <string.h>

bool virtio_console_init(struct virtio_console *console, char *shmem_base, long mmio_base)
{
    bool ret = false;

    console->ready = false;
    console->device_id = VIRTIO_CONSOLE_DEVICE_ID;
    console->mmio = (volatile struct virtio_mmio_reg *)mmio_base;
    console->negotiated_feature_bits = 0;
    
    /* Initialize the receive and transmit virtqueues */
    virtq_init(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX], VIRTIO_CONSOLE_RX_VQ_IDX, shmem_base);
    virtq_init(&console->vqs[VIRTIO_CONSOLE_TX_VQ_IDX], VIRTIO_CONSOLE_TX_VQ_IDX, shmem_base + VIRTQ_SIZE_TOTAL);
    
    /* Add buffers to the receive queue */
    while (virtq_has_free_slots(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX]))
    {
        /* Get the next free descriptor index */
        uint16_t desc_id = virtq_get_free_desc_id(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX]);

        /* Get the descriptor */
        volatile struct virtq_desc *desc = virtq_get_desc_by_id(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX], desc_id);
        
        /* Allocate memory for the I/O buffer from the memory pool */
        char *const io_buffer = virtio_memory_pool_alloc(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX].pool, VIRTIO_CONSOLE_RX_BUFFER_SIZE);
        if(io_buffer == NULL) {
            break;
        }
        
        /* Initialize the descriptor */
        virtq_desc_init(desc, (uint64_t)io_buffer, VIRTIO_CONSOLE_RX_BUFFER_SIZE);

        /* Set the descriptor as write only (VirtIO spec says "The driver MUST NOT put a device-readable buffer in a receiveq") */
        virtq_desc_set_write_only(desc);

        /* Add the buffer to the available ring */
        virtq_add_avail_buf(&console->vqs[VIRTIO_CONSOLE_RX_VQ_IDX], desc_id);
    }

    /* Initialize the VirtIO MMIO transport */
    ret = virtio_console_mmio_init(console);

    /* Set the device as ready */
    console->ready = true;

    return ret;
}

bool virtio_console_mmio_init(struct virtio_console *console)
{
    if (console->mmio->MagicValue != VIRTIO_MAGIC_VALUE)
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register magic value mismatch\n");
        return false;
    }

    if (console->mmio->Version != VIRTIO_VERSION_NO_LEGACY)
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register version mismatch\n");
        return false;
    }

    if (console->mmio->DeviceID != console->device_id)
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register device ID mismatch\n");
        return false;
    }

    console->mmio->Status = RESET;
    console->mmio->Status |= ACKNOWLEDGE;
    console->mmio->Status |= DRIVER;

    if (console->mmio->Status != (RESET | ACKNOWLEDGE | DRIVER))
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register status mismatch\n");
        return false;
    }

    for (int i = 0; i < VIRTIO_MMIO_FEATURE_SEL_SIZE; i++)
    {
        console->mmio->DeviceFeaturesSel = i;
        console->mmio->DriverFeaturesSel = i;
        uint64_t acked_features = console->mmio->DeviceFeatures & (VIRTIO_CONSOLE_FEATURES >> (i * 32));
        console->mmio->DriverFeatures = acked_features;
        console->negotiated_feature_bits |= (acked_features << (i * 32));
    }

    if (console->negotiated_feature_bits != VIRTIO_CONSOLE_FEATURES)
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register feature mismatch\n");
        return false;
    }

    console->config_space.cols = console->mmio->Config & 0xFFFF;
    console->config_space.rows = (console->mmio->Config >> 16) & 0xFFFF;
    console->config_space.max_nr_ports = *((volatile uint32_t *)((uintptr_t)&console->mmio->Config + 0x4));
    console->config_space.emerg_wr = *((volatile uint32_t *)((uintptr_t)&console->mmio->Config + 0x8));

    console->mmio->Status |= FEATURES_OK;

    if (console->mmio->Status != (RESET | ACKNOWLEDGE | DRIVER | FEATURES_OK))
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register status mismatch\n");
        return false;
    }

    for (int vq_id = 0; vq_id < VIRTIO_CONSOLE_NUM_VQS; vq_id++)
    {
        console->mmio->QueueSel = vq_id;
        if (console->mmio->QueueReady != 0)
        {
            console->mmio->Status |= FAILED;
            printf("VirtIO MMIO register queue ready mismatch\n");
            return false;
        }

        int queue_num_max = console->mmio->QueueNumMax;

        if (queue_num_max == 0)
        {
            console->mmio->Status |= FAILED;
            printf("VirtIO MMIO register queue number max mismatch\n");
            return false;
        }

        console->mmio->QueueDescLow = (uint32_t)((uint64_t)console->vqs[vq_id].desc & 0xFFFFFFFF);
        console->mmio->QueueDescHigh = (uint32_t)(((uint64_t)console->vqs[vq_id].desc >> 32) & 0xFFFFFFFF);
        console->mmio->QueueDriverLow = (uint32_t)((uint64_t)console->vqs[vq_id].avail & 0xFFFFFFFF);
        console->mmio->QueueDriverHigh = (uint32_t)(((uint64_t)console->vqs[vq_id].avail >> 32) & 0xFFFFFFFF);
        console->mmio->QueueDeviceLow= (uint32_t)((uint64_t)console->vqs[vq_id].used & 0xFFFFFFFF);
        console->mmio->QueueDeviceHigh = (uint32_t)(((uint64_t)console->vqs[vq_id].used >> 32) & 0xFFFFFFFF);

        console->mmio->QueueReady = 1;
    }

    console->mmio->Status |= DRIVER_OK;
    if (console->mmio->Status != (RESET | ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK))
    {
        console->mmio->Status |= FAILED;
        printf("VirtIO MMIO register status mismatch\n");
        return false;
    }

    return true;
}

void virtio_console_transmit(struct virtio_console *console, char *const data)
{
    int data_len = strlen(data);

    if (!console->ready) {
        printf("VirtIO console device is not ready\n");
        return;
    }

    if (data == NULL || data_len == 0) {
        printf("No data to transmit\n");
        return;
    }

    /* Extract the queue that will be used (transmit virtqueue) */
    struct virtq* vq = &console->vqs[VIRTIO_CONSOLE_TX_VQ_IDX];

    /* Get the next free descriptor index */
    uint16_t desc_id = virtq_get_free_desc_id(vq);

    /* Get the descriptor */
    volatile struct virtq_desc *desc = virtq_get_desc_by_id(vq, desc_id);

    /* Allocate memory for the I/O buffer from the memory pool */
    char *const io_buffer = virtio_memory_pool_alloc(&vq->pool, data_len);
    if(io_buffer == NULL) {
        printf("Failed to allocate memory for I/O buffer\n");
        return;
    }

    /* Copy the data to the I/O buffer */
    strcpy(io_buffer, data);

    /* Initialize the descriptor */
    virtq_desc_init(desc, (uint64_t)io_buffer, data_len);

    /* Set the descriptor as read only (VirtIO spec says "The driver MUST NOT put a device-writable buffer in a transmitq") */
    virtq_desc_set_read_only(desc);

    /* Add the buffer to the available ring */
    virtq_add_avail_buf(vq, desc_id);

    /* Notify the backend device */
    virtio_mmio_queue_notify(console->mmio, vq->queue_index);
}

char* virtio_console_receive(struct virtio_console *console)
{
    uint32_t interrupt_status = 0;
    char* received_msg = NULL;

    if (!console->ready) {
        printf("VirtIO console device is not ready\n");
        return NULL;
    }

    /* Read and acknowledge interrupts */
    interrupt_status = console->mmio->InterruptStatus;
    console->mmio->InterruptACK = interrupt_status;

    if (interrupt_status & VIRTIO_MMIO_INT_CONFIG) {
        printf("We not supoort configuration space change notifications!\n");
        return NULL;
    }

    /* Check if there are used buffers for all the virtqueues */
    for (int vq_id = 0; vq_id < VIRTIO_CONSOLE_NUM_VQS; vq_id++)
    {
        /* Extract the queue that will to test */
        struct virtq* vq = &console->vqs[vq_id];

        if (!virtq_used_has_buf(vq)) {
            continue;
        }

        /* Check if there are used buffers on the receive queue */
        while (virtq_used_has_buf(vq))
        {
            /* Get the descriptor ID that the next used ring points to */
            uint16_t desc_id = virtq_get_used_buf_id(vq);

            /* Get the descriptor */
            volatile struct virtq_desc *desc = virtq_get_desc_by_id(vq, desc_id);

            /* Get the buffer address */
            char *const io_buffer = (char *)desc->addr;

            /* Put the buffer back into the free list */
            virtq_put_free_desc(vq, desc_id);

            if (vq_id == VIRTIO_CONSOLE_RX_VQ_IDX) {
                char* msg = (char*)desc->addr;

                /* Append the msg to the output buffer */
                if (received_msg == NULL) {
                    received_msg = msg;
                } else {
                    strcat(received_msg, msg);
                }
            } else if (vq_id == VIRTIO_CONSOLE_TX_VQ_IDX) {
                /**
                 * Here we simply continue because we are not interested in the transmit queue.
                 * In fact, the backend device signals the driver when it has processed the
                 * transmit buffers (making them used) to signal the driver that it can reuse
                 * the buffers (put them back into the free list).
                 */
                continue;
            }
        }
    }

    return received_msg;
}