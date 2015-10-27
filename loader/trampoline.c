/*
 * Vulkan
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "vk_loader_platform.h"
#include "loader.h"
#include "debug_report.h"
#include "wsi_swapchain.h"


/* Trampoline entrypoints */
LOADER_EXPORT VkResult VKAPI vkCreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance)
{
    struct loader_instance *ptr_instance = NULL;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

    loader_platform_thread_once(&once_init, loader_initialize);

    if (pAllocator) {
        ptr_instance = (struct loader_instance *) pAllocator->pfnAllocation(
                           pAllocator->pUserData,
                           sizeof(struct loader_instance),
                           sizeof(VkInstance),
                           VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    } else {
        ptr_instance = (struct loader_instance *) malloc(sizeof(struct loader_instance));
    }
    if (ptr_instance == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    tls_instance = ptr_instance;
    loader_platform_thread_lock_mutex(&loader_lock);
    memset(ptr_instance, 0, sizeof(struct loader_instance));

    if (pAllocator) {
        ptr_instance->alloc_callbacks = *pAllocator;
    }

    /* Due to implicit layers need to get layer list even if
     * enabledLayerNameCount == 0 and VK_INSTANCE_LAYERS is unset. For now always
     * get layer list (both instance and device) via loader_layer_scan(). */
    memset(&ptr_instance->instance_layer_list, 0, sizeof(ptr_instance->instance_layer_list));
    memset(&ptr_instance->device_layer_list, 0, sizeof(ptr_instance->device_layer_list));
    loader_layer_scan(ptr_instance,


                      &ptr_instance->instance_layer_list,
                      &ptr_instance->device_layer_list);

    /* validate the app requested layers to be enabled */
    if (pCreateInfo->enabledLayerNameCount > 0) {
        res = loader_validate_layers(pCreateInfo->enabledLayerNameCount,
                                     pCreateInfo->ppEnabledLayerNames,
                                     &ptr_instance->instance_layer_list);
        if (res != VK_SUCCESS) {
            loader_platform_thread_unlock_mutex(&loader_lock);
            return res;
        }
    }

    /* Scan/discover all ICD libraries */
    memset(&ptr_instance->icd_libs, 0, sizeof(ptr_instance->icd_libs));
    loader_icd_scan(ptr_instance, &ptr_instance->icd_libs);

    /* get extensions from all ICD's, merge so no duplicates, then validate */
    loader_get_icd_loader_instance_extensions(ptr_instance,
                                              &ptr_instance->icd_libs,
                                              &ptr_instance->ext_list);
    res = loader_validate_instance_extensions(&ptr_instance->ext_list,
                                              &ptr_instance->instance_layer_list,
                                              pCreateInfo);
    if (res != VK_SUCCESS) {
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->device_layer_list);
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->instance_layer_list);
        loader_scanned_icd_clear(ptr_instance, &ptr_instance->icd_libs);
        loader_destroy_ext_list(ptr_instance, &ptr_instance->ext_list);
        loader_platform_thread_unlock_mutex(&loader_lock);
        loader_heap_free(ptr_instance, ptr_instance);
        return res;
    }

    ptr_instance->disp = loader_heap_alloc(
                             ptr_instance,
                             sizeof(VkLayerInstanceDispatchTable),
                             VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (ptr_instance->disp == NULL) {
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->device_layer_list);
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->instance_layer_list);
        loader_scanned_icd_clear(ptr_instance,
                                 &ptr_instance->icd_libs);
        loader_destroy_ext_list(ptr_instance,
                                &ptr_instance->ext_list);
        loader_platform_thread_unlock_mutex(&loader_lock);
        loader_heap_free(ptr_instance, ptr_instance);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(ptr_instance->disp, &instance_disp, sizeof(instance_disp));
    ptr_instance->next = loader.instances;
    loader.instances = ptr_instance;

    /* activate any layers on instance chain */
    res = loader_enable_instance_layers(ptr_instance,
                                        pCreateInfo,
                                        &ptr_instance->instance_layer_list);
    if (res != VK_SUCCESS) {
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->device_layer_list);
        loader_delete_layer_properties(ptr_instance,
                                       &ptr_instance->instance_layer_list);
        loader_scanned_icd_clear(ptr_instance,
                                 &ptr_instance->icd_libs);
        loader_destroy_ext_list(ptr_instance,
                                &ptr_instance->ext_list);
        loader.instances = ptr_instance->next;
        loader_platform_thread_unlock_mutex(&loader_lock);
        loader_heap_free(ptr_instance, ptr_instance->disp);
        loader_heap_free(ptr_instance, ptr_instance);
        return res;
    }
    loader_activate_instance_layers(ptr_instance);

    wsi_swapchain_create_instance(ptr_instance, pCreateInfo);
    debug_report_create_instance(ptr_instance, pCreateInfo);


    *pInstance = (VkInstance) ptr_instance;

    res = ptr_instance->disp->CreateInstance(pCreateInfo, pAllocator, pInstance);

    /*
     * Finally have the layers in place and everyone has seen
     * the CreateInstance command go by. This allows the layer's
     * GetInstanceProcAddr functions to return valid extension functions
     * if enabled.
     */
    loader_activate_instance_layer_extensions(ptr_instance);

    loader_platform_thread_unlock_mutex(&loader_lock);
    return res;
}

