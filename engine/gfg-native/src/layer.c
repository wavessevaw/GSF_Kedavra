/* SPDX-License-Identifier: GPL-3.0-or-later
 * GFG Native WSI: independent Vulkan-loader layer foundation.
 *
 * GPU generation runs on the application device with bounded asynchronous
 * WSI output when requested. Tests establish WSI content/order, not scanout.
 * No MAKO, LSFG, Lossless.dll, or extracted model is linked or loaded.
 * All functionality is opt-in via GFG_NATIVE_FG=1.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "swapchain_chain.h"
#include "queue_observation.h"
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
#include "virtual_wsi_facade.h"
#include "output_proxy_bootstrap.h"
#include "test_loader_output_runtime.h"
#include "test_gamescope_swapchain_policy.h"
#include "virtual_layouts.h"
#endif
#ifdef GFG_NATIVE_WITH_TE
#include "cpu_bridge.h"
#include "gpu_bridge.h"
#include "present_ids.h"
#include "output_worker.h"
#endif
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

typedef struct native_instance {
    struct native_instance *next;
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps;
    uint32_t api_version;
    uint32_t engine_version;
    const char *frontend_hint;
    int enabled;
} native_instance;

typedef struct native_queue {
    struct native_queue *next;
    VkQueue handle;
    uint32_t family;
} native_queue;

typedef struct native_swapchain {
    struct native_swapchain *next;
    VkSwapchainKHR handle;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;
    uint64_t presents;
    uint64_t acquired;
    uint64_t acquire_misses;
    uint64_t present_misses;
    uint32_t image_count;
    unsigned char *owned; /* only exact acquired image indices, never fabricated */
#ifdef GFG_NATIVE_WITH_TE
    gfg_cpu_fg *bridge;
    int bridge_attempted;
    gfg_gpu_fg *gpu;
    int gpu_attempted;
    gfg_output *output;
    VkSemaphore *acquire_semaphores;
    pthread_mutex_t output_wsi;
    struct native_present_ids *ids; /* B4: layer-owned present ids (PRESENT_TRACE) */
#endif
    VkSwapchainCreateFlagsKHR create_flags;
    int copy_eligible; /* enabled transfer usage only; never means captured */
    VkPresentModeKHR present_mode;
    int force_fifo;
    const char *path; /* "gpu", "cpu" or "none": which transport handled presents */
    double *present_us; /* GFG_NATIVE_TIMING=1: CPU time per app present */
    uint32_t present_samples;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    gfg_virtual_wsi *virtual_app; /* only in isolated test builds */
    gfg_output_proxy *test_proxy; /* heap-owned app proxy in TEST builds */
    gfg_offscreen_dispatch test_proxy_ops;
    PFN_vkDestroySwapchainKHR test_proxy_destroy;
    gfg_test_loader_runtime *test_runtime;
#endif
} native_swapchain;

typedef struct native_device {
    struct native_device *next;
    void *key;
    VkDevice handle;
    VkPhysicalDevice physical;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkGetDeviceQueue get_queue;
    PFN_vkGetDeviceQueue2 get_queue2;
    PFN_vkDestroyDevice destroy;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkQueuePresentKHR present;
    PFN_vkGetSwapchainImagesKHR get_images;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    gfg_offscreen_dispatch test_proxy_ops;
    gfg_vwsi_acquire_signal test_proxy_signal;
    gfg_vwsi_copy_submit test_proxy_copy;
    void *test_proxy_user;
    VkQueue test_loader_queue;
    uint32_t test_loader_family;
    PFN_vkCmdPipelineBarrier cmd_barrier;
    PFN_vkCmdPipelineBarrier2 cmd_barrier2;
    PFN_vkCmdPipelineBarrier2KHR cmd_barrier2_khr;
#endif
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkAcquireNextImage2KHR acquire2;
    gsf_queue_observer *queue_observer; /* opt-in, API-call completion evidence */
    PFN_vkGetFenceStatus fence_status;
    PFN_vkWaitForFences wait_fences;
    PFN_vkResetFences reset_fences;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkImportFenceFdKHR import_fence_fd;
    PFN_vkGetFenceFdKHR get_fence_fd;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2, submit2_khr;
    PFN_vkQueueBindSparse bind_sparse;
    PFN_vkQueueWaitIdle queue_idle;
    PFN_vkDeviceWaitIdle device_idle;
    PFN_vkQueueBeginDebugUtilsLabelEXT queue_label_begin;
    PFN_vkQueueEndDebugUtilsLabelEXT queue_label_end;
    PFN_vkQueueInsertDebugUtilsLabelEXT queue_label_insert;
    PFN_vkQueueSetPerformanceConfigurationINTEL queue_perf;
    PFN_vkQueueSetPerfHintQCOM queue_hint;
    PFN_vkQueueNotifyOutOfBandNV queue_oob;
    PFN_vkQueueNotifyOutOfBandLegacyNV queue_oob_legacy;
    int async_output;
    pthread_mutex_t queue_lock; /* every host access to an app/worker queue */
    native_instance *instance;
    int gpu_capable;  /* external memory fd + timeline enabled by this layer */
    int display_timing; /* 0 absent, 1 app namespace, 2 layer-owned GOOGLE timing */
    int semaphore_fd; /* VK_KHR_external_semaphore_fd enabled */
    int calibrated_mode; /* 1 KHR, 2 EXT, 0 not enabled */
    int present_caps; /* NATIVE_PRESENT_* enabled (by the app or, for tracing, by us) */
    int present_trace;
    int app_present_ids;
    PFN_vkWaitForPresentKHR wait_present;
    PFN_vkWaitForPresent2KHR wait_present2;
    PFN_vkSetDeviceLoaderData set_loader_data; /* for layer-created command buffers */
    pthread_mutex_t lock;
    native_swapchain *swapchains;
    native_queue *queues;
} native_device;

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static native_instance *instances;
static native_device *devices;
static int trace_on = -1;

