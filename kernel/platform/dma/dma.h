#ifndef ARX_PLATFORM_DMA_H
#define ARX_PLATFORM_DMA_H

#include <pci/pci.h>
#include <memory/pmm.h>

typedef enum platform_dma_status
{
    DMA_SUCCESS,
    DMA_INVALID_ARGUMENT,
    DMA_UNSUPPORTED,
    DMA_NO_MEMORY,
    DMA_MAPPING_FAILED,
    DMA_INVALID_ALLOCATION,
    DMA_BUSY
} platform_dma_status_t;

typedef struct dma_address
{
    uint64_t value;
} dma_address_t;

typedef struct dma_device
{
    const pci_device_t* pciDevice;
    uint64_t            addressMask;
    size_t              minimumAlignment;
    size_t              boundary;
} dma_device_t;

typedef struct platform_dma_caps
{
    bool        coherent;
    phys_addr_t physicalAddressMask;
} platform_dma_caps_t;

typedef struct platform_dma_mapping
{
    dma_address_t address;
    void*         cookie;
} platform_dma_mapping_t;

typedef struct platform_dma_ops
{
    void* context;
    platform_dma_status_t (*query)(void* context, const dma_device_t* device, platform_dma_caps_t* caps);
    // Failure leaves no mapping; success must satisfy every supplied constraint.
    platform_dma_status_t (*map)(void* context, const dma_device_t* device, phys_addr_t physical, size_t size, size_t alignment, size_t boundary, platform_dma_mapping_t* mapping);
    // Failure retains the mapping. Success makes the backing safe to release.
    platform_dma_status_t (*unmap)(void* context, const dma_device_t* device, const platform_dma_mapping_t* mapping, size_t size);
    void (*writeBarrier)(void* context);
    void (*readBarrier)(void* context);
} platform_dma_ops_t;

#ifdef __cplusplus
extern "C"
{
#endif
    bool dma_range_valid(dma_address_t address, size_t size, uint64_t mask, size_t alignment, size_t boundary);
    void platform_dma_init(void);
#ifdef __cplusplus
}
#endif

#endif