LOADER_EXPORT void VKAPI vkDestroyInstance(
                                            VkInstance instance,
                                            const VkAllocationCallbacks* pAllocator)
{
    const VkLayerInstanceDispatchTable *disp;
    struct loader_instance *ptr_instance = NULL;
    disp = loader_get_instance_dispatch(instance);

    loader_platform_thread_lock_mutex(&loader_lock);

    ptr_instance = loader_get_instance(instance);
    disp->DestroyInstance(instance, pAllocator);

    loader_deactivate_instance_layers(ptr_instance);
    loader_heap_free(ptr_instance, ptr_instance->disp);
    loader_heap_free(ptr_instance, ptr_instance);
    loader_platform_thread_unlock_mutex(&loader_lock);
}

LOADER_EXPORT VkResult VKAPI vkEnumeratePhysicalDevices(
                                            VkInstance instance,
                                            uint32_t* pPhysicalDeviceCount,
                                            VkPhysicalDevice* pPhysicalDevices)
{
    const VkLayerInstanceDispatchTable *disp;
    VkResult res;
    disp = loader_get_instance_dispatch(instance);

    loader_platform_thread_lock_mutex(&loader_lock);
    res = disp->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount,
                                         pPhysicalDevices);
    loader_platform_thread_unlock_mutex(&loader_lock);
    return res;
}






LOADER_EXPORT void VKAPI vkGetPhysicalDeviceFeatures(
                                            VkPhysicalDevice gpu,
                                            VkPhysicalDeviceFeatures *pFeatures)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(gpu);
    disp->GetPhysicalDeviceFeatures(gpu, pFeatures);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceFormatProperties(
                                            VkPhysicalDevice gpu,
                                            VkFormat format,
                                            VkFormatProperties *pFormatInfo)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(gpu);
    disp->GetPhysicalDeviceFormatProperties(gpu, format, pFormatInfo);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(physicalDevice);
    disp->GetPhysicalDeviceImageFormatProperties(physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceProperties(
                                            VkPhysicalDevice gpu,
                                            VkPhysicalDeviceProperties* pProperties)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(gpu);
    disp->GetPhysicalDeviceProperties(gpu, pProperties);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceQueueFamilyProperties(
                                            VkPhysicalDevice gpu,
                                            uint32_t* pQueueFamilyPropertyCount,
                                            VkQueueFamilyProperties* pQueueProperties)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(gpu);
    disp->GetPhysicalDeviceQueueFamilyProperties(gpu, pQueueFamilyPropertyCount, pQueueProperties);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceMemoryProperties(
                                            VkPhysicalDevice gpu,
                                            VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(gpu);
    disp->GetPhysicalDeviceMemoryProperties(gpu, pMemoryProperties);
}

LOADER_EXPORT VkResult VKAPI vkCreateDevice(
        VkPhysicalDevice gpu,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice)
{
    VkResult res;

    loader_platform_thread_lock_mutex(&loader_lock);

    res = loader_CreateDevice(gpu, pCreateInfo, pAllocator, pDevice);

    loader_platform_thread_unlock_mutex(&loader_lock);
    return res;
}

LOADER_EXPORT void VKAPI vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;
    struct loader_device *dev;
    struct loader_icd *icd = loader_get_icd_and_device(device, &dev);
    const struct loader_instance *inst = icd->this_instance;
    disp = loader_get_dispatch(device);

    loader_platform_thread_lock_mutex(&loader_lock);
    disp->DestroyDevice(device, pAllocator);
    loader_remove_logical_device(inst, device);
    loader_platform_thread_unlock_mutex(&loader_lock);
}