static void *dispatch_key(const void *handle) {
    return handle ? *(void *const *)handle : NULL;
}
static int tracing(void) {
    int expected = -1;
    const char *value = getenv("GFG_NATIVE_TRACE");
    (void)__atomic_compare_exchange_n(&trace_on, &expected,
        value && value[0] == '1' ? 1 : 0, 0,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return __atomic_load_n(&trace_on, __ATOMIC_RELAXED);
}
static native_instance *find_instance(void *key) {
    native_instance *found = NULL;
    pthread_mutex_lock(&map_lock);
    for (native_instance *i = instances; i; i = i->next)
        if (i->key == key) { found = i; break; }
    pthread_mutex_unlock(&map_lock);
    return found;
}
static native_device *find_device(void *key) {
    native_device *found = NULL;
    pthread_mutex_lock(&map_lock);
    for (native_device *d = devices; d; d = d->next)
        if (d->key == key) { found = d; break; }
    pthread_mutex_unlock(&map_lock);
    return found;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
native_gipa(VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
native_gdpa(VkDevice device, const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL
native_CreateInstance(const VkInstanceCreateInfo *ci,
    const VkAllocationCallbacks *alloc, VkInstance *out) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                     && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance next = (PFN_vkCreateInstance)
        gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = next(ci, alloc, out);
    if (result != VK_SUCCESS) return result;
    native_instance *i = calloc(1, sizeof(*i));
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
        gipa(*out, "vkDestroyInstance");
    if (!i || !destroy) {
        free(i);
        if (destroy) destroy(*out, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const char *engine = ci->pApplicationInfo
        ? ci->pApplicationInfo->pEngineName : NULL;
    i->frontend_hint=engine && !strcmp(engine,"DXVK")?"dxvk":
        engine && !strcmp(engine,"vkd3d")?"vkd3d":"other_or_unspecified";
    i->engine_version=ci->pApplicationInfo?ci->pApplicationInfo->engineVersion:0;
    const char *flag = getenv("GFG_NATIVE_FG");
    i->enabled = flag && strcmp(flag, "1") == 0 &&
        !(engine && (strcmp(engine, "gfg-temporal") == 0 ||
                     strcmp(engine, "gfg-temporal-bootstrap") == 0));
    i->handle = *out;
    i->key = dispatch_key(*out);
    i->api_version = ci->pApplicationInfo && ci->pApplicationInfo->apiVersion
        ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    i->gipa = gipa;
    i->destroy = destroy;
    i->surface_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        gipa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    pthread_mutex_lock(&map_lock);
    i->next = instances;
    instances = i;
    pthread_mutex_unlock(&map_lock);
    if (tracing())
        fprintf(stderr, "[gfg-native] instance initialized enabled=%d (WSI pass-through)\n",
                i->enabled);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
native_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    if (!instance) return;
    native_instance *found = NULL;
    pthread_mutex_lock(&map_lock);
    for (native_instance **p = &instances; *p; p = &(*p)->next)
        if ((*p)->key == dispatch_key(instance)) {
            found = *p; *p = found->next; break;
        }
    pthread_mutex_unlock(&map_lock);
    if (!found) return;
    found->destroy(instance, alloc);
    free(found);
}

/* GPU bridge prerequisites on the APP device: external memory fd (shared-fd
 * mode only) and timeline semaphores. Only when GFG_NATIVE_FG=1 and
 * GFG_NATIVE_GPU_BRIDGE=1 and Vulkan >= 1.2. Application pNext feature nodes
 * are read-only here: if the app already supplies a requested feature set to
 * VK_FALSE, generation stays disabled. Missing optional feature nodes are
 * prepended to our private VkDeviceCreateInfo copy. */
enum {
    NATIVE_PRESENT_ID = 1, NATIVE_PRESENT_WAIT = 2,      /* VK_KHR_present_id / _wait */
    NATIVE_PRESENT_ID2 = 4, NATIVE_PRESENT_WAIT2 = 8     /* VK_KHR_present_id2 / _wait2 */
};
typedef struct {
    VkDeviceCreateInfo ci;
    const char **extensions;
    VkPhysicalDeviceVulkan12Features vulkan12;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    VkPhysicalDevicePresentIdFeaturesKHR present_id;
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait;
    int active;
    int display_timing; /* 0 absent, 1 app namespace, 2 layer-owned GOOGLE timing */
    int semaphore_fd;
    int calibrated_mode;
    int present_caps; /* NATIVE_PRESENT_* enabled on the created device */
    int app_present_ids; /* the application owns its ID/wait/timing namespace */
} native_device_patch;

static int native_env_is_1(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}
static int native_has_extension(const VkExtensionProperties *p, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}
static int native_requested_extension(const VkDeviceCreateInfo *ci, const char *name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
        if (ci->ppEnabledExtensionNames[i] && !strcmp(ci->ppEnabledExtensionNames[i], name))
            return 1;
    return 0;
}
/* Enable a VkBool32 feature without writing into the application's chain.
 * Existing false values are an explicit app choice and disable our bridge. */
static int native_enable_feature(native_device_patch *p, const VkDeviceCreateInfo *app,
        VkStructureType type, size_t offset, VkBaseOutStructure *ours) {
    for (const VkBaseInStructure *node = app->pNext; node;
         node = (const VkBaseInStructure *)node->pNext)
        if (node->sType == type) {
            const VkBool32 value = *(const VkBool32 *)((const char *)node + offset);
            return value == VK_TRUE;
        }
    ours->sType = type;
    ours->pNext = (VkBaseOutStructure *)p->ci.pNext;
    *(VkBool32 *)((char *)ours + offset) = VK_TRUE;
    p->ci.pNext = ours;
    return 1;
}
static int native_chained_feature(const VkDeviceCreateInfo *ci, VkStructureType type, size_t offset) {
    for (const VkBaseInStructure *node = ci->pNext; node; node = node->pNext)
        if (node->sType == type) return *(const VkBool32 *)((const char *)node + offset) == VK_TRUE;
    return 0;
}

static const char *native_patch_device(native_instance *inst, VkPhysicalDevice phys,
        const VkDeviceCreateInfo *ci, native_device_patch *p) {
    memset(p, 0, sizeof(*p));
    p->ci = *ci;
#ifndef GFG_NATIVE_WITH_TE
    (void)inst; (void)phys;
    return "built-without-te";
#endif
    if (!inst || !inst->enabled) return "layer-not-enabled";
    if (!native_env_is_1("GFG_NATIVE_GPU_BRIDGE")) return "gpu-bridge-not-requested";
    if (inst->api_version < VK_API_VERSION_1_2) return "instance-api-below-1.2";
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)
        inst->gipa(inst->handle, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures2 feats2 = (PFN_vkGetPhysicalDeviceFeatures2)
        inst->gipa(inst->handle, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
        inst->gipa(inst->handle, "vkEnumerateDeviceExtensionProperties");
    if (!props || !feats2 || !enumerate) return "entrypoints";
    VkPhysicalDeviceProperties pp;
    props(phys, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_2) return "device-api-below-1.2";
    VkPhysicalDeviceTimelineSemaphoreFeatures tf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &tf};
    feats2(phys, &f2);
    if (!tf.timelineSemaphore) return "no-timeline-semaphore";
    uint32_t n = 0;
    if (enumerate(phys, NULL, &n, NULL) != VK_SUCCESS || !n) return "extension-query";
    VkExtensionProperties *ext = calloc(n, sizeof(*ext));
    if (!ext) return "oom";
    if (enumerate(phys, NULL, &n, ext) != VK_SUCCESS) { free(ext); return "extension-query"; }
    const int mem_fd = native_has_extension(ext, n, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    const int sem_fd = native_has_extension(ext, n, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    const int has_timing=native_has_extension(ext,n,VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    const int has_pid = native_has_extension(ext, n, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    const int has_pwait = native_has_extension(ext, n, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    /* Diagnostic opt-in. Do not call calibration functions unless the device
     * was successfully created with this extension enabled. */
    const int calibrated_mode = native_env_is_1("GFG_NATIVE_CALIBRATED_TIMESTAMPS") ?
        native_has_extension(ext,n,VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ? 1 :
        native_has_extension(ext,n,VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ? 2 : 0 : 0;
    free(ext);
    const char *sync_mode = getenv("GFG_NATIVE_GPU_SYNC");
    if (sync_mode && strcmp(sync_mode, "same-device") && strcmp(sync_mode, "shared-fd"))
        return "unsupported-sync-mode";
    const int shared_fd = sync_mode && !strcmp(sync_mode, "shared-fd");
    if (shared_fd && !mem_fd) return "no-external-memory-fd";
    if (shared_fd && !sem_fd) return "no-external-semaphore-fd";
    /* What the app itself enabled (extension + feature bit). */
    if (native_requested_extension(ci, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
        native_chained_feature(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                               offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId)))
        p->present_caps |= NATIVE_PRESENT_ID;
    if (native_requested_extension(ci, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) &&
        native_chained_feature(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                               offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait)))
        p->present_caps |= NATIVE_PRESENT_WAIT;
    if (native_requested_extension(ci, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
        native_chained_feature(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
                               offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)))
        p->present_caps |= NATIVE_PRESENT_ID2;
    if (native_requested_extension(ci, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) &&
        native_chained_feature(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
                               offsetof(VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2)))
        p->present_caps |= NATIVE_PRESENT_WAIT2;
    p->app_present_ids = !!(p->present_caps & (NATIVE_PRESENT_ID | NATIVE_PRESENT_ID2));
    /* GFG_NATIVE_PRESENT_TRACE=1: enable v1 present id/wait ourselves when
     * supported and not already enabled (presented-frame instrumentation). */
    int add_pid = 0, add_pwait = 0;
    if (native_env_is_1("GFG_NATIVE_PRESENT_TRACE") && !p->app_present_ids && has_pid && has_pwait) {
        VkPhysicalDevicePresentIdFeaturesKHR qid = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        VkPhysicalDevicePresentWaitFeaturesKHR qwait = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .pNext = &qid};
        VkPhysicalDeviceFeatures2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &qwait};
        feats2(phys, &q2);
        if (qid.presentId && qwait.presentWait) {
            add_pid = !(p->present_caps & NATIVE_PRESENT_ID);
            add_pwait = !(p->present_caps & NATIVE_PRESENT_WAIT);
        }
    }
    p->extensions = calloc(ci->enabledExtensionCount + 6u, sizeof(char *));
    if (!p->extensions) return "oom";
    uint32_t count = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
        p->extensions[count++] = ci->ppEnabledExtensionNames[i];
    /* Same-device mode allocates and synchronizes every object on the app's
     * own device. External-FD extensions are only needed by the explicitly
     * selected second-device path. */
    if (shared_fd && !native_requested_extension(ci, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
        p->extensions[count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    if (shared_fd && !native_requested_extension(ci, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
        p->extensions[count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    if (add_pid && !native_requested_extension(ci, VK_KHR_PRESENT_ID_EXTENSION_NAME))
        p->extensions[count++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
    if (add_pwait && !native_requested_extension(ci, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        p->extensions[count++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    if(calibrated_mode) {
        const char *cal_name=calibrated_mode==1?VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME:
            VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        if(!native_requested_extension(ci,cal_name))p->extensions[count++]=cal_name;
        p->calibrated_mode=calibrated_mode;
    }
    if(native_requested_extension(ci,VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME))p->display_timing=1;
    else if(has_timing) {
        p->extensions[count++]=VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
        p->display_timing=2;
    }
    p->ci.enabledExtensionCount = count;
    p->ci.ppEnabledExtensionNames = p->extensions;
    /* timelineSemaphore: an app Vulkan12Features struct forbids also
     * chaining the individual struct, so read the existing form or add ours. */
    int vk12 = 0;
    for (const VkBaseInStructure *node = ci->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) vk12 = 1;
    if (vk12) {
        if (!native_enable_feature(p, ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            offsetof(VkPhysicalDeviceVulkan12Features, timelineSemaphore),
            (VkBaseOutStructure *)&p->vulkan12))
            return "app-disabled-timeline";
    } else {
        if (!native_enable_feature(p, ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore),
            (VkBaseOutStructure *)&p->timeline)) return "app-disabled-timeline";
    }
    if (add_pid) {
        const int enabled = native_enable_feature(p, ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
            offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId),
            (VkBaseOutStructure *)&p->present_id);
        if (enabled) p->present_caps |= NATIVE_PRESENT_ID;
    }
    if (add_pwait) {
        const int enabled = native_enable_feature(p, ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
            offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait),
            (VkBaseOutStructure *)&p->present_wait);
        if (enabled) p->present_caps |= NATIVE_PRESENT_WAIT;
    }
    p->active = 1;
    p->semaphore_fd = shared_fd;
    return NULL;
}
static void native_unpatch_device(native_device_patch *p) {
    free(p->extensions);
    p->extensions = NULL;
}

/* Lower layers consume the loader's LINK_INFO cursor even on failure. A
 * fallback is a fresh traversal of that chain, not a call with its exhausted
 * cursor. Only the loader-owned mutable link is restored; app feature/extension
 * structures remain unchanged. Capture the cursor after consuming OUR link. */
static VkResult native_create_device_with_fallback(PFN_vkCreateDevice next,
        VkPhysicalDevice physical,const VkDeviceCreateInfo *app,
        const VkDeviceCreateInfo *patched,VkLayerDeviceCreateInfo *loader_link,
        const VkAllocationCallbacks *allocator,VkDevice *device,int *fallback) {
    VkLayerDeviceLink *downstream=loader_link->u.pLayerInfo;
    *fallback=0;
    VkResult result=next(physical,patched?patched:app,allocator,device);
    if(patched && result!=VK_SUCCESS) {
        loader_link->u.pLayerInfo=downstream;
        *fallback=1;
        result=next(physical,app,allocator,device);
    }
    return result;
}

#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
/* Loader-driven create-only probe. Acquire/Present MUST NOT succeed in
 * this mode; it validates real app -> loader -> layer -> Vulkan GPU
 * resource allocation, not video output or game compatibility. */
static VkResult native_probe_no_acquire(void *user, VkSemaphore sem,VkFence fence) {
    (void)user;(void)sem;(void)fence;return VK_ERROR_FEATURE_NOT_PRESENT;
}
static VkResult native_probe_no_capture(void *user,uint32_t idx,uint64_t token,
        uint32_t waits,const VkSemaphore *sems) {
    (void)user;(void)idx;(void)token;(void)waits;(void)sems;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL
native_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *alloc, VkDevice *out) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                     && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkSetDeviceLoaderData set_loader_data = NULL;
    for (VkLayerDeviceCreateInfo *cb = (VkLayerDeviceCreateInfo *)ci->pNext; cb;
         cb = (VkLayerDeviceCreateInfo *)cb->pNext)
        if (cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            cb->function == VK_LOADER_DATA_CALLBACK) {
            set_loader_data = cb->u.pfnSetDeviceLoaderData;
            break;
        }
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateDevice next = (PFN_vkCreateDevice)
        gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    native_device_patch patch;
    const char *patch_reason = native_patch_device(find_instance(dispatch_key(phys)),
                                                   phys, ci, &patch);
    int fallback=0;
    VkResult result=native_create_device_with_fallback(next,phys,ci,
        patch.active?&patch.ci:NULL,link,alloc,out,&fallback);
    const int gpu_capable = patch.active && !fallback && result == VK_SUCCESS;
    const int display_timing=gpu_capable?patch.display_timing:0;
    const int semaphore_fd = gpu_capable && patch.semaphore_fd;
    const int calibrated_mode = gpu_capable ? patch.calibrated_mode : 0;
    const int present_caps = gpu_capable ? patch.present_caps : 0;
    const int app_present_ids = gpu_capable && patch.app_present_ids;
    native_unpatch_device(&patch);
    if(fallback)patch_reason="patched-create-failed-real-only";
    if (tracing())
        fprintf(stderr, "[gfg-native] device gpu_bridge_capable=%d semaphore_fd=%d"
                " present_caps=0x%x reason=%s\n", gpu_capable, semaphore_fd, present_caps,
                patch_reason ? patch_reason : "enabled");
    if (result != VK_SUCCESS) return result;
    native_device *d = calloc(1, sizeof(*d));
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)
        gdpa(*out, "vkDestroyDevice");
    if (!d || !destroy) {
        free(d);
        if (destroy) destroy(*out, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pthread_mutex_init(&d->lock, NULL) != 0) {
        destroy(*out, alloc); free(d);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_init(&d->queue_lock, NULL) != 0) {
        pthread_mutex_destroy(&d->lock); destroy(*out, alloc); free(d);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    d->handle = *out;
    d->key = dispatch_key(*out);
    d->physical = phys;
    d->gpu_capable = gpu_capable;
    d->set_loader_data = set_loader_data;
    d->semaphore_fd = semaphore_fd;
    d->calibrated_mode=calibrated_mode;
    d->display_timing=display_timing;
    d->present_caps = present_caps;
    d->present_trace = native_env_is_1("GFG_NATIVE_PRESENT_TRACE");
    d->app_present_ids = app_present_ids;
    d->async_output = gpu_capable && native_env_is_1("GFG_NATIVE_ASYNC") &&
        !(getenv("GFG_NATIVE_GPU_SYNC") && !strcmp(getenv("GFG_NATIVE_GPU_SYNC"), "shared-fd"));
    d->gdpa = gdpa;
    d->destroy = destroy;
    d->get_queue = (PFN_vkGetDeviceQueue)gdpa(*out, "vkGetDeviceQueue");
    d->get_queue2 = (PFN_vkGetDeviceQueue2)gdpa(*out, "vkGetDeviceQueue2");
    d->create_swapchain = (PFN_vkCreateSwapchainKHR)
        gdpa(*out, "vkCreateSwapchainKHR");
    d->destroy_swapchain = (PFN_vkDestroySwapchainKHR)
        gdpa(*out, "vkDestroySwapchainKHR");
    d->present = (PFN_vkQueuePresentKHR)
        gdpa(*out, "vkQueuePresentKHR");
    d->get_images = (PFN_vkGetSwapchainImagesKHR)
        gdpa(*out, "vkGetSwapchainImagesKHR");
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if(native_env_is_1("GFG_NATIVE_TEST_LOADER_CREATE_PROBE") ||
       native_env_is_1("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT")) {
        /* Acquire only the DOWNSTREAM functions for the exact device and
         * instance chains. Never query a borrowed physical device through
         * the loader-global vkGetPhysicalDeviceMemoryProperties symbol. */
        d->test_proxy_ops=(gfg_offscreen_dispatch){
            .create_image=(PFN_vkCreateImage)gdpa(*out,"vkCreateImage"),
            .destroy_image=(PFN_vkDestroyImage)gdpa(*out,"vkDestroyImage"),
            .get_image_requirements=(PFN_vkGetImageMemoryRequirements)
                gdpa(*out,"vkGetImageMemoryRequirements"),
            /* Resolved below from the instance's exact next-layer GIPA. */
            .get_memory_properties=NULL,
            .allocate_memory=(PFN_vkAllocateMemory)gdpa(*out,"vkAllocateMemory"),
            .free_memory=(PFN_vkFreeMemory)gdpa(*out,"vkFreeMemory"),
            .bind_image_memory=(PFN_vkBindImageMemory)gdpa(*out,"vkBindImageMemory")
        };
        if(native_env_is_1("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT")) {
            d->test_proxy_signal=gfg_test_live_acquire;
            d->test_proxy_copy=gfg_test_live_capture;
            if(ci && ci->queueCreateInfoCount && ci->pQueueCreateInfos &&
               ci->pQueueCreateInfos[0].queueCount) {
                d->test_loader_family=ci->pQueueCreateInfos[0].queueFamilyIndex;
                if(d->get_queue)
                    d->get_queue(d->handle,d->test_loader_family,0,
                                 &d->test_loader_queue);
            }
        } else {
            d->test_proxy_signal=native_probe_no_acquire;
            d->test_proxy_copy=native_probe_no_capture;
        }
    }
#endif
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    d->cmd_barrier = (PFN_vkCmdPipelineBarrier)gdpa(*out, "vkCmdPipelineBarrier");
    d->cmd_barrier2 = (PFN_vkCmdPipelineBarrier2)gdpa(*out, "vkCmdPipelineBarrier2");
    d->cmd_barrier2_khr = (PFN_vkCmdPipelineBarrier2KHR)gdpa(*out, "vkCmdPipelineBarrier2KHR");
#endif
    d->acquire = (PFN_vkAcquireNextImageKHR)
        gdpa(*out, "vkAcquireNextImageKHR");
    d->wait_present = (PFN_vkWaitForPresentKHR)gdpa(*out, "vkWaitForPresentKHR");
    d->wait_present2 = (PFN_vkWaitForPresent2KHR)gdpa(*out, "vkWaitForPresent2KHR");
    d->acquire2 = (PFN_vkAcquireNextImage2KHR)
        gdpa(*out, "vkAcquireNextImage2KHR");
    d->import_fence_fd = (PFN_vkImportFenceFdKHR)gdpa(*out,"vkImportFenceFdKHR");
    d->get_fence_fd = (PFN_vkGetFenceFdKHR)gdpa(*out,"vkGetFenceFdKHR");
    d->fence_status = (PFN_vkGetFenceStatus)gdpa(*out, "vkGetFenceStatus");
    d->wait_fences = (PFN_vkWaitForFences)gdpa(*out, "vkWaitForFences");
    d->reset_fences = (PFN_vkResetFences)gdpa(*out, "vkResetFences");
    d->destroy_fence = (PFN_vkDestroyFence)gdpa(*out, "vkDestroyFence");
    if(d->async_output && native_env_is_1("GFG_NATIVE_QUEUE_OBSERVE"))
        d->queue_observer=calloc(1,sizeof(*d->queue_observer));
    d->submit = (PFN_vkQueueSubmit)gdpa(*out, "vkQueueSubmit");
    d->submit2 = (PFN_vkQueueSubmit2)gdpa(*out, "vkQueueSubmit2");
    d->submit2_khr = (PFN_vkQueueSubmit2)gdpa(*out, "vkQueueSubmit2KHR");
    d->bind_sparse = (PFN_vkQueueBindSparse)gdpa(*out, "vkQueueBindSparse");
    d->queue_idle = (PFN_vkQueueWaitIdle)gdpa(*out, "vkQueueWaitIdle");
    d->device_idle = (PFN_vkDeviceWaitIdle)gdpa(*out, "vkDeviceWaitIdle");
    d->queue_label_begin = (PFN_vkQueueBeginDebugUtilsLabelEXT)gdpa(*out, "vkQueueBeginDebugUtilsLabelEXT");
    d->queue_label_end = (PFN_vkQueueEndDebugUtilsLabelEXT)gdpa(*out, "vkQueueEndDebugUtilsLabelEXT");
    d->queue_label_insert = (PFN_vkQueueInsertDebugUtilsLabelEXT)gdpa(*out, "vkQueueInsertDebugUtilsLabelEXT");
    d->queue_perf = (PFN_vkQueueSetPerformanceConfigurationINTEL)gdpa(*out, "vkQueueSetPerformanceConfigurationINTEL");
    d->queue_hint = (PFN_vkQueueSetPerfHintQCOM)gdpa(*out, "vkQueueSetPerfHintQCOM");
    d->queue_oob = (PFN_vkQueueNotifyOutOfBandNV)gdpa(*out, "vkQueueNotifyOutOfBandNV");
    d->queue_oob_legacy = (PFN_vkQueueNotifyOutOfBandLegacyNV)gdpa(*out, "vkQueueNotifyOutOfBandLegacyNV");
    pthread_mutex_lock(&map_lock);
    for (native_instance *i = instances; i; i = i->next)
        if (i->key == dispatch_key(phys)) {
            d->instance = i; break;
        }
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if((native_env_is_1("GFG_NATIVE_TEST_LOADER_CREATE_PROBE") ||
        native_env_is_1("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT")) && d->instance)
        d->test_proxy_ops.get_memory_properties =
            (PFN_vkGetPhysicalDeviceMemoryProperties)
            d->instance->gipa(d->instance->handle,
                              "vkGetPhysicalDeviceMemoryProperties");
#endif
    d->next = devices;
    devices = d;
    pthread_mutex_unlock(&map_lock);
    return VK_SUCCESS;
}

#ifdef GFG_NATIVE_WITH_TE
/* ---- B4: layer-owned present ids + optional presentation monitor ----
 * Active only with GFG_NATIVE_PRESENT_TRACE=1 when IDs were enabled by the
 * layer, rather than already owned by the application. In that namespace
 * every present gets a fresh layer ID. App-owned IDs, waits and timing query
 * results bypass this remapping and synthetic calls omit optional IDs. */
#define NATIVE_MONITOR_QUEUE 256u
typedef struct native_present_ids {
    pthread_mutex_t lock;
    /* Present-wait and acquire externally synchronize the swapchain. The
     * monitor and application WSI wrappers share this short-held lock. */
    pthread_mutex_t wsi_lock;
    pthread_cond_t cond;          /* CLOCK_MONOTONIC */
    gfg_present_map map;
    int mode;                     /* 1 VkPresentIdKHR, 2 VkPresentId2KHR */
    int wait_mode;                /* 0 none, 1 vkWaitForPresentKHR, 2 vkWaitForPresent2KHR */
    VkDevice device;
    VkSwapchainKHR swapchain;
    PFN_vkWaitForPresentKHR wait1;
    PFN_vkWaitForPresent2KHR wait2;
    int monitor_started;
    int stop;
    pthread_t thread;
    uint64_t queue_id[NATIVE_MONITOR_QUEUE];
    int queue_kind[NATIVE_MONITOR_QUEUE];
    uint32_t qhead, qcount;
    uint64_t dropped, done, last_done_ns;
} native_present_ids;

static uint64_t native_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static VkResult native_ids_driver_wait(native_present_ids *ids, uint64_t id, uint64_t timeout) {
    if (ids->wait_mode == 2) {
        VkPresentWait2InfoKHR wi = {.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR,
            .presentId = id, .timeout = timeout};
        return ids->wait2(ids->device, ids->swapchain, &wi);
    }
    return ids->wait1(ids->device, ids->swapchain, id, timeout);
}
static void *native_monitor_main(void *arg) {
    native_present_ids *ids = arg;
    static const char *kinds[4] = {"R", "G1", "G2", "D"};
    pthread_mutex_lock(&ids->lock);
    for (;;) {
        while (!ids->qcount && !ids->stop) pthread_cond_wait(&ids->cond, &ids->lock);
        if (ids->stop) break;
        const uint64_t id = ids->queue_id[ids->qhead];
        const int kind = ids->queue_kind[ids->qhead];
        pthread_mutex_unlock(&ids->lock);
        VkResult r;
        for (;;) {
            /* Present-wait requires external synchronization of the swapchain.
             * Poll with timeout zero so it never holds that lock across a frame
             * interval or stalls the application's next QueuePresent call. */
            pthread_mutex_lock(&ids->wsi_lock);
            r = native_ids_driver_wait(ids, id, 0);
            pthread_mutex_unlock(&ids->wsi_lock);
            if (r != VK_TIMEOUT || __atomic_load_n(&ids->stop, __ATOMIC_RELAXED))
                break;
            const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
        const uint64_t t = native_now_ns();
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[gfg-native] present_done id=%llu kind=%s t_ns=%llu dt_us=%.1f\n",
                    (unsigned long long)id, kinds[kind < 4 && kind >= 0 ? kind : 0],
                    (unsigned long long)t,
                    ids->last_done_ns ? (double)(t - ids->last_done_ns) / 1000.0 : 0.0);
            ids->last_done_ns = t;
        } else if (r != VK_TIMEOUT) {
            fprintf(stderr, "[gfg-native] present_wait id=%llu kind=%s vkresult=%d\n",
                    (unsigned long long)id, kinds[kind < 4 && kind >= 0 ? kind : 0], (int)r);
        }
        pthread_mutex_lock(&ids->lock);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) ++ids->done;
        ids->qhead = (ids->qhead + 1) % NATIVE_MONITOR_QUEUE;
        --ids->qcount;
    }
    pthread_mutex_unlock(&ids->lock);
    return NULL;
}
static uint64_t native_ids_assign(void *user, int kind, uint64_t app_id) {
    native_present_ids *ids = user;
    pthread_mutex_lock(&ids->lock);
    const uint64_t id = gfg_pm_next(&ids->map);
    if (kind == 0) gfg_pm_map(&ids->map, app_id, id);
    if (ids->monitor_started) {
        if (ids->qcount < NATIVE_MONITOR_QUEUE) {
            const uint32_t slot = (ids->qhead + ids->qcount++) % NATIVE_MONITOR_QUEUE;
            ids->queue_id[slot] = id;
            ids->queue_kind[slot] = kind;
        } else {
            ++ids->dropped;
        }
    }
    pthread_cond_broadcast(&ids->cond);
    pthread_mutex_unlock(&ids->lock);
    return id;
}
static native_present_ids *native_ids_create(native_device *d, native_swapchain *sc) {
    /* Preserve the application's ID/wait/timing namespace. Synthetic calls
     * omit optional IDs, and the real call keeps its app ID. This also keeps
     * passthrough of unknown metadata and direct app waits correct. */
    if (d->app_present_ids) {
        if (d->present_trace)
            fprintf(stderr, "[gfg-native] present-ids policy=application-owned synthetic_ids=omitted monitor=0\n");
        return NULL;
    }
    if (!d->present_trace) return NULL;
    int mode = 0, wait_mode = 0;
    if ((sc->create_flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) &&
        (d->present_caps & NATIVE_PRESENT_ID2)) {
        mode = 2;
        if ((sc->create_flags & VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR) &&
            (d->present_caps & NATIVE_PRESENT_WAIT2) && d->wait_present2)
            wait_mode = 2;
    } else if (d->present_caps & NATIVE_PRESENT_ID) {
        mode = 1;
        if ((d->present_caps & NATIVE_PRESENT_WAIT) && d->wait_present) wait_mode = 1;
    }
    if (!mode) {
        fprintf(stderr, "[gfg-native] present-ids mode=0 monitor=0 (present_caps=0x%x)\n",
                d->present_caps);
        return NULL;
    }
    native_present_ids *ids = calloc(1, sizeof(*ids));
    if (!ids) return NULL;
    pthread_condattr_t ca;
    if (pthread_mutex_init(&ids->lock, NULL) != 0) { free(ids); return NULL; }
    if (pthread_mutex_init(&ids->wsi_lock, NULL) != 0) {
        pthread_mutex_destroy(&ids->lock); free(ids); return NULL;
    }
    if (pthread_condattr_init(&ca) != 0) {
        pthread_mutex_destroy(&ids->wsi_lock); pthread_mutex_destroy(&ids->lock);
        free(ids); return NULL;
    }
    if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&ids->cond, &ca) != 0) {
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&ids->wsi_lock); pthread_mutex_destroy(&ids->lock);
        free(ids); return NULL;
    }
    pthread_condattr_destroy(&ca);
    ids->mode = mode;
    ids->wait_mode = wait_mode;
    ids->device = d->handle;
    ids->swapchain = sc->handle;
    ids->wait1 = d->wait_present;
    ids->wait2 = d->wait_present2;
    if (d->present_trace && wait_mode && pthread_create(&ids->thread, NULL, native_monitor_main, ids) == 0)
        ids->monitor_started = 1;
    fprintf(stderr, "[gfg-native] present-ids mode=%d monitor=%d (present_caps=0x%x)\n",
            mode, ids->monitor_started, d->present_caps);
    return ids;
}
static void native_ids_destroy(native_present_ids *ids) {
    if (!ids) return;
    pthread_mutex_lock(&ids->lock);
    __atomic_store_n(&ids->stop, 1, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&ids->cond);
    pthread_mutex_unlock(&ids->lock);
    if (ids->monitor_started) pthread_join(ids->thread, NULL);
    fprintf(stderr, "[gfg-native] present-ids retired last_layer_id=%llu last_app_id=%llu"
            " monitored_done=%llu dropped=%llu\n",
            (unsigned long long)ids->map.last_layer, (unsigned long long)ids->map.last_app,
            (unsigned long long)ids->done, (unsigned long long)ids->dropped);
    pthread_cond_destroy(&ids->cond);
    pthread_mutex_destroy(&ids->wsi_lock);
    pthread_mutex_destroy(&ids->lock);
    free(ids);
}
/* app id -> layer id; waits (bounded by timeout) for the app's present */
static VkResult native_ids_translate(native_present_ids *ids, uint64_t app_id, uint64_t timeout,
                                     uint64_t *layer_id, uint64_t *remaining) {
    const uint64_t start = native_now_ns();
    VkResult r = VK_SUCCESS;
    pthread_mutex_lock(&ids->lock);
    while (!gfg_pm_translate(&ids->map, app_id, layer_id)) {
        if (timeout == 0) { r = VK_TIMEOUT; break; }
        if (timeout == UINT64_MAX) {
            pthread_cond_wait(&ids->cond, &ids->lock);
            continue;
        }
        const uint64_t elapsed = native_now_ns() - start;
        if (elapsed >= timeout) { r = VK_TIMEOUT; break; }
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        const uint64_t left = timeout - elapsed;
        until.tv_sec += (time_t)(left / 1000000000ull);
        until.tv_nsec += (long)(left % 1000000000ull);
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&ids->cond, &ids->lock, &until);
    }
    pthread_mutex_unlock(&ids->lock);
    if (timeout == UINT64_MAX) *remaining = UINT64_MAX;
    else {
        const uint64_t elapsed = native_now_ns() - start;
        *remaining = elapsed < timeout ? timeout - elapsed : 0;
    }
    return r;
}
#endif

#define NATIVE_TIMING_CAP 8192
/* All queue host operations, including app submits resolved through GIPA,
 * share one device lock. It is never held across a worker fence/image wait. */
/* Fence lifetimes share queue_lock ONLY in this opt-in observer. Never hold
 * this lock across an added GPU wait, nor add fences to application work. */
static _Thread_local gsf_queue_snapshot *native_queue_probe_scope;
#if defined(GFG_NATIVE_WITH_TE) || defined(GFG_NATIVE_TEST_QUEUE_OBSERVE)
static void native_queue_probe(VkQueue queue,gsf_queue_snapshot *snapshot) {
    (void)queue;native_queue_probe_scope=snapshot;
}
#endif
static uint64_t native_qo_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void native_queue_sample(native_device *d,VkQueue queue) {
    gsf_queue_snapshot *s=native_queue_probe_scope;
    if(!d->queue_observer || !s)return;
    if(s->captures++)return; /* first TE queue submit, count subsequent ones */
    const uint64_t begin=native_qo_now();
    gsf_qo_snapshot(d->queue_observer,queue,d->handle,d->fence_status,s);
    s->captured_ns=native_qo_now();s->query_cpu_ns=s->captured_ns-begin;
}
#define NATIVE_SUBMIT_WRAPPER(name,field,type,application) \
static VKAPI_ATTR VkResult VKAPI_CALL name(VkQueue q,uint32_t n,const type *s,VkFence f) { \
    native_device *d=find_device(dispatch_key(q)); \
    if(!d || !d->field)return VK_ERROR_INITIALIZATION_FAILED; \
    pthread_mutex_lock(&d->queue_lock); \
    if(!(application))native_queue_sample(d,q); \
    VkResult r=d->field(q,n,s,f); \
    if((application) && d->queue_observer)gsf_qo_submit(d->queue_observer,q,f,r); \
    pthread_mutex_unlock(&d->queue_lock);return r; }
NATIVE_SUBMIT_WRAPPER(native_QueueSubmit,submit,VkSubmitInfo,1)
NATIVE_SUBMIT_WRAPPER(native_QueueSubmit2,submit2,VkSubmitInfo2,1)
NATIVE_SUBMIT_WRAPPER(native_QueueSubmit2KHR,submit2_khr,VkSubmitInfo2,1)
NATIVE_SUBMIT_WRAPPER(native_QueueBindSparse,bind_sparse,VkBindSparseInfo,1)
#if defined(GFG_NATIVE_WITH_TE) || defined(GFG_NATIVE_TEST_QUEUE_OBSERVE)
NATIVE_SUBMIT_WRAPPER(native_worker_QueueSubmit,submit,VkSubmitInfo,0)
NATIVE_SUBMIT_WRAPPER(native_worker_QueueSubmit2,submit2,VkSubmitInfo2,0)
NATIVE_SUBMIT_WRAPPER(native_worker_QueueSubmit2KHR,submit2_khr,VkSubmitInfo2,0)
#endif
#undef NATIVE_SUBMIT_WRAPPER
static VKAPI_ATTR VkResult VKAPI_CALL native_GetFenceStatus(VkDevice device,VkFence f) {
    native_device *d=find_device(dispatch_key(device));
    if(!d || !d->fence_status)return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->queue_observer)return d->fence_status(device,f);
    pthread_mutex_lock(&d->queue_lock);VkResult r=d->fence_status(device,f);
    if(r==VK_SUCCESS)gsf_qo_signaled(d->queue_observer,f);
    pthread_mutex_unlock(&d->queue_lock);return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_WaitForFences(VkDevice device,uint32_t n,const VkFence *f,
                                                          VkBool32 all,uint64_t timeout) {
    native_device *d=find_device(dispatch_key(device));
    if(!d || !d->wait_fences)return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->queue_observer)return d->wait_fences(device,n,f,all,timeout);
    gsf_qo_fence before[GSF_QO_FENCES];
    pthread_mutex_lock(&d->queue_lock);
    memcpy(before,d->queue_observer->fences,sizeof(before));
    pthread_mutex_unlock(&d->queue_lock);
    /* This is ONLY the app's original wait, unchanged. Never hold queue_lock
     * across it: another thread may need to submit the work being waited on. */
    VkResult r=d->wait_fences(device,n,f,all,timeout);
    if(r==VK_SUCCESS && (all || n==1)) {
        pthread_mutex_lock(&d->queue_lock);
        for(unsigned i=0;i<GSF_QO_FENCES;++i)if(before[i].handle) {
            gsf_qo_fence *now=&d->queue_observer->fences[i];
            if(now->handle!=before[i].handle || now->queue!=before[i].queue ||
                now->sequence!=before[i].sequence)continue; /* lifetime changed */
            for(uint32_t j=0;j<n;++j)if(f[j]==before[i].handle) {
                gsf_qo_signaled(d->queue_observer,f[j]);break;
            }
        }
        pthread_mutex_unlock(&d->queue_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_ResetFences(VkDevice device,uint32_t n,const VkFence *f) {
    native_device *d=find_device(dispatch_key(device));
    if(!d || !d->reset_fences)return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->queue_observer)return d->reset_fences(device,n,f);
    pthread_mutex_lock(&d->queue_lock);
    VkResult r=d->reset_fences(device,n,f);
    if(r==VK_SUCCESS)for(uint32_t i=0;i<n;++i)gsf_qo_forget(d->queue_observer,f[i]);
    pthread_mutex_unlock(&d->queue_lock);return r;
}
static VKAPI_ATTR void VKAPI_CALL native_DestroyFence(VkDevice device,VkFence f,const VkAllocationCallbacks *a) {
    native_device *d=find_device(dispatch_key(device));if(!d || !d->destroy_fence)return;
    if(!d->queue_observer){d->destroy_fence(device,f,a);return;}
    pthread_mutex_lock(&d->queue_lock);gsf_qo_forget(d->queue_observer,f);
    d->destroy_fence(device,f,a);pthread_mutex_unlock(&d->queue_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL native_ImportFenceFdKHR(VkDevice device,const VkImportFenceFdInfoKHR *info) {
    native_device *d=find_device(dispatch_key(device));
    if(!d || !d->import_fence_fd)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!d->queue_observer)return d->import_fence_fd(device,info);
    pthread_mutex_lock(&d->queue_lock);
    VkResult r=d->import_fence_fd(device,info);
    if(r==VK_SUCCESS)gsf_qo_forget(d->queue_observer,info->fence);
    pthread_mutex_unlock(&d->queue_lock);return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_GetFenceFdKHR(VkDevice device,const VkFenceGetFdInfoKHR *info,int *fd) {
    native_device *d=find_device(dispatch_key(device));
    if(!d || !d->get_fence_fd)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!d->queue_observer)return d->get_fence_fd(device,info,fd);
    pthread_mutex_lock(&d->queue_lock);
    VkResult r=d->get_fence_fd(device,info,fd);
    if(r==VK_SUCCESS)gsf_qo_forget(d->queue_observer,info->fence);
    pthread_mutex_unlock(&d->queue_lock);return r;
}
static VkResult native_locked_present(VkQueue queue, const VkPresentInfoKHR *info) {
    native_device *d = find_device(dispatch_key(queue));
    if (!d || !d->present) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&d->queue_lock); VkResult r = d->present(queue, info);
    pthread_mutex_unlock(&d->queue_lock); return r;
}
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
#include "../tests/gpu_observer.h"
static int test_observer_active;
void gfg_test_observer_kind(unsigned kind,uint64_t frame,unsigned multiplier) {
    observer.frame=(uint32_t)frame;observer.interval_calls=kind==2?multiplier-1:kind;
    observer.multiplier=multiplier;
}
#endif
static VkResult native_serialized_present(VkQueue queue, const VkPresentInfoKHR *info) {
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
    if (test_observer_active) return observer_present(queue,info);
#endif
    return native_locked_present(queue,info);
}
/* The bridge's own WSI must reach the next layer directly. Re-entering the
 * application acquire wrapper would compete with its own output priority and
 * recursively lock the swapchain. Only queue host entrypoints are wrapped. */
#if defined(GFG_NATIVE_WITH_TE) || defined(GFG_NATIVE_TEST_QUEUE_OBSERVE)
static VKAPI_ATTR VkResult VKAPI_CALL native_worker_QueueWaitIdle(VkQueue q) {
    native_device *d=find_device(dispatch_key(q));
    if(!d || !d->queue_idle)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&d->queue_lock);VkResult r=d->queue_idle(q);
    if(r==VK_SUCCESS && d->queue_observer)gsf_qo_idle(d->queue_observer,q);
    pthread_mutex_unlock(&d->queue_lock);return r;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_worker_gdpa(VkDevice device, const char *name) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !name) return NULL;
    if (!strcmp(name,"vkQueueWaitIdle")) return (PFN_vkVoidFunction)native_worker_QueueWaitIdle;
    if (!strcmp(name,"vkQueueSubmit")) return (PFN_vkVoidFunction)native_worker_QueueSubmit;
    if (!strcmp(name,"vkQueueSubmit2")) return d->submit2 ? (PFN_vkVoidFunction)native_worker_QueueSubmit2 : NULL;
    if (!strcmp(name,"vkQueueSubmit2KHR")) return d->submit2_khr ? (PFN_vkVoidFunction)native_worker_QueueSubmit2KHR : NULL;
    return d->gdpa(device,name);
}
#endif
static VkResult native_drain_device(native_device *d) {
    VkResult result = VK_SUCCESS;
#ifdef GFG_NATIVE_WITH_TE
    pthread_mutex_lock(&d->lock);
    for (native_swapchain *sc = d->swapchains; sc; sc = sc->next)
        if (sc->output) {
            VkResult r = gfg_output_drain(sc->output,0,UINT64_MAX,NULL);
            if (r < 0) result = r;
        }
    pthread_mutex_unlock(&d->lock);
#else
    (void)d;
#endif
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_QueueWaitIdle(VkQueue q) {
    native_device *d = find_device(dispatch_key(q));
    if (!d || !d->queue_idle) return VK_ERROR_INITIALIZATION_FAILED;
    /* Always wait the real GPU queue even if a previous WSI job failed. */
    native_drain_device(d);
    pthread_mutex_lock(&d->queue_lock); VkResult r = d->queue_idle(q);
    if(r==VK_SUCCESS && d->queue_observer)gsf_qo_idle(d->queue_observer,q);
    pthread_mutex_unlock(&d->queue_lock); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_DeviceWaitIdle(VkDevice device) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->device_idle) return VK_ERROR_INITIALIZATION_FAILED;
    native_drain_device(d);
    pthread_mutex_lock(&d->queue_lock); VkResult r = d->device_idle(device);
    if(r==VK_SUCCESS && d->queue_observer)gsf_qo_idle(d->queue_observer,VK_NULL_HANDLE);
    pthread_mutex_unlock(&d->queue_lock); return r;
}
#define QUEUE_VOID_LABEL(name, field) \
static VKAPI_ATTR void VKAPI_CALL native_##name(VkQueue q, const VkDebugUtilsLabelEXT *p) { \
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->field) return; \
    pthread_mutex_lock(&d->queue_lock); d->field(q,p); pthread_mutex_unlock(&d->queue_lock); }
QUEUE_VOID_LABEL(QueueBeginDebugUtilsLabelEXT,queue_label_begin)
QUEUE_VOID_LABEL(QueueInsertDebugUtilsLabelEXT,queue_label_insert)
#undef QUEUE_VOID_LABEL
static VKAPI_ATTR void VKAPI_CALL native_QueueEndDebugUtilsLabelEXT(VkQueue q) {
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->queue_label_end) return;
    pthread_mutex_lock(&d->queue_lock); d->queue_label_end(q); pthread_mutex_unlock(&d->queue_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL native_QueueSetPerformanceConfigurationINTEL(VkQueue q, VkPerformanceConfigurationINTEL p) {
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->queue_perf) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&d->queue_lock); VkResult r=d->queue_perf(q,p); pthread_mutex_unlock(&d->queue_lock); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_QueueSetPerfHintQCOM(VkQueue q, const VkPerfHintInfoQCOM *p) {
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->queue_hint) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&d->queue_lock); VkResult r=d->queue_hint(q,p); pthread_mutex_unlock(&d->queue_lock); return r;
}
static VKAPI_ATTR void VKAPI_CALL native_QueueNotifyOutOfBandNV(VkQueue q, const VkOutOfBandQueueTypeInfoNV *p) {
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->queue_oob) return;
    pthread_mutex_lock(&d->queue_lock); d->queue_oob(q,p); pthread_mutex_unlock(&d->queue_lock);
}
static VKAPI_ATTR void VKAPI_CALL native_QueueNotifyOutOfBandLegacyNV(VkQueue q, uint32_t p) {
    native_device *d = find_device(dispatch_key(q)); if (!d || !d->queue_oob_legacy) return;
    pthread_mutex_lock(&d->queue_lock); d->queue_oob_legacy(q,p); pthread_mutex_unlock(&d->queue_lock);
}

static int native_cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static void native_swapchain_free(native_swapchain *sc) {
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if (sc->test_runtime) {
        gfg_test_live_destroy(sc->test_runtime);
        free(sc->test_runtime);
        sc->test_runtime=NULL;
    }
    if (sc->test_proxy) {
        gfg_output_proxy_destroy(sc->test_proxy,sc->test_proxy_destroy,
                                 &sc->test_proxy_ops);
        free(sc->test_proxy);
        sc->test_proxy=NULL;
    }
#endif
    if (sc->present_us && sc->present_samples > 2) {
        /* the first two presents carry one-time bridge creation; excluded */
        const uint32_t n = sc->present_samples - 2;
        double *v = sc->present_us + 2;
        qsort(v, n, sizeof(double), native_cmp_double);
        fprintf(stderr,
            "[gfg-native] present-timing path=%s %ux%u frames=%u median_us=%.1f p95_us=%.1f"
            " max_us=%.1f (CPU time inside the layer's vkQueuePresentKHR, incl. downstream presents)\n",
            sc->path ? sc->path : "none", sc->extent.width, sc->extent.height, n,
            v[n / 2], v[(uint32_t)((n - 1) * 0.95)], v[n - 1]);
    }
#ifdef GFG_NATIVE_WITH_TE
    gfg_output_stop(sc->output);
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
    if (test_observer_active) {
        if (!observer_finish()) abort();
        free(observer.images);
        test_observer_active=0;
    }
#endif
#ifdef GFG_NATIVE_WITH_CPU_BRIDGE
    if (sc->bridge) gfg_cpu_fg_destroy(sc->bridge);
#endif
    if (sc->gpu) gfg_gpu_fg_destroy(sc->gpu);
    gfg_output_destroy(sc->output);
    free(sc->acquire_semaphores);
    native_ids_destroy(sc->ids); /* stops the monitor before the swapchain goes */
    pthread_mutex_destroy(&sc->output_wsi);
#endif
    free(sc->present_us);
    free(sc->owned);
    free(sc);
}

static VKAPI_ATTR void VKAPI_CALL
native_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    if (!device) return;
    /* Keep the dispatch mapping alive until worker submissions are drained. */
    native_device *live = find_device(dispatch_key(device));
    if (live) {
        native_swapchain *sc = live->swapchains;
        live->swapchains = NULL;
        while (sc) { native_swapchain *next = sc->next; native_swapchain_free(sc); sc = next; }
    }
    native_device *found = NULL;
    pthread_mutex_lock(&map_lock);
    for (native_device **p = &devices; *p; p = &(*p)->next)
        if ((*p)->key == dispatch_key(device)) {
            found = *p; *p = found->next; break;
        }
    pthread_mutex_unlock(&map_lock);
    if (!found) return;
    native_swapchain *sc = found->swapchains;
    while (sc) {
        native_swapchain *next = sc->next;
        native_swapchain_free(sc);
        sc = next;
    }
    native_queue *queue = found->queues;
    while (queue) {
        native_queue *next = queue->next;
        free(queue);
        queue = next;
    }
    found->destroy(device, alloc);
    pthread_mutex_destroy(&found->lock);
    pthread_mutex_destroy(&found->queue_lock);
    free(found->queue_observer);
    free(found);
}

static int native_supported_sdr8_format(VkFormat format) {
    /* CPU transport handles byte-valued SDR images. SRGB is a transfer
       function, not a different channel count or per-pixel memory layout. */
    return format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_SRGB;
}

/* Deck OLED / Gamescope field logs (2026-10-17): the FROG WSI layer
 * supplies a packed-10 swapchain. Native's first present invokes
 * vkGetPhysicalDeviceFormatProperties via a lower instance dispatcher,
 * which provokes the loader's fatal Invalid physicalDevice trampoline.
 * Until physical-device handle provenance is fixed and tested *under the
 * actual Gamescope WSI stack*, leave this swapchain EXACTLY as the game
 * requested. No extra images, no generator, no format-property queries.
 * A safe real-frame fallback is preferable to aborting the game process.
 */
static int native_gamescope_packed10_unsafe(const VkSwapchainCreateInfoKHR *ci) {
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    return layers && strstr(layers, "VK_LAYER_FROG_gamescope_wsi_x86_64") &&
        (ci->imageFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
         ci->imageFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
}

static const char *native_ineligible_reason(const VkSwapchainCreateInfoKHR *ci) {
    /* These two flags select the game's presentation-ID and wait protocol.
     * They do NOT request protected memory or a different image layout.
     * Preserve them, and all original pNext structures, unchanged.
     * Never open the gate for protected or other unverified flag bits. */
    const VkSwapchainCreateFlagsKHR present_protocol =
        VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
        VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    if (ci->flags & ~(present_protocol | VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR))
        return "unsupported-swapchain-flags";
    /* DXVK uses UNORM/SRGB aliases of the same 8-bit storage. GSF copies
     * the base-format pixels; it never reinterprets the game's WSI views.
     * Keep the flag/list unchanged and reject unverified alias families. */
    if(ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        const VkBaseInStructure *node=ci->pNext;unsigned count=0;
        for(;node && count<16 && node->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            node=node->pNext,++count) {}
        if(!node || count==16)return "mutable-format-list-missing";
        const VkImageFormatListCreateInfo *list=(const VkImageFormatListCreateInfo *)node;
        if(!list->viewFormatCount || list->viewFormatCount>16 || !list->pViewFormats)
            return "mutable-format-list-invalid";
        VkFormat unorm,srgb;
        if(ci->imageFormat==VK_FORMAT_R8G8B8A8_UNORM || ci->imageFormat==VK_FORMAT_R8G8B8A8_SRGB) {
            unorm=VK_FORMAT_R8G8B8A8_UNORM;srgb=VK_FORMAT_R8G8B8A8_SRGB;
        }else if(ci->imageFormat==VK_FORMAT_B8G8R8A8_UNORM || ci->imageFormat==VK_FORMAT_B8G8R8A8_SRGB) {
            unorm=VK_FORMAT_B8G8R8A8_UNORM;srgb=VK_FORMAT_B8G8R8A8_SRGB;
        }else return "mutable-format-family-unsupported";
        int has_base=0;
        for(uint32_t i=0;i<list->viewFormatCount;++i) {
            VkFormat f=list->pViewFormats[i];
            if(f!=unorm && f!=srgb)return "mutable-format-alias-unsupported";
            if(f==ci->imageFormat)has_base=1;
        }
        if(!has_base)return "mutable-format-base-missing";
    }
    if (native_gamescope_packed10_unsafe(ci))
        return "gamescope-packed10-physical-dispatch-unverified";
    /* Gamescope's embedded WSI implements MAILBOX directly at the driver and
     * FIFO in its own per-vblank commit queue. MAILBOX can discard G1/G2; do
     * not claim 3x output unless the app requested the ordered FIFO contract. */
    if (ci->presentMode != VK_PRESENT_MODE_FIFO_KHR)
        return "non-fifo-present-mode";
    if (ci->imageArrayLayers != 1) return "array-layers";
    if (ci->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) return "sharing-mode";
    if (!ci->imageExtent.width || !ci->imageExtent.height) return "zero-extent";
    if (!native_supported_sdr8_format(ci->imageFormat) &&
            !(native_env_is_1("GFG_NATIVE_GPU_BRIDGE") &&
              (ci->imageFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
               ci->imageFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32)))
        return "unsupported-format";
    if(ci->imageColorSpace!=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
       ci->imageColorSpace!=VK_COLOR_SPACE_HDR10_ST2084_EXT)
        return "unsupported-color-space";
    if(ci->imageColorSpace==VK_COLOR_SPACE_HDR10_ST2084_EXT &&
       ci->imageFormat!=VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
       ci->imageFormat!=VK_FORMAT_A2R10G10B10_UNORM_PACK32)
        return "unsupported-pq-format";
    return NULL;
}

static int native_copy_usage_ready(VkImageUsageFlags usage) {
    const VkImageUsageFlags wanted =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return (usage & wanted) == wanted;
}

static int native_copy_bridge_requested(const native_device *d) {
    if (native_env_is_1("GFG_NATIVE_GPU_BRIDGE")) return d->gpu_capable;
#ifdef GFG_NATIVE_WITH_CPU_BRIDGE
    return native_env_is_1("GFG_NATIVE_CPU_BRIDGE");
#else
    return 0;
#endif
}

static void native_record_queue(native_device *d, VkQueue queue, uint32_t family) {
    if (!queue) return;
    pthread_mutex_lock(&d->lock);
    for (native_queue *node = d->queues; node; node = node->next)
        if (node->handle == queue) {
            node->family = family;
            pthread_mutex_unlock(&d->lock);
            return;
        }
    native_queue *node = calloc(1, sizeof(*node));
    if (node) {
        node->handle = queue;
        node->family = family;
        node->next = d->queues;
        d->queues = node;
    }
    pthread_mutex_unlock(&d->lock);
}
static VKAPI_ATTR void VKAPI_CALL
native_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
        VkQueue *queue) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->get_queue) return;
    d->get_queue(device, family, index, queue);
    if (queue) native_record_queue(d, *queue, family);
}
static VKAPI_ATTR void VKAPI_CALL
native_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info,
        VkQueue *queue) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->get_queue2) return;
    d->get_queue2(device, info, queue);
    if (queue && info) native_record_queue(d, *queue, info->queueFamilyIndex);
}

