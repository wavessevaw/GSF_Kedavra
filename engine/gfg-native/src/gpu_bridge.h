/* SPDX-License-Identifier: GPL-3.0-or-later
 * GFG Native GPU-resident frame-generation transport (GFG_NATIVE_GPU_BRIDGE=1).
 * See gpu_bridge.c for the dataflow and the synchronization contract.
 */
#ifndef GFG_GPU_BRIDGE_H
#define GFG_GPU_BRIDGE_H
#include <stdint.h>
#include "queue_observation.h"
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

typedef struct gfg_gpu_fg gfg_gpu_fg;

typedef struct {
    const char *frontend_hint; /* declared VkApplicationInfo engine, not a Proton/version detector */
    uint32_t engine_version, instance_api_version;
    gsf_queue_probe_fn queue_probe; /* diagnostic only, no admission input */
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;   /* next-layer instance dispatch */
    VkPhysicalDevice physical;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;     /* next-layer device dispatch */
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    VkFormat format;
    VkColorSpaceKHR color_space;       /* packed10 alone does not imply HDR PQ */
    VkPresentModeKHR present_mode;
    int force_fifo;
    VkQueue queue;                    /* the app's present queue */
    uint32_t family;
    int display_timing;               /* GOOGLE extension enabled: 1 app namespace, 2 private */
    int semaphore_fd;                 /* VK_KHR_external_semaphore_fd enabled on the app device */
    int calibrated_mode;              /* calibrated timestamps enabled on device: KHR=1, EXT=2 */
    PFN_vkSetDeviceLoaderData set_loader_data; /* dispatch for layer-allocated command buffers */
} gfg_gpu_fg_create_info;

/* Process-wide counters (all bridges). Exposed to tests through the layer's
 * exported gfg_native_gpu_bridge_stats(). Field order is ABI for the test. */
typedef struct {
    uint64_t version;                 /* = 1 */
    uint64_t bridges_created;
    uint64_t sync_mode;               /* 1: shared timeline fd, 3: application device */
    uint64_t real_presents;           /* real presents that went through a bridge */
    uint64_t synthetic_presents;      /* synthetic presents accepted by the WSI (SUCCESS/SUBOPTIMAL) */
    uint64_t skipped_synthetic;       /* synthetic slots not presented after the first frame */
    uint64_t te_busy;                 /* te_submit_frame returned VK_NOT_READY */
    uint64_t ring_busy;               /* all transport ring slots were still in flight */
    uint64_t acquire_not_ready;       /* vkAcquireNextImageKHR(timeout 0) NOT_READY/TIMEOUT */
    uint64_t holds;                   /* TE pairs submitted as hold (first frame / discontinuity) */
    uint64_t errors;
    uint64_t present_path_host_pixel_reads; /* mapped pixel reads inside vkQueuePresentKHR outside verify */
    uint64_t present_path_cpu_waits;  /* vkWaitForFences/vkQueueWaitIdle/vkWaitSemaphores in present */
    uint64_t teardown_cpu_waits;      /* the same calls at creation/destroy (not steady state) */
    uint64_t verify_checks;
    uint64_t verify_distinct;
    uint64_t verify_host_reads;       /* GFG_NATIVE_GPU_BRIDGE_VERIFY readbacks (test-only, off hot path) */
} gfg_gpu_stats;

/* Present-ID ownership (B4). When mode != 0 the layer owns the strictly
 * increasing per-swapchain ID sequence: EVERY present the bridge issues
 * (G1, G2, the real one, pass-through) gets assign()'s id; the app's id on a
 * real present is handed to assign() so the layer can translate the app's own
 * vkWaitForPresent(2)KHR calls. kind: 0 real, 1 G1, 2 G2, 3 recovery duplicate (not synthetic). */
typedef struct {
    int mode;                         /* 0 none, 1 VkPresentIdKHR, 2 VkPresentId2KHR */
    uint64_t (*assign)(void *user, int kind, uint64_t app_id);
    void *user;
} gfg_present_id_hooks;

gfg_gpu_fg *gfg_gpu_fg_create(const gfg_gpu_fg_create_info *info);
VkResult gfg_gpu_fg_present(gfg_gpu_fg *fg, VkQueue queue,
                            const VkPresentInfoKHR *original,
                            PFN_vkQueuePresentKHR next_present,
                            const gfg_present_id_hooks *ids);
void gfg_gpu_fg_destroy(gfg_gpu_fg *fg);
void gfg_gpu_fg_stats(gfg_gpu_stats *out);
/* Only called while no present job is executing. A NULL lock restores sync mode. */
void gfg_gpu_fg_set_output_thread(gfg_gpu_fg *fg, pthread_mutex_t *wsi);
/* Only the output worker enables this around its call; synchronous metadata
 * fallback must never sleep on the application's Present thread. */
void gfg_gpu_fg_set_output_pacing(gfg_gpu_fg *fg,int active);
void gfg_gpu_fg_set_output_pressure(gfg_gpu_fg *fg,int active);
/* Output worker only; timestamps were captured before mailbox backpressure. */
void gfg_gpu_fg_set_source(gfg_gpu_fg *fg,uint64_t captured_ns,uint64_t admitted_ns,uint64_t serial);
uint64_t gfg_gpu_fg_source_period_ns(gfg_gpu_fg *fg);
void gfg_gpu_fg_restart_history(gfg_gpu_fg *fg); /* output thread only */
int gfg_gpu_fg_waiting_image(gfg_gpu_fg *fg);
#endif