LOADER_EXPORT VkResult VKAPI vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    VkResult res;

    loader_platform_thread_lock_mutex(&loader_lock);
    //TODO convert over to using instance chain dispatch
    res = loader_EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
    loader_platform_thread_unlock_mutex(&loader_lock);
    return res;
}

LOADER_EXPORT VkResult VKAPI vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pPropertyCount,
    VkLayerProperties*                          pProperties)
{
    VkResult res;

    loader_platform_thread_lock_mutex(&loader_lock);
    //TODO convert over to using instance chain dispatch
    res = loader_EnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
    loader_platform_thread_unlock_mutex(&loader_lock);
    return res;
}

LOADER_EXPORT void VKAPI vkGetDeviceQueue(VkDevice device, uint32_t queueNodeIndex, uint32_t queueIndex, VkQueue* pQueue)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetDeviceQueue(device, queueNodeIndex, queueIndex, pQueue);
    loader_set_dispatch(*pQueue, disp);
}

LOADER_EXPORT VkResult VKAPI vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(queue);

    return disp->QueueSubmit(queue, submitCount, pSubmits, fence);
}

LOADER_EXPORT VkResult VKAPI vkQueueWaitIdle(VkQueue queue)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(queue);

    return disp->QueueWaitIdle(queue);
}

LOADER_EXPORT VkResult VKAPI vkDeviceWaitIdle(VkDevice device)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->DeviceWaitIdle(device);
}

LOADER_EXPORT VkResult VKAPI vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
}

LOADER_EXPORT void VKAPI vkFreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->FreeMemory(device, mem, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkMapMemory(VkDevice device, VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void** ppData)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->MapMemory(device, mem, offset, size, flags, ppData);
}

LOADER_EXPORT void VKAPI vkUnmapMemory(VkDevice device, VkDeviceMemory mem)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->UnmapMemory(device, mem);
}

LOADER_EXPORT VkResult VKAPI vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->FlushMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);
}

LOADER_EXPORT VkResult VKAPI vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->InvalidateMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);
}

LOADER_EXPORT void VKAPI vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize* pCommittedMemoryInBytes)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetDeviceMemoryCommitment(device, memory, pCommittedMemoryInBytes);
}

LOADER_EXPORT VkResult VKAPI vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory mem, VkDeviceSize offset)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->BindBufferMemory(device, buffer, mem, offset);
}

LOADER_EXPORT VkResult VKAPI vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory mem, VkDeviceSize offset)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->BindImageMemory(device, image, mem, offset);
}

LOADER_EXPORT void VKAPI vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetBufferMemoryRequirements(device, buffer, pMemoryRequirements);
}

LOADER_EXPORT void VKAPI vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetImageMemoryRequirements(device, image, pMemoryRequirements);
}