#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
/* The test virtual swapchain handle is private to this layer, so a Vulkan
 * validation layer ABOVE us cannot see its creation and may dereference a
 * nonexistent tracked VkSwapchainKHR. The explicit test environment's
 * VK_INSTANCE_LAYERS order lets us reject this KNOWN unsafe stack before
 * allocating or returning any virtual handle. This is NOT a general layer
 * negotiation solution; a production virtual WSI needs a proper contract. */
static int native_test_outer_validation_before_gsf(void) {
    const char *list=getenv("VK_INSTANCE_LAYERS");
    if(!list)return 0;
    int validation=-1,gsf=-1,index=0;
    for(const char *part=list;*part;) {
        part+=strspn(part,":;, \t");
        if(!*part)break;
        size_t len=strcspn(part,":;, \t");
        if(len==strlen("VK_LAYER_KHRONOS_validation") &&
           !strncmp(part,"VK_LAYER_KHRONOS_validation",len))validation=index;
        if(len==strlen("VK_LAYER_GFG_native") &&
           !strncmp(part,"VK_LAYER_GFG_native",len))gsf=index;
        part+=len;++index;
    }
    return validation>=0 && gsf>=0 && validation<gsf;
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL
native_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
    const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->create_swapchain || !ci || !out)
        return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    /* The app asks us to create its source WSI, and ONLY the internally
     * owned output is sent to downstream Vulkan. This entire branch is
     * absent from normal GFG builds: its callbacks currently exist only
     * in an isolated test harness, not a real game/Deck pipeline. */
    if (native_env_is_1("GFG_NATIVE_TEST_PROXY_CREATE")) {
        /* Never hand a private handle to an outer validation layer. A
         * controlled unsupported test must fail safely at CREATE time,
         * before acquiring source images or consuming any app semaphore. */
        if(native_test_outer_validation_before_gsf()) {
            fprintf(stderr,"GSF_LOADER_OWNER_REJECT reason=validation-layer-above-gsf"
                    " fail_closed=1 virtual_handle_returned=0\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (ci->oldSwapchain) {
            int old_is_virtual=0;
            pthread_mutex_lock(&d->lock);
            for(native_swapchain *scan=d->swapchains;scan;scan=scan->next)
                if(scan->handle==ci->oldSwapchain && scan->test_proxy)
                    old_is_virtual=1;
            pthread_mutex_unlock(&d->lock);
            /* NEVER leak virtual handle to downstream oldSwapchain WSI. */
            if(old_is_virtual)return VK_ERROR_OUT_OF_DATE_KHR;
        }
        /* CRITICAL: Gamescope may inject swapchain pNext metadata that
         * this experimental virtual WSI does not yet implement. Never
         * silently fall through to ordinary real WSI and falsely report
         * inserted frames when the user explicitly requests live output. */
        const int owner_live=native_env_is_1("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT");
        /* In SteamOS Gaming Mode a higher-priority Gamescope WSI layer
         * can convert the synthetic app's FIFO create to MAILBOX before
         * our test layer sees it. The offscreen source requires ordered
         * FIFO ownership, so ONLY in explicit test-only Gaming mode
         * request FIFO again from the downstream output swapchain.
         * This does not prove Gamescope's physical present ordering. */
        VkSwapchainCreateInfoKHR owned_ci=*ci,real_ci=*ci;
        const char *pair_reason=NULL;
        const int gamescope_mailbox_adapt=gfg_test_gamescope_prepare_pair(
            ci,owner_live,native_env_is_1("GFG_NATIVE_TEST_GAME_MODE"),
            &owned_ci,&real_ci,&pair_reason);
        if(gamescope_mailbox_adapt==1) {
            fprintf(stderr,
                "GSF_GAMESCOPE_WSI_PAIR virtual_mode=%d real_mode=%d"
                " real_pnext_type=%d preserved=1 test_only=1 oled_scanout=UNVERIFIED\n",
                (int)owned_ci.presentMode,(int)real_ci.presentMode,
                real_ci.pNext?
                  (int)((const VkBaseInStructure *)real_ci.pNext)->sType:-1);
        }
        const VkSwapchainCreateInfoKHR *virtual_ci=
            gamescope_mailbox_adapt==1?&owned_ci:ci;
        const char *unsupported=gfg_offscreen_unsupported(virtual_ci);
        if(gamescope_mailbox_adapt<0)
            unsupported=pair_reason?pair_reason:"invalid-gamescope-swapchain-chain";
        const int have_callbacks=d->test_proxy_signal && d->test_proxy_copy;
        const int have_dispatch=gfg_offscreen_dispatch_complete(&d->test_proxy_ops);
        if(owner_live &&
           (unsupported || !have_callbacks || !have_dispatch ||
            native_env_is_1("GFG_NATIVE_TEST_FORCE_PROXY_REJECT"))) {
            const VkBaseInStructure *next=(const VkBaseInStructure *)ci->pNext;
            fprintf(stderr,
                "GSF_LOADER_OWNER_REJECT reason=%s callbacks=%d dispatch=%d"
                " pnext_type=%d flags=0x%x format=%d colorspace=%d"
                " image_usage=0x%x present_mode=%d min_images=%u"
                " fail_closed=1 real_wsi_fallback=0\n",
                unsupported?unsupported:
                (!have_callbacks?"missing-acquire-copy-callback":
                 (!have_dispatch?"missing-gpu-dispatch":"injected-regression")),
                have_callbacks,have_dispatch,next?(int)next->sType:-1,
                (unsigned)ci->flags,(int)ci->imageFormat,
                (int)ci->imageColorSpace,(unsigned)ci->imageUsage,
                (int)ci->presentMode,ci->minImageCount);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (have_callbacks && have_dispatch && !unsupported) {
            gfg_output_proxy *proxy=calloc(1,sizeof(*proxy));
            native_swapchain *entry=calloc(1,sizeof(*entry));
            const int live=native_env_is_1("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT");
            gfg_test_loader_runtime *runtime=live?calloc(1,sizeof(*runtime)):NULL;
            if(proxy && entry && (!live || (runtime && d->test_loader_queue))) {
                d->test_proxy_user=live?(void*)runtime:d->test_proxy_user;
                VkResult vr=gfg_output_proxy_create_ex(proxy,device,d->physical,
                            virtual_ci,&real_ci,
                            d->create_swapchain,d->destroy_swapchain,
                            &d->test_proxy_ops,d->test_proxy_user,
                            d->test_proxy_signal,d->test_proxy_copy);
                if(vr==VK_SUCCESS && live)
                    vr=gfg_test_live_init(runtime,device,d->test_loader_queue,
                            d->test_loader_family,proxy,d->test_proxy_ops,virtual_ci);
                if(vr!=VK_SUCCESS && live)
                    fprintf(stderr,"GSF_LOADER_LIVE_FAILURE runtime_init=%d queue=%p GPU_source_count=%u output_present=%d\\n",
                            (int)vr,(void*)d->test_loader_queue,proxy->source.count,
                            proxy->output!=VK_NULL_HANDLE);
                if(vr==VK_SUCCESS) {
                    entry->test_runtime=runtime;
                    entry->handle=gfg_output_proxy_app_handle(proxy);
                    entry->virtual_app=&proxy->app;
                    entry->test_proxy=proxy;
                    entry->test_proxy_ops=d->test_proxy_ops;
                    entry->test_proxy_destroy=d->destroy_swapchain;
                    entry->format=virtual_ci->imageFormat;
                    entry->color_space=virtual_ci->imageColorSpace;
                    entry->extent=virtual_ci->imageExtent;
#ifdef GFG_NATIVE_WITH_TE
                    if(pthread_mutex_init(&entry->output_wsi,NULL))abort();
#endif
                    pthread_mutex_lock(&d->lock);
                    entry->next=d->swapchains;d->swapchains=entry;
                    pthread_mutex_unlock(&d->lock);
                    *out=entry->handle;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
                    if(native_env_is_1("GFG_NATIVE_TEST_LOADER_CREATE_PROBE"))
                        fprintf(stderr,"GSF_LOADER_CREATE_PROBE virtual_image_count=%u one_real_output_wsi=1 GPU_memory_allocated=1 PASS\n",
                                proxy->source.count);
                    if(live)
                        fprintf(stderr,"GSF_LOADER_LIVE_INIT virtual_images=%u one_real_output=1 GPU_worker=1\n",
                                proxy->source.count);
#endif
                    return VK_SUCCESS;
                }
            }
            if(live && (!runtime || !d->test_loader_queue))
                fprintf(stderr,"GSF_LOADER_LIVE_FAILURE missing_queue_or_memory queue=%p runtime=%p\\n",
                        (void*)d->test_loader_queue,(void*)runtime);
            if(runtime) {
                gfg_test_live_destroy(runtime);
                free(runtime);
                d->test_proxy_user=NULL;
            }
            if(proxy)gfg_output_proxy_destroy(proxy,d->destroy_swapchain,
                                              &d->test_proxy_ops);
            free(entry);free(proxy);
            if(live)return VK_ERROR_INITIALIZATION_FAILED;
            /* The ordinary unsupported path forwards the original WSI. */
        }
    }
#endif
#ifdef GFG_NATIVE_WITH_TE
    /* A successful create retires oldSwapchain immediately. Finish its jobs
     * before it becomes illegal to acquire the pending generated images. */
    if (ci->oldSwapchain) {
        pthread_mutex_lock(&d->lock);
        for (native_swapchain *old = d->swapchains; old; old = old->next)
            if (old->handle == ci->oldSwapchain && old->output)
                gfg_output_drain(old->output,0,UINT64_MAX,NULL);
        pthread_mutex_unlock(&d->lock);
    }
#endif
    int candidate = 0;
    const char *reason = NULL;
    VkImageUsageFlags supported_usage = 0;
    int caps_queried = 0;
    VkResult caps_result = VK_SUCCESS;
    VkResult extended_result = VK_SUCCESS;
    VkSwapchainCreateInfoKHR extended = *ci;
    gfg_create_chain create_copy;
    const int ordinary_mode = ci->presentMode == VK_PRESENT_MODE_MAILBOX_KHR ||
        ci->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR || ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    int mode_copy_ok = 1;
    if (ordinary_mode && native_env_is_1("GFG_NATIVE_FORCE_FIFO") &&
        native_env_is_1("GFG_NATIVE_GPU_BRIDGE") && d->gpu_capable)
        mode_copy_ok = gfg_create_fifo(&create_copy,&extended);
    VkSurfaceCapabilitiesKHR caps = {0};
    if (!d->instance || !d->instance->enabled) {
        reason = "layer-not-enabled";
    } else if (!native_copy_bridge_requested(d)) {
        reason = "frame-bridge-unavailable";
    } else if (!mode_copy_ok) {
        reason = "fifo-mode-list-incompatible-or-unknown-chain";
    } else if ((reason = native_ineligible_reason(&extended)) != NULL) {
        /* Keep the original WSI path untouched on unsupported shapes. */
    } else if (native_copy_usage_ready(ci->imageUsage)) {
        /* The game already requested both copy operations. Keep its
         * flags, pNext and usage; only the image count grows (below). */
        candidate = 1;
        reason = "existing-usage-ready";
    } else if (!d->instance->surface_caps) {
        reason = "surface-capabilities-entrypoint-missing";
    } else {
        caps_queried = 1;
        caps_result = d->instance->surface_caps(d->physical, ci->surface, &caps);
        supported_usage = caps.supportedUsageFlags;
        const VkImageUsageFlags wanted =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (caps_result != VK_SUCCESS) {
            reason = "surface-capabilities-query-failed";
        } else if ((caps.supportedUsageFlags & wanted) != wanted) {
            reason = "missing-transfer-src-or-dst-support";
        } else {
            extended.imageUsage |= wanted;
            candidate = 1;
            reason = "extended-usage-ready";
        }
    }
    const uint32_t spare_images=gfg_real_wsi_extra_images(getenv("GFG_NATIVE_MODE"));
    if (candidate) {
        /* Reserve one held original plus one real WSI image per generated
         * output: +2 for 2x, +3 for 3x. No virtual app-facing swapchain. */
        if (!caps_queried && d->instance->surface_caps) {
            caps_queried = 1;
            caps_result = d->instance->surface_caps(d->physical, ci->surface, &caps);
        }
        if (!gfg_real_wsi_can_extend(ci->minImageCount,
                caps_queried && caps_result==VK_SUCCESS ? caps.maxImageCount : 0,
                spare_images)) {
            candidate = 0;
            reason = "insufficient-image-count-cap";
        } else {
            const uint32_t wanted_images = ci->minImageCount + spare_images;
            if (wanted_images > extended.minImageCount) extended.minImageCount = wanted_images;
        }
    }
    if (native_env_is_1("GFG_NATIVE_REAL_WSI_PROBE")) {
        const VkBaseInStructure *head=(const VkBaseInStructure *)ci->pNext;
        fprintf(stderr,"GSF_REAL_WSI_GATE gpu_requested=%d gpu_capable=%d async=%d"
                " fifo_requested=%d original_mode=%d effective_mode=%d"
                " original_pnext=%d image_count_requested=%u spare=%u"
                " create_flags=0x%x image_format=%d image_usage=0x%x"
                " real_swapchain=1 virtual_swapchain=0 candidate=%d reason=%s\n",
                native_env_is_1("GFG_NATIVE_GPU_BRIDGE"),d->gpu_capable,
                d->async_output,native_env_is_1("GFG_NATIVE_FORCE_FIFO"),
                (int)ci->presentMode,(int)extended.presentMode,
                head?(int)head->sType:-1,
                candidate?extended.minImageCount:ci->minImageCount,
                spare_images,(unsigned)ci->flags,(int)ci->imageFormat,(unsigned)ci->imageUsage,
                candidate,reason?reason:"none");
    }
    VkResult result = d->create_swapchain(device, candidate ? &extended : ci,
                                           alloc, out);
    if (result != VK_SUCCESS && candidate) {
        // Never prevent a game from creating its original swapchain.
        extended_result = result;
        reason = "extended-create-failed";
        candidate = 0;
        *out = VK_NULL_HANDLE;
        result = d->create_swapchain(device, ci, alloc, out);
    }
    if (native_env_is_1("GFG_NATIVE_REAL_WSI_PROBE"))
        fprintf(stderr,"GSF_REAL_WSI_CREATE result=%d active_candidate=%d"
                " original_mode=%d mode_sent_downstream=%d"
                " source_real_swapchain=1 synthetic_insertions=UNVERIFIED\n",
                (int)result,candidate,(int)ci->presentMode,
                candidate?(int)extended.presentMode:(int)ci->presentMode);
    if (result != VK_SUCCESS) return result;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if(native_env_is_1("GFG_NATIVE_TEST_LOADER_CREATE_PROBE"))
        fprintf(stderr,"GSF_LOADER_CREATE_PROBE passthrough_real_wsi=1 reason=%s\n",
                reason?reason:"not-eligible");
#endif
    native_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        // The swapchain is valid, but native recording is unavailable.
        // Forwarding is still safe; no bookkeeping means no generation.
        return VK_SUCCESS;
    }
    sc->handle = *out;
#ifdef GFG_NATIVE_WITH_TE
    if (pthread_mutex_init(&sc->output_wsi, NULL)) { free(sc); return VK_SUCCESS; }
#endif
    sc->format = ci->imageFormat;
    sc->color_space = ci->imageColorSpace;
    sc->extent = ci->imageExtent;
    sc->copy_eligible = candidate;
    sc->present_mode = candidate ? extended.presentMode : ci->presentMode;
    sc->force_fifo = sc->present_mode != ci->presentMode;
    sc->create_flags = ci->flags;
    {
        const char *timing = getenv("GFG_NATIVE_TIMING");
        if (timing && timing[0] == '1')
            sc->present_us = calloc(NATIVE_TIMING_CAP, sizeof(double));
    }
    if (d->get_images) {
        uint32_t image_count = 0;
        if (d->get_images(device, *out, &image_count, NULL) == VK_SUCCESS &&
                image_count > 0 && image_count <= 256) {
            sc->owned = calloc(image_count, sizeof(*sc->owned));
            if (sc->owned) {
                sc->image_count = image_count;
#ifdef GFG_NATIVE_WITH_TE
                sc->acquire_semaphores=calloc(image_count,sizeof(*sc->acquire_semaphores));
#endif
            }
        }
    }
    pthread_mutex_lock(&d->lock);
    sc->next = d->swapchains;
    d->swapchains = sc;
    pthread_mutex_unlock(&d->lock);
    if (tracing())
        fprintf(stderr,
            "[gfg-native] swapchain %ux%u candidate=%d output=real-only"
            " reason=%s format=%d flags=0x%x layers=%u sharing=%d"
            " requested_usage=0x%x supported_usage=0x%x caps_queried=%d"
            " caps_result=%d extended_result=%d min_images=%u->%u images=%u present_mode=%d requested_present_mode=%d\n",
            sc->extent.width, sc->extent.height, sc->copy_eligible,
            reason ? reason : "accepted", (int)ci->imageFormat,
            (unsigned)ci->flags, ci->imageArrayLayers,
            (int)ci->imageSharingMode, (unsigned)ci->imageUsage,
            (unsigned)supported_usage, caps_queried,
            (int)caps_result, (int)extended_result, ci->minImageCount,
            candidate ? extended.minImageCount : ci->minImageCount, sc->image_count,
            (int)sc->present_mode, (int)ci->presentMode);
    return VK_SUCCESS;
}

#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
static gfg_virtual_wsi *native_test_find_virtual(native_device *d,VkSwapchainKHR h) {
    if(!d)return NULL;
    gfg_virtual_wsi *found=NULL;
    pthread_mutex_lock(&d->lock);
    for(native_swapchain *sc=d->swapchains;sc;sc=sc->next) {
        if(sc->handle==h && sc->virtual_app) {found=sc->virtual_app;break;}
    }
    pthread_mutex_unlock(&d->lock);
    return found;
}
/* Return a private, TEST-ONLY registration for one offscreen game image.
 * Never globally rewrite PRESENT on a real compositor output image. */
static gfg_virtual_wsi *native_test_find_virtual_image(native_device *d,VkImage img) {
    if(!d || !img)return NULL;
    gfg_virtual_wsi *found=NULL;
    pthread_mutex_lock(&d->lock);
    for(native_swapchain *sc=d->swapchains;sc;sc=sc->next) {
        if(!sc->virtual_app)continue;
        gfg_virtual_wsi *v=sc->virtual_app;
        for(unsigned i=0;i<v->source.count;++i) {
            if(v->images[i]==img) {found=v;break;}
        }
        if(found)break;
    }
    pthread_mutex_unlock(&d->lock);
    return found;
}
static void native_test_translate_image_barrier(native_device *d,
                                                 VkImageMemoryBarrier *b) {
    gfg_virtual_wsi *v=native_test_find_virtual_image(d,b->image);
    if(!v)return;
    gfg_virtual_layout_map map={.images=v->images,.count=v->source.count};
    b->oldLayout=gfg_vlayout_rewrite(&map,b->image,b->oldLayout,0);
    b->newLayout=gfg_vlayout_rewrite(&map,b->image,b->newLayout,1);
}
static void native_test_translate_image_barrier2(native_device *d,
                                                  VkImageMemoryBarrier2 *b) {
    gfg_virtual_wsi *v=native_test_find_virtual_image(d,b->image);
    if(!v)return;
    gfg_virtual_layout_map map={.images=v->images,.count=v->source.count};
    b->oldLayout=gfg_vlayout_rewrite(&map,b->image,b->oldLayout,0);
    b->newLayout=gfg_vlayout_rewrite(&map,b->image,b->newLayout,1);
}
static VKAPI_ATTR void VKAPI_CALL
native_CmdPipelineBarrier(VkCommandBuffer cmd,VkPipelineStageFlags src,
        VkPipelineStageFlags dst,VkDependencyFlags flags,
        uint32_t nmem,const VkMemoryBarrier *mem,
        uint32_t nbuf,const VkBufferMemoryBarrier *buf,
        uint32_t nimg,const VkImageMemoryBarrier *img) {
    native_device *d=find_device(dispatch_key(cmd));
    if(!d || !d->cmd_barrier)return;
    if(!nimg || !img) {
        d->cmd_barrier(cmd,src,dst,flags,nmem,mem,nbuf,buf,nimg,img);return;
    }
    if(nimg && SIZE_MAX/nimg<sizeof(*img))return;
    VkImageMemoryBarrier *owned=malloc((size_t)nimg*sizeof(*owned));
    if(!owned)return;
    memcpy(owned,img,(size_t)nimg*sizeof(*owned));
    for(uint32_t i=0;i<nimg;++i)native_test_translate_image_barrier(d,&owned[i]);
    d->cmd_barrier(cmd,src,dst,flags,nmem,mem,nbuf,buf,nimg,owned);
    free(owned);
}
static void native_test_cmd_barrier2(VkCommandBuffer cmd,
                                      const VkDependencyInfo *info,int is_khr) {
    native_device *d=find_device(dispatch_key(cmd));
    if(!d || (!d->cmd_barrier2 && !d->cmd_barrier2_khr) || !info)return;
    const uint32_t n=info->imageMemoryBarrierCount;
    if(n && (!info->pImageMemoryBarriers ||
             SIZE_MAX/n<sizeof(VkImageMemoryBarrier2)))return;
    VkImageMemoryBarrier2 *owned=n?malloc((size_t)n*sizeof(*owned)):NULL;
    if(n && !owned)return;
    if(n)memcpy(owned,info->pImageMemoryBarriers,(size_t)n*sizeof(*owned));
    for(uint32_t i=0;i<n;++i)native_test_translate_image_barrier2(d,&owned[i]);
    VkDependencyInfo copy=*info;
    if(n)copy.pImageMemoryBarriers=owned;
    if(is_khr && d->cmd_barrier2_khr)d->cmd_barrier2_khr(cmd,&copy);
    else if(d->cmd_barrier2)d->cmd_barrier2(cmd,&copy);
    else d->cmd_barrier2_khr(cmd,&copy);
    free(owned);
}
static VKAPI_ATTR void VKAPI_CALL
native_CmdPipelineBarrier2(VkCommandBuffer cmd,const VkDependencyInfo *info) {
    native_test_cmd_barrier2(cmd,info,0);
}
static VKAPI_ATTR void VKAPI_CALL
native_CmdPipelineBarrier2KHR(VkCommandBuffer cmd,const VkDependencyInfo *info) {
    native_test_cmd_barrier2(cmd,info,1);
}
static VKAPI_ATTR VkResult VKAPI_CALL
native_GetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR h,
                            uint32_t *count,VkImage *images) {
    native_device *d=find_device(dispatch_key(device));
    if(!d||!d->get_images)return VK_ERROR_INITIALIZATION_FAILED;
    gfg_virtual_wsi *v=native_test_find_virtual(d,h);
    return v?gfg_vwsi_get_images(v,h,count,images):
             d->get_images(device,h,count,images);
}
#endif
static VKAPI_ATTR void VKAPI_CALL
native_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
    const VkAllocationCallbacks *alloc) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->destroy_swapchain) return;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    /* Fake virtual swapchain handles NEVER reach a real WSI destroy.
     * The controlled test harness owns actual image allocation/teardown. */
    gfg_virtual_wsi *virtual_app=native_test_find_virtual(d,sc);
    if(virtual_app) {
        native_swapchain *owned=NULL;
        pthread_mutex_lock(&d->lock);
        for(native_swapchain **p=&d->swapchains;*p;p=&(*p)->next) {
            if((*p)->handle==sc && (*p)->test_proxy) {
                owned=*p;*p=owned->next;break;
            }
        }
        pthread_mutex_unlock(&d->lock);
        if(owned) {
            /* Test harness guarantees that no API calls or GPU commands
             * remain in flight when destroying this source. */
            native_swapchain_free(owned);
        } else gfg_vwsi_close(virtual_app);
        return;
    }