LOADER_EXPORT void VKAPI vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements* pSparseMemoryRequirements)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetImageSparseMemoryRequirements(device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

LOADER_EXPORT void VKAPI vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, uint32_t samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t* pPropertyCount, VkSparseImageFormatProperties* pProperties)
{
    const VkLayerInstanceDispatchTable *disp;

    disp = loader_get_instance_dispatch(physicalDevice);

    disp->GetPhysicalDeviceSparseImageFormatProperties(physicalDevice, format, type, samples, usage, tiling, pPropertyCount, pProperties);
}

LOADER_EXPORT VkResult VKAPI vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence fence)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(queue);

    return disp->QueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
}

LOADER_EXPORT VkResult VKAPI vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateFence(device, pCreateInfo, pAllocator, pFence);
}

LOADER_EXPORT void VKAPI vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyFence(device, fence, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->ResetFences(device, fenceCount, pFences);
}

LOADER_EXPORT VkResult VKAPI vkGetFenceStatus(VkDevice device, VkFence fence)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->GetFenceStatus(device, fence);
}

LOADER_EXPORT VkResult VKAPI vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->WaitForFences(device, fenceCount, pFences, waitAll, timeout);
}

LOADER_EXPORT VkResult VKAPI vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
}

LOADER_EXPORT void VKAPI vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroySemaphore(device, semaphore, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateEvent(VkDevice device, const VkEventCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkEvent* pEvent)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateEvent(device, pCreateInfo, pAllocator, pEvent);
}

LOADER_EXPORT void VKAPI vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyEvent(device, event, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkGetEventStatus(VkDevice device, VkEvent event)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->GetEventStatus(device, event);
}

LOADER_EXPORT VkResult VKAPI vkSetEvent(VkDevice device, VkEvent event)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->SetEvent(device, event);
}

LOADER_EXPORT VkResult VKAPI vkResetEvent(VkDevice device, VkEvent event)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->ResetEvent(device, event);
}

LOADER_EXPORT VkResult VKAPI vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkQueryPool* pQueryPool)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
}

LOADER_EXPORT void VKAPI vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyQueryPool(device, queryPool, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t startQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->GetQueryPoolResults(device, queryPool, startQuery, queryCount, dataSize, pData, stride, flags);
}

LOADER_EXPORT VkResult VKAPI vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
}

LOADER_EXPORT void VKAPI vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyBuffer(device, buffer, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBufferView* pView)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateBufferView(device, pCreateInfo, pAllocator, pView);
}

LOADER_EXPORT void VKAPI vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyBufferView(device, bufferView, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateImage(device, pCreateInfo, pAllocator, pImage);
}

LOADER_EXPORT void VKAPI vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyImage(device, image, pAllocator);
}

LOADER_EXPORT void VKAPI vkGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource* pSubresource, VkSubresourceLayout* pLayout)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetImageSubresourceLayout(device, image, pSubresource, pLayout);
}

LOADER_EXPORT VkResult VKAPI vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateImageView(device, pCreateInfo, pAllocator, pView);
}

LOADER_EXPORT void VKAPI vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyImageView(device, imageView, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShader)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateShaderModule(device, pCreateInfo, pAllocator, pShader);
}

LOADER_EXPORT void VKAPI vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyShaderModule(device, shaderModule, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateShader(VkDevice device, const VkShaderCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShader* pShader)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateShader(device, pCreateInfo, pAllocator, pShader);
}

LOADER_EXPORT void VKAPI vkDestroyShader(VkDevice device, VkShader shader, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyShader(device, shader, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineCache* pPipelineCache)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreatePipelineCache(device, pCreateInfo, pAllocator, pPipelineCache);
}

LOADER_EXPORT void VKAPI vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyPipelineCache(device, pipelineCache, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->GetPipelineCacheData(device, pipelineCache, pDataSize, pData);
}