#endif
    pthread_mutex_lock(&d->lock);
    for (native_swapchain **p = &d->swapchains; *p; p = &(*p)->next)
        if ((*p)->handle == sc) {
            native_swapchain *old = *p;
            *p = old->next;
            native_swapchain_free(old);
            break;
        }
    pthread_mutex_unlock(&d->lock);
    d->destroy_swapchain(device, sc, alloc);
}

static void native_acquired(native_device *d, VkSwapchainKHR handle, uint32_t index, VkSemaphore semaphore) {
#ifndef GFG_NATIVE_WITH_TE
    (void)semaphore;
#endif
    if(native_env_is_1("GFG_TEST_CB_HANDLES")) fprintf(stderr,"GFG app acquired idx=%u sem=%p\n",index,(void*)semaphore);
    pthread_mutex_lock(&d->lock);
    for (native_swapchain *sc = d->swapchains; sc; sc = sc->next) {
        if (sc->handle != handle) continue;
        if (!sc->owned || index >= sc->image_count || sc->owned[index]) {
            ++sc->acquire_misses;
        } else {
            sc->owned[index] = 1;
            ++sc->acquired;
#ifdef GFG_NATIVE_WITH_TE
            if(sc->acquire_semaphores) sc->acquire_semaphores[index]=semaphore;
            if(sc->output) gfg_output_acquired(sc->output,index,semaphore);
#endif
        }
        break;
    }
    pthread_mutex_unlock(&d->lock);
}

#ifdef GFG_NATIVE_WITH_TE
static native_present_ids *native_find_ids(native_device *d, VkSwapchainKHR swapchain);
static gfg_output *native_find_output(native_device *d, VkSwapchainKHR swapchain) {
    gfg_output *out = NULL;
    pthread_mutex_lock(&d->lock);
    for (native_swapchain *sc = d->swapchains; sc; sc = sc->next)
        if (sc->handle == swapchain) { out = sc->output; break; }
    pthread_mutex_unlock(&d->lock); return out;
}
static uint64_t native_remaining(uint64_t start, uint64_t timeout) {
    const uint64_t elapsed = native_now_ns() - start;
    return timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0;
}
static int native_semaphore_reusable(native_device *d,VkSemaphore semaphore) {
    int ready=1;pthread_mutex_lock(&d->lock);
    for(native_swapchain *sc=d->swapchains;sc;sc=sc->next)
        if(sc->output && !gfg_output_sem_reusable(sc->output,semaphore)) ready=0;
    pthread_mutex_unlock(&d->lock);return ready;
}
/* Async acquires never hold the WSI mutex while waiting for an image that
 * the output thread must enqueue. Timeout 0 remains strictly nonblocking. */
static VkResult native_async_acquire(native_device *d, gfg_output *output,
        VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
        const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
    const uint64_t start = native_now_ns();
    native_present_ids *ids = output ? NULL : native_find_ids(d,swapchain);
    pthread_mutex_t *lock = output ? gfg_output_wsi(output) : ids ? &ids->wsi_lock : NULL;
    for (;;) {
        VkResult status = output ? gfg_output_status(output) : VK_SUCCESS;
        if (status < 0) return status;
        VkResult r = VK_NOT_READY;
        /* A compositor-released image should go to the waiting output first. */
        if (native_semaphore_reusable(d,semaphore) && (!output || !gfg_output_waiting_image(output)) &&
            (!lock || pthread_mutex_trylock(lock) == 0)) {
            if (info) {
                VkAcquireNextImageInfoKHR immediate = *info; immediate.timeout = 0;
                r = d->acquire2(d->handle, &immediate, index);
            } else r = d->acquire(d->handle, swapchain, 0, semaphore, fence, index);
            if(lock) pthread_mutex_unlock(lock);
        }
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (!timeout) return VK_NOT_READY;
        if (!native_remaining(start,timeout)) return VK_TIMEOUT;
        struct timespec delay = {.tv_nsec = 250000}; nanosleep(&delay,NULL);
    }
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL
native_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
        uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->acquire) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    gfg_virtual_wsi *v=native_test_find_virtual(d,swapchain);
    if(v)return gfg_vwsi_acquire(v,swapchain,timeout,semaphore,fence,index);