LOADER_EXPORT VkResult VKAPI vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->MergePipelineCaches(device, dstCache, srcCacheCount, pSrcCaches);
}

LOADER_EXPORT VkResult VKAPI vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

LOADER_EXPORT VkResult VKAPI vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

LOADER_EXPORT void VKAPI vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyPipeline(device, pipeline, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
}

LOADER_EXPORT void VKAPI vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyPipelineLayout(device, pipelineLayout, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateSampler(device, pCreateInfo, pAllocator, pSampler);
}

LOADER_EXPORT void VKAPI vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroySampler(device, sampler, pAllocator);
}


LOADER_EXPORT VkResult VKAPI vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
}

LOADER_EXPORT void VKAPI vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
}

LOADER_EXPORT void VKAPI vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyDescriptorPool(device, descriptorPool, pAllocator);
}


LOADER_EXPORT VkResult VKAPI vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->ResetDescriptorPool(device, descriptorPool, flags);
}

LOADER_EXPORT VkResult VKAPI vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->AllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
}

LOADER_EXPORT VkResult VKAPI vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->FreeDescriptorSets(device, descriptorPool, descriptorSetCount, pDescriptorSets);
}

LOADER_EXPORT void VKAPI vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->UpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

LOADER_EXPORT VkResult VKAPI vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
}

LOADER_EXPORT void VKAPI vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyFramebuffer(device, framebuffer, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
}

LOADER_EXPORT void VKAPI vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyRenderPass(device, renderPass, pAllocator);
}

LOADER_EXPORT void VKAPI vkGetRenderAreaGranularity(VkDevice device, VkRenderPass renderPass, VkExtent2D* pGranularity)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->GetRenderAreaGranularity(device, renderPass, pGranularity);
}

LOADER_EXPORT VkResult VKAPI vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->CreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
}

LOADER_EXPORT void VKAPI vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->DestroyCommandPool(device, commandPool, pAllocator);
}

LOADER_EXPORT VkResult VKAPI vkResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    return disp->ResetCommandPool(device, commandPool, flags);
}

LOADER_EXPORT VkResult VKAPI vkAllocateCommandBuffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkCommandBuffer* pCommandBuffers)
{
    const VkLayerDispatchTable *disp;
    VkResult res;

    disp = loader_get_dispatch(device);

    res = disp->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (res == VK_SUCCESS) {
        for (uint32_t i =0; i < pAllocateInfo->bufferCount; i++) {
            if (pCommandBuffers[i]) {
                loader_init_dispatch(pCommandBuffers[i], disp);
            }
        }
    }

    return res;
}

LOADER_EXPORT void VKAPI vkFreeCommandBuffers(
        VkDevice                                device,
        VkCommandPool                               commandPool,
        uint32_t                                commandBufferCount,
        const VkCommandBuffer*                      pCommandBuffers)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(device);

    disp->FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

LOADER_EXPORT VkResult VKAPI vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    return disp->BeginCommandBuffer(commandBuffer, pBeginInfo);
}

LOADER_EXPORT VkResult VKAPI vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    return disp->EndCommandBuffer(commandBuffer);
}

LOADER_EXPORT VkResult VKAPI vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    return disp->ResetCommandBuffer(commandBuffer, flags);
}

LOADER_EXPORT void VKAPI vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

LOADER_EXPORT void VKAPI vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport* pViewports)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetViewport(commandBuffer, viewportCount, pViewports);
}

LOADER_EXPORT void VKAPI vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D* pScissors)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetScissor(commandBuffer, scissorCount, pScissors);
}

LOADER_EXPORT void VKAPI vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetLineWidth(commandBuffer, lineWidth);
}

LOADER_EXPORT void VKAPI vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetDepthBias(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

LOADER_EXPORT void VKAPI vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetBlendConstants(commandBuffer, blendConstants);
}

LOADER_EXPORT void VKAPI vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
}

LOADER_EXPORT void VKAPI vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t stencilCompareMask)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetStencilCompareMask(commandBuffer, faceMask, stencilCompareMask);
}