#endif
    VkResult result;
#ifdef GFG_NATIVE_WITH_TE
    gfg_output *output = native_find_output(d,swapchain);
    if (d->async_output) result = native_async_acquire(d,output,swapchain,timeout,semaphore,fence,NULL,index);
    else {
    native_present_ids *ids = d->present_trace ? native_find_ids(d, swapchain) : NULL;
    if (ids) pthread_mutex_lock(&ids->wsi_lock);
#endif
    result = d->acquire(device, swapchain, timeout, semaphore, fence, index);
#ifdef GFG_NATIVE_WITH_TE
    if (ids) pthread_mutex_unlock(&ids->wsi_lock);
    }
#endif
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && index)
        native_acquired(d, swapchain, *index, semaphore);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
native_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info,
        uint32_t *index) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->acquire2) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    gfg_virtual_wsi *v=info?native_test_find_virtual(d,info->swapchain):NULL;
    if(v)return gfg_vwsi_acquire2(v,info,index);
#endif
    VkResult result;
#ifdef GFG_NATIVE_WITH_TE
    gfg_output *output = info ? native_find_output(d,info->swapchain) : NULL;
    if (d->async_output && info) result = native_async_acquire(d,output,info->swapchain,info->timeout,
                                             info->semaphore,info->fence,info,index);
    else {
    native_present_ids *ids = d->present_trace && info
        ? native_find_ids(d, info->swapchain) : NULL;
    if (ids) pthread_mutex_lock(&ids->wsi_lock);
#endif
    result = d->acquire2(device, info, index);
#ifdef GFG_NATIVE_WITH_TE
    if (ids) pthread_mutex_unlock(&ids->wsi_lock);
    }
#endif
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && index && info)
        native_acquired(d, info->swapchain, *index, info->semaphore);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
native_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    struct timespec source_time;clock_gettime(CLOCK_MONOTONIC,&source_time);
    const uint64_t source_ns=(uint64_t)source_time.tv_sec*1000000000ull+(uint64_t)source_time.tv_nsec;
#ifndef GFG_NATIVE_WITH_TE
    (void)source_ns;
#endif
    native_device *d = find_device(dispatch_key(queue));
    if (!d || !d->present) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if(info && info->swapchainCount && info->pSwapchains) {
        gfg_virtual_wsi *virtual_app=NULL;
        for(uint32_t i=0;i<info->swapchainCount;++i) {
            virtual_app=native_test_find_virtual(d,info->pSwapchains[i]);
            if(virtual_app)break;
        }
        if(virtual_app) {
            /* A mixed app+output or multi-app present cannot be forwarded
             * downstream: the fake handle would crash the Vulkan driver.
             * Fail before consuming *any* application wait semaphore. */
            if(info->swapchainCount!=1)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            uint64_t token=0;
            return gfg_vwsi_present(virtual_app,info,&token);
        }
    }
#endif
    /* Record only the game's acquired indices. The async path consumes its
     * semaphore waits before returning and owns a private metadata copy. */
    if (info && info->pImageIndices && info->pSwapchains) {
        pthread_mutex_lock(&d->lock);
        for (uint32_t n = 0; n < info->swapchainCount; ++n) {
            for (native_swapchain *sc = d->swapchains; sc; sc = sc->next) {
                if (sc->handle != info->pSwapchains[n]) continue;
                const uint32_t idx = info->pImageIndices[n];
                if (!sc->owned || idx >= sc->image_count || !sc->owned[idx])
                    ++sc->present_misses;
                else sc->owned[idx] = 0;
                break;
            }
        }
        pthread_mutex_unlock(&d->lock);
    }
    /* Unsupported shapes keep the real present; eligible single-swapchain
     * jobs use the native GPU transport below. */
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    struct timespec present_start;
    clock_gettime(CLOCK_MONOTONIC, &present_start);
    const char *path = "none";
#ifdef GFG_NATIVE_WITH_TE
#ifdef GFG_NATIVE_WITH_CPU_BRIDGE
    gfg_cpu_fg *bridge = NULL;
    const char *flag = getenv("GFG_NATIVE_CPU_BRIDGE");
#endif
    gfg_gpu_fg *gpu = NULL;
    gfg_output *output = NULL;
    native_present_ids *sync_ids = NULL;
    gfg_present_id_hooks ids = {0};
    /* GPU bridge takes precedence; when requested it never falls back to
     * the CPU bridge (a failed GPU bridge means real-frame pass-through). */
    const int want_gpu = native_env_is_1("GFG_NATIVE_GPU_BRIDGE");
    if (want_gpu && info && info->swapchainCount == 1 && info->pSwapchains &&
        info->pImageIndices && d->instance && d->instance->enabled) {
        pthread_mutex_lock(&d->lock);
        for (native_swapchain *sc = d->swapchains; sc; sc = sc->next) {
            if (sc->handle != info->pSwapchains[0]) continue;
            if (sc->copy_eligible && d->gpu_capable && !sc->gpu_attempted) {
                sc->gpu_attempted = 1;
                for (native_queue *q = d->queues; q; q = q->next) {
                    if (q->handle != queue) continue;
                    gfg_gpu_fg_create_info gi = {
                        .frontend_hint=d->instance->frontend_hint,.engine_version=d->instance->engine_version,
                        .instance_api_version=d->instance->api_version,
                        .instance = d->instance->handle, .gipa = d->instance->gipa,
                        .physical = d->physical, .device = d->handle,
                        .gdpa = d->async_output ? native_worker_gdpa : d->gdpa,
                        .swapchain = sc->handle, .extent = sc->extent, .format = sc->format, .color_space=sc->color_space,
                        .present_mode = sc->present_mode, .force_fifo=sc->force_fifo, .queue = queue,
                        .family = q->family, .semaphore_fd = d->semaphore_fd, .display_timing=d->display_timing, .calibrated_mode=d->calibrated_mode,
                        .set_loader_data = d->set_loader_data,
                        .queue_probe = d->queue_observer ? native_queue_probe : NULL};
                    sc->gpu = gfg_gpu_fg_create(&gi);
                    if (native_env_is_1("GFG_NATIVE_REAL_WSI_PROBE"))
                        fprintf(stderr,"GSF_REAL_WSI_GPU_BRIDGE_CREATE"
                                " active=%d real_swapchain=1 mode=%d"
                                " async=%d requested_multiplier=%s\n",
                                sc->gpu!=NULL,(int)sc->present_mode,
                                d->async_output,getenv("GFG_NATIVE_MODE")?
                                  getenv("GFG_NATIVE_MODE"):"3");
                    if (sc->gpu) {
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
                        uint32_t n=sc->image_count;
                        VkImage *images=calloc(n,sizeof(*images));
#define OBS_LOAD(name) observer_api.name=(PFN_vk##name)d->gdpa(d->handle,"vk" #name);
                        OBS_FUNCS(OBS_LOAD)
#undef OBS_LOAD
                        observer_api.QueueSubmit=native_QueueSubmit;
                        observer_set_loader_data=d->set_loader_data;
                        PFN_vkGetPhysicalDeviceMemoryProperties props=(PFN_vkGetPhysicalDeviceMemoryProperties)
                            d->instance->gipa(d->instance->handle,"vkGetPhysicalDeviceMemoryProperties");
                        props(d->physical,&observer.memory_props);
                        if (!images || d->get_images(d->handle,sc->handle,&n,images)!=VK_SUCCESS ||
                            !observer_start(d->instance->handle,d->physical,d->handle,queue,
                                sc->handle,sc->extent,sc->format,images,n,q->family,native_locked_present)) abort();
                        test_observer_active=1;
#endif
                        sc->ids = native_ids_create(d, sc);
                        if (d->async_output && sc->acquire_semaphores) {
                            const gfg_present_id_hooks hooks = {
                                .mode = sc->ids ? sc->ids->mode : 0,
                                .assign = sc->ids ? native_ids_assign : NULL, .user = sc->ids};
                            sc->output = gfg_output_create(sc->gpu,queue,native_serialized_present,&hooks,
                                sc->ids ? &sc->ids->wsi_lock : &sc->output_wsi,
                                d->handle,native_worker_gdpa,sc->image_count,sc->acquire_semaphores);
                        }
                    }
                    break;
                }
                if (!sc->gpu)
                    fprintf(stderr, "[gfg-native] GPU bridge not available; real-frame pass-through\n");
            } else if (!sc->gpu_attempted && !d->gpu_capable) {
                sc->gpu_attempted = 1;
                fprintf(stderr, "[gfg-native] GPU bridge not available (device lacks"
                        " external memory fd / timeline); real-frame pass-through\n");
            }
            gpu = sc->gpu;
            output = sc->output;
            sync_ids = sc->ids;
            if (sc->ids) {
                ids.mode = sc->ids->mode;
                ids.assign = native_ids_assign;
                ids.user = sc->ids;
            }
            break;
        }
        pthread_mutex_unlock(&d->lock);
    }
#ifdef GFG_NATIVE_WITH_CPU_BRIDGE
    if (!want_gpu && info && info->swapchainCount == 1 && info->pSwapchains &&
        info->pImageIndices && flag && !strcmp(flag, "1") &&
        d->instance && d->instance->enabled) {
        pthread_mutex_lock(&d->lock);
        for (native_swapchain *sc = d->swapchains; sc; sc = sc->next) {
            if (sc->handle != info->pSwapchains[0]) continue;
            if (sc->copy_eligible && !sc->bridge_attempted) {
                sc->bridge_attempted = 1;
                for (native_queue *q = d->queues; q; q = q->next) {
                    if (q->handle != queue) continue;
                    sc->bridge = gfg_cpu_fg_create(d->instance->handle,
                        d->instance->gipa, d->physical, d->handle,
                        d->gdpa, sc->handle, sc->extent, sc->format,
                        queue, q->family);
                    break;
                }
                if (!sc->bridge)
                    fprintf(stderr,"[gfg-native] CPU bridge not available; real-frame pass-through\n");
            }
            bridge = sc->bridge;
            break;
        }
        pthread_mutex_unlock(&d->lock);
    }
    #endif
    /* The optional wait monitor uses the same swapchain as QueuePresent. Its
     * zero-timeout polls share this lock to satisfy Vulkan's host sync rule. */
    int queued = 0;
    if (output) {
        result = gfg_output_submit(output,info,source_ns,&queued);
        if (queued) path = "gpu-async";
        else gfg_output_drain(output,0,UINT64_MAX,NULL);
    } else if (d->async_output) native_drain_device(d); /* multi-swapchain / fallback order */
    if (!queued) {
    if (sync_ids && !output) pthread_mutex_lock(&sync_ids->wsi_lock);
    if (gpu) {
        path = "gpu";
        result = gfg_gpu_fg_present(gpu, queue, info,
                                    d->async_output ? native_serialized_present : d->present, &ids);
    }
#ifdef GFG_NATIVE_WITH_CPU_BRIDGE
    else if (bridge) {
        path = "cpu";
        result = gfg_cpu_fg_present(bridge, info, d->present);
    }
#endif
    else {
        result = native_serialized_present(queue, info);
    }
    if (sync_ids && !output) pthread_mutex_unlock(&sync_ids->wsi_lock);
    }
#else
    result = native_serialized_present(queue, info);
#endif
    struct timespec present_end;
    clock_gettime(CLOCK_MONOTONIC, &present_end);
    const double present_us = 1e6 * (double)(present_end.tv_sec - present_start.tv_sec) +
        (double)(present_end.tv_nsec - present_start.tv_nsec) / 1e3;
    if (info && info->pSwapchains) {
        pthread_mutex_lock(&d->lock);
        for (uint32_t n = 0; n < info->swapchainCount; ++n)
            for (native_swapchain *sc = d->swapchains; sc; sc = sc->next)
                if (sc->handle == info->pSwapchains[n]) {
                    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++sc->presents;
                    sc->path = path;
                    if (sc->present_us && sc->present_samples < NATIVE_TIMING_CAP)
                        sc->present_us[sc->present_samples++] = present_us;
                    break;
                }
        pthread_mutex_unlock(&d->lock);
    }
    return result;
}

#ifdef GFG_NATIVE_WITH_TE
static native_present_ids *native_find_ids(native_device *d, VkSwapchainKHR swapchain) {
    native_present_ids *ids = NULL;
    pthread_mutex_lock(&d->lock);
    for (native_swapchain *sc = d->swapchains; sc; sc = sc->next)
        if (sc->handle == swapchain) { ids = sc->ids; break; }
    pthread_mutex_unlock(&d->lock);
    return ids;
}
static VkResult native_output_wait(native_device *d, gfg_output *output, VkSwapchainKHR sc,
        uint64_t id, uint64_t timeout, const VkPresentWait2InfoKHR *info) {
    const uint64_t start = native_now_ns();
    VkResult r = gfg_output_drain(output,id,timeout,NULL);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_t *lock = gfg_output_wsi(output);
    for (;;) {
        r = VK_TIMEOUT;
        if (!pthread_mutex_trylock(lock)) {
            if (info) {
                VkPresentWait2InfoKHR poll = *info; poll.timeout = 0;
                r = d->wait_present2(d->handle,sc,&poll);
            } else r = d->wait_present(d->handle,sc,id,0);
            pthread_mutex_unlock(lock);
        }
        if (r != VK_TIMEOUT) return r;
        if (!timeout || !native_remaining(start,timeout)) return VK_TIMEOUT;
        struct timespec delay = {.tv_nsec = 250000}; nanosleep(&delay,NULL);
    }
}
#include "wsi_queries.inc"
static VKAPI_ATTR VkResult VKAPI_CALL
native_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t present_id,
                         uint64_t timeout) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->wait_present) return VK_ERROR_INITIALIZATION_FAILED;
    native_present_ids *ids = native_find_ids(d, swapchain);
    gfg_output *output = native_find_output(d, swapchain);
    if (output && !ids) return native_output_wait(d,output,swapchain,present_id,timeout,NULL);
    if (output) {
        uint64_t remaining;
        VkResult r = gfg_output_drain(output,0,timeout,&remaining);
        if (r != VK_SUCCESS) return r;
        timeout = remaining;
    }
    if (!ids) return d->wait_present(device, swapchain, present_id, timeout);
    if (!present_id) {
        pthread_mutex_lock(&ids->wsi_lock);
        VkResult direct = d->wait_present(device, swapchain, present_id, timeout);
        pthread_mutex_unlock(&ids->wsi_lock);
        return direct;
    }
    uint64_t layer_id = 0, remaining = 0;
    VkResult r = native_ids_translate(ids, present_id, timeout, &layer_id, &remaining);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&ids->wsi_lock);
    r = d->wait_present(device, swapchain, layer_id, remaining);
    pthread_mutex_unlock(&ids->wsi_lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL
native_WaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain,
                          const VkPresentWait2InfoKHR *info) {
    native_device *d = find_device(dispatch_key(device));
    if (!d || !d->wait_present2) return VK_ERROR_INITIALIZATION_FAILED;
    native_present_ids *ids = native_find_ids(d, swapchain);
    gfg_output *output = native_find_output(d, swapchain);
    if (output && !ids && info) return native_output_wait(d,output,swapchain,info->presentId,info->timeout,info);
    VkPresentWait2InfoKHR after_drain;
    if (output && info) {
        uint64_t remaining;
        VkResult r = gfg_output_drain(output,0,info->timeout,&remaining);
        if (r != VK_SUCCESS) return r;
        after_drain = *info; after_drain.timeout = remaining; info = &after_drain;
    }
    if (!ids) return d->wait_present2(device, swapchain, info);
    if (!info || !info->presentId) {
        pthread_mutex_lock(&ids->wsi_lock);
        VkResult direct = d->wait_present2(device, swapchain, info);
        pthread_mutex_unlock(&ids->wsi_lock);
        return direct;
    }
    uint64_t layer_id = 0, remaining = 0;
    VkResult r = native_ids_translate(ids, info->presentId, info->timeout, &layer_id, &remaining);
    if (r != VK_SUCCESS) return r;
    VkPresentWait2InfoKHR translated = *info; /* pNext kept */
    translated.presentId = layer_id;
    translated.timeout = remaining;
    pthread_mutex_lock(&ids->wsi_lock);
    r = d->wait_present2(device, swapchain, &translated);
    pthread_mutex_unlock(&ids->wsi_lock);
    return r;
}
#endif

#define HOOK(name) if (!strcmp(symbol, "vk" #name)) return (PFN_vkVoidFunction)native_##name

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
native_gdpa(VkDevice device, const char *symbol) {
    if (!symbol) return NULL;
    if (!strcmp(symbol, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)native_gdpa;
    HOOK(DestroyDevice);
    if (!device) return NULL;
    native_device *d = find_device(dispatch_key(device));
    if (!d) return NULL;
    if (d->get_queue) HOOK(GetDeviceQueue);
    if (d->get_queue2) HOOK(GetDeviceQueue2);
    if (d->create_swapchain) HOOK(CreateSwapchainKHR);
    if (d->destroy_swapchain) HOOK(DestroySwapchainKHR);
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if(d->get_images) HOOK(GetSwapchainImagesKHR);
    if(d->cmd_barrier) HOOK(CmdPipelineBarrier);
    if(d->cmd_barrier2) HOOK(CmdPipelineBarrier2);
    if(d->cmd_barrier2_khr) HOOK(CmdPipelineBarrier2KHR);
#endif
    if (d->present) HOOK(QueuePresentKHR);
    if (d->acquire) HOOK(AcquireNextImageKHR);
    if (d->acquire2) HOOK(AcquireNextImage2KHR);
    if(d->queue_observer) {
        if (d->fence_status) HOOK(GetFenceStatus);
        if (d->wait_fences) HOOK(WaitForFences);
        if (d->import_fence_fd) HOOK(ImportFenceFdKHR);
        if (d->get_fence_fd) HOOK(GetFenceFdKHR);
        if (d->reset_fences) HOOK(ResetFences);
        if (d->destroy_fence) HOOK(DestroyFence);
    }
    if (d->submit) HOOK(QueueSubmit);
    if (d->submit2) HOOK(QueueSubmit2);
    if (d->submit2_khr) HOOK(QueueSubmit2KHR);
    if (d->bind_sparse) HOOK(QueueBindSparse);
    if (d->queue_idle) HOOK(QueueWaitIdle);
    if (d->device_idle) HOOK(DeviceWaitIdle);
    if (d->queue_label_begin) HOOK(QueueBeginDebugUtilsLabelEXT);
    if (d->queue_label_end) HOOK(QueueEndDebugUtilsLabelEXT);
    if (d->queue_label_insert) HOOK(QueueInsertDebugUtilsLabelEXT);
    if (d->queue_perf) HOOK(QueueSetPerformanceConfigurationINTEL);
    if (d->queue_hint) HOOK(QueueSetPerfHintQCOM);
    if (d->queue_oob) HOOK(QueueNotifyOutOfBandNV);
    if (d->queue_oob_legacy) HOOK(QueueNotifyOutOfBandLegacyNV);
#ifdef GFG_NATIVE_WITH_TE
    if (d->gdpa(device,"vkGetSwapchainStatusKHR")) HOOK(GetSwapchainStatusKHR);
    if (d->gdpa(device,"vkGetRefreshCycleDurationGOOGLE")) HOOK(GetRefreshCycleDurationGOOGLE);
    if (d->gdpa(device,"vkGetPastPresentationTimingGOOGLE")) HOOK(GetPastPresentationTimingGOOGLE);
    if (d->gdpa(device,"vkSetSwapchainPresentTimingQueueSizeEXT")) HOOK(SetSwapchainPresentTimingQueueSizeEXT);
    if (d->gdpa(device,"vkGetSwapchainTimingPropertiesEXT")) HOOK(GetSwapchainTimingPropertiesEXT);
    if (d->gdpa(device,"vkGetSwapchainTimeDomainPropertiesEXT")) HOOK(GetSwapchainTimeDomainPropertiesEXT);
    if (d->gdpa(device,"vkGetPastPresentationTimingEXT")) HOOK(GetPastPresentationTimingEXT);
    if (d->gdpa(device,"vkReleaseSwapchainImagesKHR")) HOOK(ReleaseSwapchainImagesKHR);
    if (d->gdpa(device,"vkReleaseSwapchainImagesEXT")) HOOK(ReleaseSwapchainImagesEXT);
    if (d->gdpa(device,"vkSetHdrMetadataEXT")) HOOK(SetHdrMetadataEXT);
    if (d->wait_present) HOOK(WaitForPresentKHR);
    if (d->wait_present2) HOOK(WaitForPresent2KHR);
#endif
    return d->gdpa(device, symbol);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
native_gipa(VkInstance instance, const char *symbol) {
    if (!symbol) return NULL;
    if (!strcmp(symbol, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)native_gipa;
    if (!strcmp(symbol, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)native_gdpa;
    HOOK(CreateInstance);
    HOOK(DestroyInstance);
    HOOK(CreateDevice);
    if (!instance) return NULL;
    if(native_env_is_1("GFG_NATIVE_QUEUE_OBSERVE")) {
        HOOK(GetFenceStatus); HOOK(WaitForFences);
        HOOK(ImportFenceFdKHR); HOOK(GetFenceFdKHR);
        HOOK(ResetFences); HOOK(DestroyFence);
    }
    HOOK(QueueSubmit); HOOK(QueueSubmit2); HOOK(QueueSubmit2KHR); HOOK(QueueBindSparse);
    HOOK(QueueWaitIdle); HOOK(DeviceWaitIdle);
    HOOK(QueueBeginDebugUtilsLabelEXT); HOOK(QueueEndDebugUtilsLabelEXT); HOOK(QueueInsertDebugUtilsLabelEXT);
    HOOK(QueueSetPerformanceConfigurationINTEL); HOOK(QueueSetPerfHintQCOM);
    HOOK(QueueNotifyOutOfBandNV); HOOK(QueueNotifyOutOfBandLegacyNV);
    // Instance-level lookup of device commands must not bypass our layer.
    // Many Vulkan frameworks resolve WSI commands via vkGetInstanceProcAddr.
    if (!strcmp(symbol, "vkGetDeviceQueue"))
        return (PFN_vkVoidFunction)native_GetDeviceQueue;
    if (!strcmp(symbol, "vkGetDeviceQueue2"))
        return (PFN_vkVoidFunction)native_GetDeviceQueue2;
    if (!strcmp(symbol, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)native_CreateSwapchainKHR;
    if (!strcmp(symbol, "vkDestroySwapchainKHR"))
        return (PFN_vkVoidFunction)native_DestroySwapchainKHR;
#ifdef GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
    if (!strcmp(symbol, "vkGetSwapchainImagesKHR"))
        return (PFN_vkVoidFunction)native_GetSwapchainImagesKHR;
    if (!strcmp(symbol, "vkCmdPipelineBarrier"))
        return (PFN_vkVoidFunction)native_CmdPipelineBarrier;
    if (!strcmp(symbol, "vkCmdPipelineBarrier2"))
        return (PFN_vkVoidFunction)native_CmdPipelineBarrier2;
    if (!strcmp(symbol, "vkCmdPipelineBarrier2KHR"))
        return (PFN_vkVoidFunction)native_CmdPipelineBarrier2KHR;
#endif
    if (!strcmp(symbol, "vkQueuePresentKHR"))
        return (PFN_vkVoidFunction)native_QueuePresentKHR;
    if (!strcmp(symbol, "vkAcquireNextImageKHR"))
        return (PFN_vkVoidFunction)native_AcquireNextImageKHR;
    if (!strcmp(symbol, "vkAcquireNextImage2KHR"))
        return (PFN_vkVoidFunction)native_AcquireNextImage2KHR;
#ifdef GFG_NATIVE_WITH_TE
    HOOK(GetSwapchainStatusKHR); HOOK(GetRefreshCycleDurationGOOGLE); HOOK(GetPastPresentationTimingGOOGLE);
    HOOK(SetSwapchainPresentTimingQueueSizeEXT); HOOK(GetSwapchainTimingPropertiesEXT);
    HOOK(GetSwapchainTimeDomainPropertiesEXT); HOOK(GetPastPresentationTimingEXT);
    HOOK(ReleaseSwapchainImagesKHR); HOOK(ReleaseSwapchainImagesEXT); HOOK(SetHdrMetadataEXT);
    if (!strcmp(symbol, "vkWaitForPresentKHR"))
        return (PFN_vkVoidFunction)native_WaitForPresentKHR;
    if (!strcmp(symbol, "vkWaitForPresent2KHR"))
        return (PFN_vkVoidFunction)native_WaitForPresent2KHR;
#endif
    native_instance *i = find_instance(dispatch_key(instance));
    return i ? i->gipa(instance, symbol) : NULL;
}
#undef HOOK

#ifdef GFG_NATIVE_WITH_TE
/* Test hook: process-wide GPU bridge counters (see gpu_bridge.h). */
EXPORT void gfg_native_gpu_bridge_stats(gfg_gpu_stats *out) { gfg_gpu_fg_stats(out); }
#endif

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return native_gipa(instance, name);
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return native_gdpa(device, name);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = native_gipa;
    v->pfnGetDeviceProcAddr = native_gdpa;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