LOADER_EXPORT void VKAPI vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t stencilWriteMask)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetStencilWriteMask(commandBuffer, faceMask, stencilWriteMask);
}

LOADER_EXPORT void VKAPI vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t stencilReference)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetStencilReference(commandBuffer, faceMask, stencilReference);
}

LOADER_EXPORT void VKAPI vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

LOADER_EXPORT void VKAPI vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
}

LOADER_EXPORT void VKAPI vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t startBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBindVertexBuffers(commandBuffer, startBinding, bindingCount, pBuffers, pOffsets);
}

LOADER_EXPORT void VKAPI vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

LOADER_EXPORT void VKAPI vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

LOADER_EXPORT void VKAPI vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

LOADER_EXPORT void VKAPI vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

LOADER_EXPORT void VKAPI vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDispatch(commandBuffer, x, y, z);
}

LOADER_EXPORT void VKAPI vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdDispatchIndirect(commandBuffer, buffer, offset);
}

LOADER_EXPORT void VKAPI vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferCopy* pRegions)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

LOADER_EXPORT void VKAPI vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

LOADER_EXPORT void VKAPI vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}

LOADER_EXPORT void VKAPI vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

LOADER_EXPORT void VKAPI vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

LOADER_EXPORT void VKAPI vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const uint32_t* pData)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

LOADER_EXPORT void VKAPI vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
}

LOADER_EXPORT void VKAPI vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}

LOADER_EXPORT void VKAPI vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

LOADER_EXPORT void VKAPI vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment* pAttachments, uint32_t rectCount, const VkClearRect* pRects)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
}

LOADER_EXPORT void VKAPI vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageResolve* pRegions)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdResolveImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

LOADER_EXPORT void VKAPI vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdSetEvent(commandBuffer, event, stageMask);
}

LOADER_EXPORT void VKAPI vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdResetEvent(commandBuffer, event, stageMask);
}

LOADER_EXPORT void VKAPI vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, VkPipelineStageFlags sourceStageMask, VkPipelineStageFlags dstStageMask, uint32_t memoryBarrierCount, const void* const* ppMemoryBarriers)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdWaitEvents(commandBuffer, eventCount, pEvents, sourceStageMask, dstStageMask, memoryBarrierCount, ppMemoryBarriers);
}

LOADER_EXPORT void VKAPI vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const void* const* ppMemoryBarriers)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, ppMemoryBarriers);
}

LOADER_EXPORT void VKAPI vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t slot, VkFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBeginQuery(commandBuffer, queryPool, slot, flags);
}

LOADER_EXPORT void VKAPI vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t slot)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdEndQuery(commandBuffer, queryPool, slot);
}

LOADER_EXPORT void VKAPI vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t startQuery, uint32_t queryCount)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdResetQueryPool(commandBuffer, queryPool, startQuery, queryCount);
}

LOADER_EXPORT void VKAPI vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t slot)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, slot);
}

LOADER_EXPORT void VKAPI vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t startQuery, uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride, VkFlags flags)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdCopyQueryPoolResults(commandBuffer, queryPool, startQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}

LOADER_EXPORT void VKAPI vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* values)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdPushConstants(commandBuffer, layout, stageFlags, offset, size, values);
}

LOADER_EXPORT void VKAPI vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin, VkRenderPassContents contents)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

LOADER_EXPORT void VKAPI vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkRenderPassContents contents)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdNextSubpass(commandBuffer, contents);
}

LOADER_EXPORT void VKAPI vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdEndRenderPass(commandBuffer);
}

LOADER_EXPORT void VKAPI vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBuffersCount, const VkCommandBuffer* pCommandBuffers)
{
    const VkLayerDispatchTable *disp;

    disp = loader_get_dispatch(commandBuffer);

    disp->CmdExecuteCommands(commandBuffer, commandBuffersCount, pCommandBuffers);
}
