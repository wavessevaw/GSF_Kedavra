#define _GNU_SOURCE
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFG Native GPU-resident transport for the TE8 generator (GFG_NATIVE_GPU_BRIDGE=1).
 *
 * Default: application-device TE context, same queue, GPU-only pixel flow.
 * All timeline signals are submitted before dependent WSI present calls.
 * Only same-device and explicitly requested shared-fd synchronization are
 * allowed. The host-signalled relay is removed because it reproduced VUID
 * 03268: a present semaphore depended on a timeline signal not yet submitted
 * to a queue. Unsupported shared-fd capability means safe pass-through.
 * No CPU pixel access and no CPU wait on the present path in steady state.
 * The explicit fd research mode below uses a separate device; the default
 * same-device mode uses ordinary image barriers and the caller-owned timeline.
 * TE8 in fd mode runs on its own VkDevice (same physical GPU, matched by deviceUUID); its
 * two source images and two destination images are exported as OPAQUE_FD
 * memory and imported into the APP's VkDevice with identical create info and
 * a dedicated allocation.
 *
 * Synchronization between the two devices is one timeline semaphore:
 *   real frame n gets the block V = 3n+1 .. V+2
 *   V   : app queue finished copying swapchain image n into TE source slot
 *   V+1 : TE finished synthetic output 0 (t = 1/3)
 *   V+2 : TE finished synthetic output 1 (t = 2/3) and all TE work of pair n
 * Shared mode: the timeline is created exportable on the app device and
 *   imported by TE (te_context_desc.sync_fd), so both GPUs wait/signal it.
 * Shared mode imports one timeline payload into TE; the WSI layer submits every
 * dependent signal before presenting. No host relay or host pixel wait exists.
 *
 * Present n (single swapchain, the app's present queue):
 *   1. submit: wait app semaphores (TRANSFER) + timeline >= last TE value,
 *      copy swapchain[idx] -> TE source slot (EXTERNAL ownership release),
 *      signal timeline V and ready[idx];
 *   2. te_submit_frame{slot, count 2, t {1/3, 2/3}, wait V, signal V+1, nonblocking};
 *      same-device initial/gap frames use count 0 and signal V+2 after history;
 *   3. for k = 1, 2: acquire (timeout 0), submit: wait acquire semaphore +
 *      timeline >= V+k, copy/blit dest[k-1] -> image, signal ready[img]; present;
 *   4. forward the app's present with pWaitSemaphores = { ready[idx] }.
 * Screen order: G(n-2/3), G(n-1/3), R(n).
 *
 * Binary semaphore rule: every binary semaphore we signal is waited exactly
 * once (ready[idx] by the forwarded real present, ready[img] by its synthetic
 * present, the acquire semaphore by the synthetic copy).  Per-frame command
 * buffers and acquire semaphores live in a ring whose fences are only polled
 * (vkGetFenceStatus); a busy slot passes the real present through unmodified.
 */
#include "gpu_bridge.h"
#include "frame_clock.h"
#include "present_chain.h"
#include "past_timing.h"
#include "refresh_observation.h"
#include "te_engine.h"
#include "timeline_watch.h"
#include "calibrated_clock.h"
#include "generation_budget.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#define RING_MAX 4
#define TIMING_CAP 8192
enum { DROP_RING, DROP_TE, DROP_HISTORY, DROP_ACQUIRE, DROP_BYPASS, DROP_ERROR, DROP_DEADLINE, DROP_CUT, DROP_RATE, DROP_DUPLICATE, DROP_COUNT };

#define FN(name) PFN_vk##name name
#define DEVICE_FUNCS(X) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) \
    X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(InvalidateMappedMemoryRanges) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBlitImage) X(CmdCopyImageToBuffer) \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(ResetFences) X(GetFenceStatus) X(WaitForFences) X(QueueSubmit) \
    X(GetSemaphoreCounterValue) X(SignalSemaphore) X(WaitSemaphores) \
    X(AcquireNextImageKHR) X(GetSwapchainImagesKHR) X(QueueWaitIdle)

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
} imported_image;

typedef struct {
    VkCommandBuffer real_cb;
    VkCommandBuffer gen_cb[2];
    VkSemaphore acquire_sem[2];
    VkFence fence;
    int busy;
} ring_slot;

struct gfg_gpu_fg {
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    uint32_t family;
    VkExtent2D extent;
    VkFormat format;
    VkPresentModeKHR present_mode;
    int force_fifo;
#define DECLARE(name) FN(name);
    DEVICE_FUNCS(DECLARE)
#undef DECLARE
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
    PFN_vkSetDeviceLoaderData set_loader_data;
    VkPhysicalDeviceMemoryProperties memory_props;

    VkImage *images;
    uint32_t image_count;
    VkSemaphore *ready;          /* one binary semaphore per swapchain image */

    imported_image src[2], dst[2];
    int src_bgra;
    int hdr10;
    VkColorSpaceKHR color_space;
    int linear_pq;
    int shared_flow;
    enum { OUT_COPY, OUT_BLIT, OUT_BLIT_COPY } out_mode;
    VkImage scratch;             /* OUT_BLIT_COPY: B8G8R8A8_UNORM swizzle target */
    VkDeviceMemory scratch_memory;

    VkSemaphore timeline;        /* app device: shared with TE */
    int shared_sync;
    int same_device; /* TE records on the application device and present queue */

    VkCommandPool pool;
    VkCommandBuffer init_cb;
    VkFence init_fence;
    ring_slot ring[RING_MAX];
    uint32_t ring_size;
    uint32_t ring_next;          /* advances on reservations, never on skipped app frames */
    uint64_t ring_reclaimed;
    uint64_t drop_outputs[DROP_COUNT]; /* per bridge, mutually exclusive output slots */
    uint64_t frame;              /* real presents seen by this bridge */

    te_device *engine;
    te_context *context;
    uint32_t te_slot;            /* next source slot TE expects */
    uint64_t te_frames;          /* frames TE accepted */
    uint64_t history_only_submits;
    int te_gap;                  /* a frame since the last accepted one was not fed to TE */
    uint64_t next_value;         /* first value of the next block */
    uint64_t last_te_value;      /* last value TE will signal */
    timeline_watch *timeline_observer;
    gsf_calibrated_clock calibrated;
    gsf_queue_probe_fn queue_probe;
    int lmc_tier;

    /* test-only verification (GFG_NATIVE_GPU_BRIDGE_VERIFY=1) */
    int verify;
    uint32_t verify_every;
    VkImage verify_hist[2];      /* copies of the real frames (TE sources lack TRANSFER_SRC) */
    VkDeviceMemory verify_hist_memory[2];
    VkBuffer verify_buffer;
    VkDeviceMemory verify_memory;
    VkDeviceSize verify_bytes;
    int verify_coherent;
    VkCommandBuffer verify_cb;
    VkFence verify_fence;
    int verify_pending;
    uint64_t verify_frame;

    uint32_t inject_every;       /* test-only fault injection (GFG_NATIVE_GPU_BRIDGE_INJECT_SKIP) */
    int disabled;
    int recreate_required;
    const char *fault; /* test-only, first generated present */
    int fault_used;
    int log;
    int in_present;
    pthread_mutex_t *output_wsi;
    atomic_int waiting_image;
    uint64_t output_waits;
    double output_deadline_us;
    unsigned bypass_logged;
    int mode_override_logged;
    int generation_logged;
    double *timing_total_us, *timing_own_us;
    /* per-phase CPU time sums (GFG_NATIVE_TIMING): record+submit real copy,
     * te_submit_frame, acquire, synthetic record+submit, fence submit */
    double phase_us[5];
    uint64_t phase_n;
    uint32_t timing_count;
    uint64_t gpu_last_sequence;
    uint32_t gpu_count, gpu_held_samples;
    double *gpu_first_ms, *gpu_pair_ms;
    double gpu_stage_ms[TE_STAGE_COUNT], gpu_lmc_ms[TE_LMC_PROF_COUNT];
    uint64_t gpu_lmc_count[TE_LMC_PROF_COUNT];
    gfg_frame_clock clock;
    int pacing_active;
    int pace_real; /* only after an actual synthetic handoff in this pair */
    atomic_int output_pressure;
    int diagnostic; /* 0 normal, 1 source-only, 2 compute-only, 3 matched-compute */
    uint64_t diagnostic_outputs;
    uint64_t gpu_budget_ns[2], last_generation_dispatch_ns;
    gsf_generation_budget generation_budget;
    int latency_admission;
    uint64_t pacing_waits, pacing_late;
    int deadline_drop, pressure_drop;
    /* Deliberately sparse, bounded real-WSI proof attempts for Decky 0.1.4. */
    int visibility_probe, probe_active;
    int cut_drop, duplicate_drop, control_checked;
    unsigned multiplier;
    uint64_t source_ns, admitted_ns, source_serial, previous_source_ns, previous_serial;
    uint64_t generation;
    uint64_t real_accepted, generated_accepted, status_last_ns, process_start_ticks;
    const char *status_dir;
    FILE *telemetry;
    uint64_t telemetry_flush_ns;
    int display_timing;
    gsf_refresh_observation refresh_observation;
    uint32_t google_id;
    PFN_vkGetRefreshCycleDurationGOOGLE refresh_cycle;
    PFN_vkGetPastPresentationTimingGOOGLE past_timing;
    uint64_t readiness_ns[2], accepted_ns[3], wsi_ns[3];
    int slot_drop[2];
    const char *slot_detail[2];
    uint64_t readiness_wait_ns[2];
    uint32_t google_ids[3];
    unsigned transport_submit_mask;

};

void gfg_gpu_fg_set_output_thread(gfg_gpu_fg *fg, pthread_mutex_t *wsi) { fg->output_wsi = wsi; }
void gfg_gpu_fg_set_output_pacing(gfg_gpu_fg *fg,int active) { fg->pacing_active=active; }
void gfg_gpu_fg_set_output_pressure(gfg_gpu_fg *fg,int active) { atomic_store(&fg->output_pressure,active); }
void gfg_gpu_fg_set_source(gfg_gpu_fg *fg,uint64_t captured,uint64_t admitted,uint64_t serial) {
    fg->source_ns=captured;fg->admitted_ns=admitted;(void)serial; /* IDs are assigned across synchronous fallbacks too. */
}
uint64_t gfg_gpu_fg_source_period_ns(gfg_gpu_fg *fg) {
    return fg->clock.interval_ns?fg->clock.interval_ns:fg->clock.period_ns?3*fg->clock.period_ns:33333333ull;
}
void gfg_gpu_fg_restart_history(gfg_gpu_fg *fg) { fg->te_gap = 1; }
int gfg_gpu_fg_waiting_image(gfg_gpu_fg *fg) { return atomic_load(&fg->waiting_image); }

static VkResult output_present(gfg_gpu_fg *fg, VkQueue queue, const VkPresentInfoKHR *pi,
                               PFN_vkQueuePresentKHR next,unsigned kind) {
    if(fg->pacing_active && fg->clock.period_ns && (kind!=2 || fg->pace_real) &&
       !fg->diagnostic && !fg->probe_active && !atomic_load(&fg->output_pressure)) {
        const uint64_t target=gfg_frame_clock_target(&fg->clock,kind)-(fg->display_timing==2?fg->clock.period_ns/4:0);
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        const uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
        if(ns<target) {
            int rc;
            do {
                /* Yield pacing to newly queued real frames within 0.5ms. */
                clock_gettime(CLOCK_MONOTONIC,&now);
                uint64_t current=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
                if(current>=target || atomic_load(&fg->output_pressure))break;
                uint64_t wake=fg->output_wsi && target-current>500000?current+500000:target;
                const struct timespec slice={(time_t)(wake/1000000000ull),(long)(wake%1000000000ull)};
                rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&slice,NULL);
            }while(rc==0 || rc==EINTR);
            ++fg->pacing_waits;
        } else if(ns-target>fg->clock.period_ns/4)++fg->pacing_late;
    }
    if (fg->output_wsi) pthread_mutex_lock(fg->output_wsi);
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
    extern void gfg_test_observer_kind(unsigned kind,uint64_t frame,unsigned multiplier);
    gfg_test_observer_kind(kind,fg->frame-1,fg->multiplier);
#endif
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    VkPresentInfoKHR timed=*pi;
    VkPresentTimeGOOGLE at={0};
    VkPresentTimesInfoGOOGLE times={.sType=VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
        .pNext=pi->pNext,.swapchainCount=1,.pTimes=&at};
    if(fg->display_timing==2 && fg->pacing_active && fg->clock.period_ns && pi->swapchainCount==1) {
        at.presentID=++fg->google_id;fg->google_ids[kind]=at.presentID;
        at.desiredPresentTime=fg->diagnostic || fg->probe_active || (kind==2 && !fg->pace_real) || atomic_load(&fg->output_pressure)?
            0:gfg_frame_clock_target(&fg->clock,kind);
        timed.pNext=&times;
    }
    VkResult r = next(queue, &timed);
    clock_gettime(CLOCK_MONOTONIC,&b);
    fg->wsi_ns[kind]=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000ull+(uint64_t)(b.tv_nsec-a.tv_nsec);
    if(r==VK_SUCCESS || r==VK_SUBOPTIMAL_KHR)fg->accepted_ns[kind]=(uint64_t)b.tv_sec*1000000000ull+(uint64_t)b.tv_nsec;
    if (fg->output_wsi) pthread_mutex_unlock(fg->output_wsi);
    return r;
}

static struct {
    atomic_uint_fast64_t v[sizeof(gfg_gpu_stats) / sizeof(uint64_t)];
} g_stats;
#define STAT_INDEX(field) (offsetof(gfg_gpu_stats, field) / sizeof(uint64_t))
#define STAT_ADD(field, n) atomic_fetch_add_explicit(&g_stats.v[STAT_INDEX(field)], (n), memory_order_relaxed)
#define STAT_SET(field, n) atomic_store_explicit(&g_stats.v[STAT_INDEX(field)], (n), memory_order_relaxed)

void gfg_gpu_fg_stats(gfg_gpu_stats *out) {
    if (!out) return;
    uint64_t *dst = (uint64_t *)out;
    for (size_t i = 0; i < sizeof(gfg_gpu_stats) / sizeof(uint64_t); ++i)
        dst[i] = atomic_load_explicit(&g_stats.v[i], memory_order_relaxed);
    out->version = 1;
}

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}
static void telemetry_flush(gfg_gpu_fg *fg,uint64_t now) {
    if(fg->telemetry && now-fg->telemetry_flush_ns>=250000000ull) {
        fflush(fg->telemetry);fg->telemetry_flush_ns=now;
    }
}
static int ok(VkResult r) { return r == VK_SUCCESS; }
static unsigned prepare_generation(const gfg_gpu_fg *fg,int have_pair,uint64_t v,
                                   uint64_t now,te_submit *s) {
    const int probe=fg->probe_active && gsf_generation_probe_allowed(
        &fg->generation_budget,fg->last_generation_dispatch_ns,now);
    unsigned mask=fg->clock.period_ns && fg->diagnostic!=2?
        gfg_frame_clock_compute_mask(&fg->clock,now,
            fg->display_timing==2?fg->clock.period_ns/4:0):(1u<<(fg->multiplier-1))-1u;
    /* Before calibrated latency is known, retain the bounded visibility
     * probe. Afterwards, only an unexpired slot can be used for recovery. */
    if(probe && have_pair && !mask && !fg->generation_budget.calibrated)mask=fg->clock.mask&1u;
    if(probe)mask&=1u;
    if(fg->diagnostic!=1 && fg->diagnostic!=2 && atomic_load(&fg->output_pressure))mask=0;
    if(fg->clock.period_ns && (fg->diagnostic==0 || fg->diagnostic==3) && fg->gpu_budget_ns[0]) {
        unsigned affordable=0,output=0;
        const uint64_t lead=fg->display_timing==2?fg->clock.period_ns/4:0;
        for(unsigned k=0;k<fg->multiplier-1;++k)if(mask&(1u<<k)) {
            const uint64_t cost=fg->gpu_budget_ns[output]?fg->gpu_budget_ns[output]:2*fg->gpu_budget_ns[0];
            if(gsf_generation_budget_fits(&fg->generation_budget,cost,fg->clock.target_ns[k],lead,now)) {
                affordable|=1u<<k;++output;
            }
        }
        /* A bounded retry learns recovery after a slow sample/scene. It is
         * one eligible output every two seconds, never an expired slot. */
        if(!affordable && mask && gsf_generation_retry_due(fg->last_generation_dispatch_ns,now))
            affordable=mask&(~mask+1u);
        /* Sparse visibility probes test whether *real generated* pixels can
         * reach the game's WSI, without treating a GPU-cost estimate as a
         * hard refusal. Readiness still has a small finite wait budget. */
        if(probe && mask)affordable=mask&(~mask+1u);
        mask=affordable;
    }
    const int generate=have_pair && mask;
    s->slot=fg->te_slot;
    /* Same-device history updates still signal v+2, with no LMC outputs.
     * Cross-device transport keeps its existing history/hold allocation. */
    s->count=generate?(uint32_t)__builtin_popcount(mask):!fg->same_device?fg->multiplier-1:0u;
    s->timestamps[0]=1.0f/(float)fg->multiplier;
    s->timestamps[1]=2.0f/3.0f;
    if(generate) {
        unsigned output=0;
        for(unsigned k=0;k<fg->multiplier-1;++k)if(mask&(1u<<k))
            s->timestamps[output++]=(float)(k+1)/(float)fg->multiplier;
    }
    s->wait_value=v;
    s->signal_first=s->count?v+3-s->count:v+2;
    s->nonblocking=1;
    s->discontinuity=fg->te_frames>0 && fg->te_gap;
    s->lmc_tier=fg->lmc_tier;
    s->defer_second=generate && s->count==2 && fg->same_device && fg->shared_flow;
    return mask;
}
static void fail_log(gfg_gpu_fg *fg, const char *what, VkResult r) {
    (void)fg;
    fprintf(stderr, "[gfg-native] gpu-bridge %s vkresult=%d\n", what, (int)r);
}

/* A skipped app frame does not own a slot. Indexing by app frame modulo the
 * ring can select a newer, pending fence while an older slot is already free.
 * Poll all candidates without resetting or waiting on a pending fence. */
static ring_slot *reserve_ring(gfg_gpu_fg *fg) {
    for (uint32_t k = 0; k < fg->ring_size; ++k) {
        const uint32_t i = (fg->ring_next + k) % fg->ring_size;
        ring_slot *rs = &fg->ring[i];
        if (rs->busy) {
            const VkResult r = fg->GetFenceStatus(fg->device, rs->fence);
            if (r == VK_NOT_READY) continue;
            if (r != VK_SUCCESS) {
                fail_log(fg, "ring fence", r);
                fg->disabled = 1;
                return NULL;
            }
            rs->busy = 0;
        }
        if (k) ++fg->ring_reclaimed;
        fg->ring_next = (i + 1) % fg->ring_size;
        return rs;
    }
    return NULL;
}

static void skip_outputs(gfg_gpu_fg *fg, unsigned reason, uint64_t count) {
    fg->drop_outputs[reason] += count;
    STAT_ADD(skipped_synthetic, count);
}

/* TE already writes GPU timestamp queries. Read only the latest completed
 * sample collected by its nonblocking fence poll; no GPU wait or pixel read.
 * This is a sample, not an exhaustive trace of every completed pair. */
static void gpu_sample(gfg_gpu_fg *fg) {
    if (!fg->context) return;
    te_pair_stats s;
    if (te_last_stats(fg->context, &s) != VK_SUCCESS ||
        s.sequence <= fg->gpu_last_sequence) return;
    fg->gpu_last_sequence = s.sequence;
    if (s.held) { ++fg->gpu_held_samples; return; }
    if(s.timed && s.outputs)for(unsigned k=0;k<s.outputs && k<2;++k)
        if(s.ready_ms[k]>0 && s.ready_ms[k]<10000.0)
            fg->gpu_budget_ns[k]=(uint64_t)(s.ready_ms[k]*1000000.0);
    uint64_t cal_seq=0,submit_return_ns=0,gpu_start_ticks=0;
    double gpu_tick_period_ns=0;
    uint32_t timestamp_valid_bits=0;
    if(te_last_calibration_fields(fg->context,&cal_seq,&submit_return_ns,
        &gpu_start_ticks,&gpu_tick_period_ns,&timestamp_valid_bits)!=VK_SUCCESS ||
       cal_seq!=s.sequence) {
        submit_return_ns=0;gpu_start_ticks=0;gpu_tick_period_ns=0;
        timestamp_valid_bits=0;
    }
    gsf_clock_estimate estimate={0};
    if(s.timed && timestamp_valid_bits==64)
        estimate=gsf_calibration_estimate(&fg->calibrated,gpu_start_ticks,
            gpu_tick_period_ns,submit_return_ns);
    else {
        estimate=(gsf_clock_estimate){.start_text="null",.delay_text="null",
            .calibration_text="null",.deviation_text="null",.delta_text="null",
            .reason=s.timed?"timestamp_bits_below_64":"gpu_timestamps_unavailable"};
    }
    if(fg->latency_admission)
        gsf_generation_budget_observe(&fg->generation_budget,estimate.valid,estimate.delay_ns);
    if(fg->telemetry) {
        fprintf(fg->telemetry,"{\"schema\":1,\"event\":\"gpu_correlated\","
            "\"pid\":%ld,\"generation\":%llu,\"te_sequence\":%llu,\"submit_return_ns\":%llu,\"gpu_start_ticks\":%llu,"
            "\"timestamp_valid_bits\":%u,\"gpu_timestamp_period_ns\":%.9f,"
            "\"gpu_start_cpu_ns\":%s,\"submit_to_gpu_start_ns\":%s,\"gpu_start_minus_submit_return_ns\":%s,"
            "\"calibration_ns\":%s,\"calibration_max_deviation_ns\":%s,"
            "\"calibration_status\":\"%s\"}\n",
            (long)getpid(),(unsigned long long)fg->generation,
            (unsigned long long)s.sequence,(unsigned long long)submit_return_ns,
            (unsigned long long)gpu_start_ticks,timestamp_valid_bits,
            gpu_tick_period_ns,estimate.start_text,estimate.delay_text,estimate.delta_text,
            estimate.calibration_text,estimate.deviation_text,estimate.reason);
    }
    if(fg->telemetry) {
        fprintf(fg->telemetry,"{\"schema\":1,\"event\":\"gpu_pair\",\"pid\":%ld,\"generation\":%llu,\"te_sequence\":%llu,"
            "\"outputs\":%u,\"timed\":%d,\"ready_ms\":[%.6f,%.6f],\"analysis_ms\":[%.6f,%.6f,%.6f],"
            "\"lmc_pass_ms\":[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f],\"cut\":%u,\"duplicate\":%d}\n",
            (long)getpid(),(unsigned long long)fg->generation,(unsigned long long)s.sequence,s.outputs,s.timed,
            s.ready_ms[0],s.ready_ms[1],s.stage_ms[0],s.stage_ms[1],s.stage_ms[2],
            s.lmc_ms[0],s.lmc_ms[1],s.lmc_ms[2],s.lmc_ms[3],s.lmc_ms[4],s.lmc_ms[5],s.lmc_ms[6],s.lmc_ms[7],s.scene_cut,s.source_duplicate);
    }
    if (!s.timed || s.outputs < 1 || !fg->gpu_pair_ms || !fg->gpu_first_ms || fg->gpu_count == TIMING_CAP) return;
    const uint32_t i = fg->gpu_count++;
    fg->gpu_first_ms[i] = s.ready_ms[0];
    fg->gpu_pair_ms[i] = s.ready_ms[s.outputs-1];
    for (unsigned k = 0; k < TE_STAGE_COUNT; ++k) fg->gpu_stage_ms[k] += s.stage_ms[k];
    for (unsigned k = 0; k < TE_LMC_PROF_COUNT; ++k)
        if (s.lmc_ms[k] >= 0.0) {
            fg->gpu_lmc_ms[k] += s.lmc_ms[k];
            ++fg->gpu_lmc_count[k];
        }
    if (fg->log && (i < 3 || i % 120 == 0))
        fprintf(stderr, "[gfg-native] gpu_pair sequence=%llu output1_ready_ms=%.3f"
                " output2_ready_ms=%.3f model_gpu=%d model_ms=%.3f tier=%d"
                " shared_flow=%d projective_fallback=%d cut=%u cut_bad_blocks=%u\n",
                (unsigned long long)s.sequence, s.ready_ms[0], s.ready_ms[1],
                s.model_gpu, s.lmc_ms[TE_LMC_FIT_WAIT], s.lmc_tier,
                s.shared_flow,s.shared_flow_fallback,s.scene_cut,s.shared_flow_cut_bad_blocks);
}

/* Every blocking wait goes through these so the counters prove the contract. */
static void count_wait(gfg_gpu_fg *fg) {
    if (fg->in_present && fg->output_wsi) ++fg->output_waits;
    else if (fg->in_present) STAT_ADD(present_path_cpu_waits, 1);
    else STAT_ADD(teardown_cpu_waits, 1);
}
static VkResult wait_fence(gfg_gpu_fg *fg, VkFence fence, uint64_t timeout) {
    count_wait(fg);
    return fg->WaitForFences(fg->device, 1, &fence, VK_TRUE, timeout);
}

static int is_bgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}
static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
}
static int is_hdr10(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}
static int supported_format(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB ||
           f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || is_hdr10(f);
}

static int32_t find_memory(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                           VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return (int32_t)i;
    return -1;
}

/* Must mirror te_engine.c image_create() for imports: same format, usage,
 * tiling, extent, external handle type, dedicated allocation and the same
 * memory-type choice (first DEVICE_LOCAL type in the requirement bits). */
static int import_image(gfg_gpu_fg *fg, imported_image *im, VkFormat format,
                        VkImageUsageFlags usage, int fd) {
    if (fd < 0) return 0;
    VkExternalMemoryImageCreateInfo emi = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {fg->extent.width, fg->extent.height, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = fg->CreateImage(fg->device, &ici, NULL, &im->image);
    if (!ok(r)) { close(fd); fail_log(fg, "import vkCreateImage", r); return 0; }
    VkMemoryRequirements req;
    fg->GetImageMemoryRequirements(fg->device, im->image, &req);
    int32_t type = find_memory(&fg->memory_props, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(&fg->memory_props, req.memoryTypeBits, 0);
    if (type < 0) { close(fd); return 0; }
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = im->image};
    VkImportMemoryFdInfoKHR imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    r = fg->AllocateMemory(fg->device, &mai, NULL, &im->memory);
    if (!ok(r)) { close(fd); fail_log(fg, "import vkAllocateMemory", r); return 0; }
    /* fd ownership passed to the implementation */
    r = fg->BindImageMemory(fg->device, im->image, im->memory, 0);
    if (!ok(r)) { fail_log(fg, "import vkBindImageMemory", r); return 0; }
    return 1;
}

static int local_image(gfg_gpu_fg *fg, VkImage *image, VkDeviceMemory *memory,
                       VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {fg->extent.width, fg->extent.height, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!ok(fg->CreateImage(fg->device, &ici, NULL, image))) return 0;
    VkMemoryRequirements req;
    fg->GetImageMemoryRequirements(fg->device, *image, &req);
    int32_t type = find_memory(&fg->memory_props, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(&fg->memory_props, req.memoryTypeBits, 0);
    if (type < 0) return 0;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    if (!ok(fg->AllocateMemory(fg->device, &mai, NULL, memory))) return 0;
    return ok(fg->BindImageMemory(fg->device, *image, *memory, 0));
}

/* ------------------------------------------------------------ barriers */

static const VkImageSubresourceRange COLOR = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

static VkImageMemoryBarrier image_barrier(VkImage image, VkAccessFlags sa, VkAccessFlags da,
        VkImageLayout from, VkImageLayout to, uint32_t src_family, uint32_t dst_family) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = src_family, .dstQueueFamilyIndex = dst_family,
        .image = image, .subresourceRange = COLOR};
    return b;
}
static void cmd_barrier(gfg_gpu_fg *fg, VkCommandBuffer cb, VkPipelineStageFlags src,
                        VkPipelineStageFlags dst, uint32_t n, const VkImageMemoryBarrier *b) {
    if(fg->same_device){
        if(src == VK_PIPELINE_STAGE_TRANSFER_BIT) src=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if(dst == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) dst=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    fg->CmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, n, b);
}
/* TE's contract (te_engine.c ownership()): images are in GENERAL; TE acquires
 * sources and destinations from VK_QUEUE_FAMILY_EXTERNAL with GENERAL->GENERAL
 * and releases destinations to EXTERNAL the same way.  We mirror that and use
 * GENERAL for our own copies so release/acquire layouts always match. */
static VkImageMemoryBarrier acquire_external(gfg_gpu_fg *fg, VkImage image, VkAccessFlags da) {
    if(fg->same_device)
        return image_barrier(image,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|
            VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,da,
            VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    return image_barrier(image, 0, da, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_QUEUE_FAMILY_EXTERNAL, fg->family);
}
static VkImageMemoryBarrier release_external(gfg_gpu_fg *fg, VkImage image, VkAccessFlags sa) {
    if(fg->same_device)
        return image_barrier(image,sa|VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    return image_barrier(image, sa, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         fg->family, VK_QUEUE_FAMILY_EXTERNAL);
}

/* Command buffers we allocate below the loader trampoline need the loader's
 * dispatch pointer, or layers between us and the ICD (validation) crash. */
static int alloc_cb(gfg_gpu_fg *fg, VkCommandBuffer *cb) {
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = fg->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (!ok(fg->AllocateCommandBuffers(fg->device, &cai, cb))) return 0;
    if (fg->set_loader_data && !ok(fg->set_loader_data(fg->device, *cb))) return 0;
    return 1;
}

static int begin(gfg_gpu_fg *fg, VkCommandBuffer cb) {
    if (!ok(fg->ResetCommandBuffer(cb, 0))) return 0;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    return ok(fg->BeginCommandBuffer(cb, &bi));
}

/* swapchain image (PRESENT_SRC, rendered by the app) -> TE source slot */
static int record_real(gfg_gpu_fg *fg, VkCommandBuffer cb, VkImage swap, VkImage slot,
                       VkImage hist) {
    if (!begin(fg, cb)) return 0;
    VkImageMemoryBarrier pre[2] = {
        /* Match the submit wait scope; same-device barriers include TE shader work. */
        image_barrier(swap, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                      VK_QUEUE_FAMILY_IGNORED),
        acquire_external(fg, slot, VK_ACCESS_TRANSFER_WRITE_BIT)};
    /* src TRANSFER also orders this write after earlier transfer reads of the
     * slot on this queue (verification copies) */
    cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2, pre);
    VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {fg->extent.width, fg->extent.height, 1}};
    fg->CmdCopyImage(cb, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    if (hist) { /* test-only verify mode: keep R(n) for the distinctness readback */
        VkImageMemoryBarrier h = image_barrier(hist, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &h);
        if (fg->hdr10) {
            VkImageBlit blit = {.srcSubresource=region.srcSubresource,.dstSubresource=region.dstSubresource,
                .srcOffsets={{0,0,0},{(int32_t)fg->extent.width,(int32_t)fg->extent.height,1}},
                .dstOffsets={{0,0,0},{(int32_t)fg->extent.width,(int32_t)fg->extent.height,1}}};
            fg->CmdBlitImage(cb,swap,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hist,
                            VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
        } else fg->CmdCopyImage(cb, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hist,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    VkImageMemoryBarrier post[2] = {
        image_barrier(swap, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED,
                      VK_QUEUE_FAMILY_IGNORED),
        release_external(fg, slot, VK_ACCESS_TRANSFER_WRITE_BIT)};
    cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2, post);
    return ok(fg->EndCommandBuffer(cb));
}

/* TE destination (R8G8B8A8_UNORM, GENERAL, EXTERNAL) -> acquired swapchain image */
static int record_generated(gfg_gpu_fg *fg, VkCommandBuffer cb, VkImage out, VkImage swap) {
    if (!begin(fg, cb)) return 0;
    VkImageMemoryBarrier pre[3] = {
        acquire_external(fg, out, VK_ACCESS_TRANSFER_READ_BIT),
        image_barrier(swap, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                      VK_QUEUE_FAMILY_IGNORED),
        /* scratch: previous use was a transfer on this queue */
        image_barrier(fg->scratch, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED)};
    cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                fg->out_mode == OUT_BLIT_COPY ? 3u : 2u, pre);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {fg->extent.width, fg->extent.height, 1}};
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)fg->extent.width, (int32_t)fg->extent.height, 1}},
        .dstOffsets = {{0, 0, 0}, {(int32_t)fg->extent.width, (int32_t)fg->extent.height, 1}}};
    switch (fg->out_mode) {
    case OUT_COPY:   /* RGBA swapchain: raw bits, SRGB bytes pass through like the source copy */
        fg->CmdCopyImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, swap,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        break;
    case OUT_BLIT:   /* RGBA_UNORM -> BGRA_UNORM: channel swizzle, no transfer function */
        fg->CmdBlitImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, swap,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        break;
    case OUT_BLIT_COPY: { /* BGRA_SRGB: swizzle into UNORM scratch, then raw copy (no re-encode) */
        fg->CmdBlitImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, fg->scratch,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
        VkImageMemoryBarrier mid = image_barrier(fg->scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &mid);
        fg->CmdCopyImage(cb, fg->scratch, VK_IMAGE_LAYOUT_GENERAL, swap,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        break; }
    }
    VkImageMemoryBarrier post[2] = {
        release_external(fg, out, 0),
        image_barrier(swap, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED)};
    cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2, post);
    return ok(fg->EndCommandBuffer(cb));
}

/* Recovery after acquire: copy the current real image, never present undefined
 * contents as a generated frame. If recording itself is unavailable, retire
 * the acquired image with the swapchain after forwarding the game's frame. */
static int record_duplicate(gfg_gpu_fg *fg,VkCommandBuffer cb,VkImage real,VkImage target){
    if(!begin(fg,cb)) return 0;
    VkImageMemoryBarrier b[2]={
        image_barrier(real,0,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED),
        image_barrier(target,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED)};
    cmd_barrier(fg,cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,2,b);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={fg->extent.width,fg->extent.height,1}};
    fg->CmdCopyImage(cb,real,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b[0]=image_barrier(real,VK_ACCESS_TRANSFER_READ_BIT,0,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    b[1]=image_barrier(target,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    cmd_barrier(fg,cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,2,b);
    return ok(fg->EndCommandBuffer(cb));
}
static int inject_fault(gfg_gpu_fg *fg,const char *kind){
    if(!fg->fault_used && fg->fault && !strcmp(fg->fault,kind)){
        fg->fault_used=1;return 1;
    }
    return 0;
}
/* Used only when it is known no submit has consumed this acquire semaphore. */
static void retire_acquired(gfg_gpu_fg *fg,VkSemaphore acquired){
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo drain={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,
        .pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage};
    (void)fg->QueueSubmit(fg->queue,1,&drain,VK_NULL_HANDLE);
    fg->disabled=1;fg->recreate_required=1;
}

/* ------------------------------------------------------------ verification */

static int verify_setup(gfg_gpu_fg *fg) {
    const VkFormat hist_format = fg->hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : fg->src_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < 2; ++i)
        if (!local_image(fg, &fg->verify_hist[i], &fg->verify_hist_memory[i], hist_format,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return 0;
    const VkDeviceSize one = (VkDeviceSize)fg->extent.width * fg->extent.height * (fg->hdr10 ? 8u : 4u);
    fg->verify_bytes = one * 4u;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = fg->verify_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!ok(fg->CreateBuffer(fg->device, &bi, NULL, &fg->verify_buffer))) return 0;
    VkMemoryRequirements req;
    fg->GetBufferMemoryRequirements(fg->device, fg->verify_buffer, &req);
    int32_t type = find_memory(&fg->memory_props, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) type = find_memory(&fg->memory_props, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type < 0) return 0;
    fg->verify_coherent = !!(fg->memory_props.memoryTypes[type].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    if (!ok(fg->AllocateMemory(fg->device, &mai, NULL, &fg->verify_memory))) return 0;
    if (!ok(fg->BindBufferMemory(fg->device, fg->verify_buffer, fg->verify_memory, 0))) return 0;
    if (!alloc_cb(fg, &fg->verify_cb)) return 0;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return ok(fg->CreateFence(fg->device, &fci, NULL, &fg->verify_fence));
}

/* Copy generated outputs and both real neighbours; 2x has no G2. */
static void verify_submit(gfg_gpu_fg *fg, uint64_t v, uint32_t cur_slot) {
    if (fg->verify_pending) return;
    VkCommandBuffer cb = fg->verify_cb;
    if (!begin(fg, cb)) return;
    VkImage imgs[4] = {fg->dst[0].image, fg->dst[1].image,
                       fg->verify_hist[cur_slot ^ 1u], fg->verify_hist[cur_slot]};
    VkImageMemoryBarrier acq[4], rel[2];
    const unsigned outputs = fg->multiplier - 1;
    unsigned barriers = 0;
    for (unsigned i = 0; i < outputs; ++i) {
        acq[barriers++] = acquire_external(fg, imgs[i], VK_ACCESS_TRANSFER_READ_BIT);
        rel[i] = release_external(fg, imgs[i], 0);
    }
    for (int i = 2; i < 4; ++i)
        acq[barriers++] = image_barrier(imgs[i], VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    /* the global barrier orders this write of the readback buffer after the
     * previous verification's write (WAW; the host already consumed it) */
    VkMemoryBarrier waw = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    fg->CmdPipelineBarrier(cb, fg->same_device?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &waw, 0, NULL, barriers, acq);
    const VkDeviceSize one = (VkDeviceSize)fg->extent.width * fg->extent.height * (fg->hdr10 ? 8u : 4u);
    for (int i = 0; i < 4; ++i) {
        if (i == 1 && outputs == 1) continue;
        VkBufferImageCopy region = {.bufferOffset = one * (VkDeviceSize)i,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {fg->extent.width, fg->extent.height, 1}};
        fg->CmdCopyImageToBuffer(cb, imgs[i], VK_IMAGE_LAYOUT_GENERAL, fg->verify_buffer, 1, &region);
    }
    cmd_barrier(fg, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, outputs, rel);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    fg->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                           1, &host, 0, NULL, 0, NULL);
    if (!ok(fg->EndCommandBuffer(cb))) return;
    const uint64_t wait_value = v + outputs;
    VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = fg->same_device ? 0u : 1u, .pWaitSemaphoreValues = &wait_value};
    const VkPipelineStageFlags stage = fg->same_device ?
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
        .waitSemaphoreCount = fg->same_device ? 0u : 1u, .pWaitSemaphores = &fg->timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (!ok(fg->ResetFences(fg->device, 1, &fg->verify_fence))) return;
    if (!ok(fg->QueueSubmit(fg->queue, 1, &si, fg->verify_fence))) return;
    fg->verify_pending = 1;
    fg->verify_frame = fg->frame;
}

static unsigned half_unorm(const uint8_t *p) {
    uint16_t h; memcpy(&h,p,sizeof(h));
    unsigned e=(h>>10)&31u, m=h&1023u;
    if(h&0x8000u) return 0;
    if(e>=15u) return 65535u;
    /* Exact fixed-point conversion for nonnegative normalized half values. */
    return e ? (((m+1024u)*65535u) >> (25u-e)) : (m*65535u >> 24u);
}
static uint64_t rgb_delta(const uint8_t *a, int a_bgra, const uint8_t *b, int b_bgra, size_t px, int fp16) {
    uint64_t sum = 0;
    for (size_t i = 0; i < px; ++i) {
        for (int c = 0; c < 3; ++c) {
            int ca = fp16 ? (int)half_unorm(a+8*i+2*c) : a[4 * i + (a_bgra ? 2 - c : c)];
            int cb = fp16 ? (int)half_unorm(b+8*i+2*c) : b[4 * i + (b_bgra ? 2 - c : c)];
            sum += (uint64_t)(ca > cb ? ca - cb : cb - ca);
        }
    }
    return sum;
}

/* Non-blocking unless final (teardown). Host reads here are test-only. */
static void verify_collect(gfg_gpu_fg *fg, int final) {
    if (!fg->verify_pending) return;
    VkResult r = final ? wait_fence(fg, fg->verify_fence, 2000000000ull)
                       : fg->GetFenceStatus(fg->device, fg->verify_fence);
    if (r != VK_SUCCESS) return;
    fg->verify_pending = 0;
    void *map = NULL;
    if (!ok(fg->MapMemory(fg->device, fg->verify_memory, 0, VK_WHOLE_SIZE, 0, &map))) return;
    if (!fg->verify_coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = fg->verify_memory, .size = VK_WHOLE_SIZE};
        fg->InvalidateMappedMemoryRanges(fg->device, 1, &range);
    }
    STAT_ADD(verify_host_reads, 1);
    const size_t px = (size_t)fg->extent.width * fg->extent.height;
    const size_t one=px*(fg->hdr10 ? 8u : 4u);
    const uint8_t *g1 = map, *g2 = g1 + one, *rp = g2 + one, *rn = rp + one;
    const int sb = fg->src_bgra;
    const int gb = fg->same_device && fg->src_bgra;
    const int two_outputs = fg->multiplier == 3;
    uint64_t d12 = two_outputs ? rgb_delta(g1, gb, g2, gb, px, fg->hdr10) : 0;
    uint64_t d1p = rgb_delta(g1, gb, rp, sb, px, fg->hdr10), d1n = rgb_delta(g1, gb, rn, sb, px, fg->hdr10);
    uint64_t d2p = two_outputs ? rgb_delta(g2, gb, rp, sb, px, fg->hdr10) : 0;
    uint64_t d2n = two_outputs ? rgb_delta(g2, gb, rn, sb, px, fg->hdr10) : 0;
    uint64_t dpn = rgb_delta(rp, sb, rn, sb, px, fg->hdr10);
    if(fg->hdr10) { d12/=257;d1p/=257;d1n/=257;d2p/=257;d2n/=257;dpn/=257; }
    int distinct = d1p && d1n && (two_outputs ? (d12 && d2p && d2n) : dpn);
    /* interpolation sanity: G1 nearer R(n-1), G2 nearer R(n) */
    int ordered = two_outputs ? (d1p < d1n && d2n < d2p) : -1; /* midpoint: N/A */
    fg->UnmapMemory(fg->device, fg->verify_memory);
    STAT_ADD(verify_checks, 1);
    if (distinct) STAT_ADD(verify_distinct, 1);
    fprintf(stderr,
        "[gfg-native] gpu-verify frame=%llu distinct=%d ordered=%d mad_g1g2=%.3f"
        " mad_g1_prev=%.3f mad_g1_real=%.3f mad_g2_prev=%.3f mad_g2_real=%.3f"
        " mad_prev_real=%.3f\n",
        (unsigned long long)fg->verify_frame, distinct, ordered,
        (double)d12 / (double)(px * 3), (double)d1p / (double)(px * 3),
        (double)d1n / (double)(px * 3), (double)d2p / (double)(px * 3),
        (double)d2n / (double)(px * 3), (double)dpn / (double)(px * 3));
}

/* ------------------------------------------------------------ lifecycle */

static int env_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

/* A successful FD import only proves handle creation. Before enabling WSI,
 * prove that host signals on the app payload are visible to TE and that a
 * queue signal on TE's imported payload is visible back on the app device.
 * This is an opt-in startup check; it never runs on the frame hot path. */
static int shared_timeline_selftest(gfg_gpu_fg *fg) {
    if (fg->same_device) return 1;
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = fg->timeline, .value = 1};
    if (!ok(fg->SignalSemaphore(fg->device, &signal))) return 0;
    if (te_timeline_wait(fg->context, 1, 1000000000ull) != VK_SUCCESS) return 0;
    if (te_timeline_fd_probe(fg->context, 1, 2) != VK_SUCCESS) return 0;
    const uint64_t target = 2;
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &fg->timeline, .pValues = &target};
    if (!ok(fg->WaitSemaphores(fg->device, &wait, 1000000000ull))) return 0;
    uint64_t value = 0;
    if (!ok(fg->GetSemaphoreCounterValue(fg->device, fg->timeline, &value)) || value < 2)
        return 0;
    fprintf(stderr, "[gfg-native] shared timeline selftest=PASS app_to_te=1 te_queue_to_app=2\n");
    return 1;
}
static unsigned env_uint(const char *name, unsigned def, unsigned lo, unsigned hi) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (!end || *end || x < lo || x > hi) return def;
    return (unsigned)x;
}

gfg_gpu_fg *gfg_gpu_fg_create(const gfg_gpu_fg_create_info *info) {
    if (!info || !info->device || !info->queue || !info->gdpa || !info->gipa ||
        !info->physical || !supported_format(info->format) ||
        (info->color_space!=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
         info->color_space!=VK_COLOR_SPACE_HDR10_ST2084_EXT) ||
        (info->color_space==VK_COLOR_SPACE_HDR10_ST2084_EXT && !is_hdr10(info->format)) ||
        info->present_mode != VK_PRESENT_MODE_FIFO_KHR ||
        info->extent.width < 16 || info->extent.height < 16 ||
        info->extent.width > 8192 || info->extent.height > 8192)
        return NULL;
    gfg_gpu_fg *fg = calloc(1, sizeof(*fg));
    if (!fg) return NULL;
    const double t0 = now_us();
    fg->pace_real=0;
    if(fg->context){te_poll(fg->context);gpu_sample(fg);}
    fg->device = info->device;
    fg->queue_probe = info->queue_probe;
    fg->queue = info->queue;
    fg->swapchain = info->swapchain;
    fg->family = info->family;
    fg->extent = info->extent;
    fg->format = info->format;
    fg->present_mode = info->present_mode;
    fg->force_fifo = info->force_fifo;
    fg->set_loader_data = info->set_loader_data;
    fg->log = getenv("GFG_NATIVE_TRACE") != NULL;
    fg->visibility_probe=env_flag("GFG_NATIVE_VISIBILITY_PROBE");
    fg->src_bgra = is_bgra(info->format);
    fg->hdr10 = is_hdr10(info->format);
    fg->color_space=info->color_space;
    fg->linear_pq=info->color_space==VK_COLOR_SPACE_HDR10_ST2084_EXT;
    const char *why = "entrypoints";
    const char *sync_mode = getenv("GFG_NATIVE_GPU_SYNC");
    if (sync_mode && strcmp(sync_mode, "same-device") && strcmp(sync_mode, "shared-fd")) {
        why = "unsupported-sync-mode";
        goto fail;
    }
    fg->same_device = !sync_mode || !strcmp(sync_mode, "same-device");
#define LOAD(name) fg->name = (PFN_vk##name)info->gdpa(info->device, "vk" #name); \
    if (!fg->name) goto fail;
    DEVICE_FUNCS(LOAD)
#undef LOAD
    fg->GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)info->gdpa(info->device, "vkGetSemaphoreFdKHR");
    PFN_vkGetPhysicalDeviceMemoryProperties mp = (PFN_vkGetPhysicalDeviceMemoryProperties)
        info->gipa(info->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)
        info->gipa(info->instance, "vkGetPhysicalDeviceProperties2");
    if (!props2) props2 = (PFN_vkGetPhysicalDeviceProperties2)
        info->gipa(info->instance, "vkGetPhysicalDeviceProperties2KHR");
    PFN_vkGetPhysicalDeviceFormatProperties fp = (PFN_vkGetPhysicalDeviceFormatProperties)
        info->gipa(info->instance, "vkGetPhysicalDeviceFormatProperties");
    if (!mp || !props2 || !fp) goto fail;
    mp(info->physical, &fg->memory_props);

    why = "output-format";
    if (fg->hdr10) {
        if (!fg->same_device) goto fail;
        VkFormatProperties output, swap;
        fp(info->physical, VK_FORMAT_R16G16B16A16_SFLOAT, &output);
        fp(info->physical, info->format, &swap);
        if (!(output.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                !(swap.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) goto fail;
        fg->out_mode = OUT_BLIT;
    } else if (!fg->src_bgra || fg->same_device) {
        fg->out_mode = OUT_COPY;
    } else {
        VkFormatProperties rgba, bgra;
        fp(info->physical, VK_FORMAT_R8G8B8A8_UNORM, &rgba);
        fp(info->physical, VK_FORMAT_B8G8R8A8_UNORM, &bgra);
        if (!(rgba.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(bgra.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            goto fail;
        fg->out_mode = is_srgb(info->format) ? OUT_BLIT_COPY : OUT_BLIT;
    }

    why = "swapchain-images";
    uint32_t n = 0;
    if (!ok(fg->GetSwapchainImagesKHR(fg->device, fg->swapchain, &n, NULL)) || n < 2 || n > 64)
        goto fail;
    fg->images = calloc(n, sizeof(*fg->images));
    fg->ready = calloc(n, sizeof(*fg->ready));
    if (!fg->images || !fg->ready ||
        !ok(fg->GetSwapchainImagesKHR(fg->device, fg->swapchain, &n, fg->images)))
        goto fail;
    fg->image_count = n;
    VkSemaphoreCreateInfo bsci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i)
        if (!ok(fg->CreateSemaphore(fg->device, &bsci, NULL, &fg->ready[i]))) goto fail;

    /* TE on the caller's device by default, or a same-GPU TE device in
     * explicit shared-fd mode. */
    why = "te-device";
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &id};
    props2(info->physical, &p2);
    te_device_desc dd = {.match_uuid = 1, .validation = env_flag("GFG_NATIVE_TE_VALIDATION")};
    memcpy(dd.device_uuid, id.deviceUUID, VK_UUID_SIZE);
    if(fg->same_device){
        if(te_device_borrow(info->instance,info->gipa,info->physical,info->device,
            info->gdpa,info->set_loader_data,info->queue,info->family,&fg->engine)!=VK_SUCCESS){
            fprintf(stderr,"[gfg-native] same-device TE unavailable: %s\n",te_last_error());
            goto fail;
        }
    } else     if (te_device_create(&dd, &fg->engine) != VK_SUCCESS) {
        fprintf(stderr, "[gfg-native] gpu-bridge TE device: %s\n", te_last_error());
        goto fail;
    }
    int te_mem = 0, te_sem = 0;
    te_device_supports_fd_import(fg->engine, &te_mem, &te_sem);
    why = "te-memory-fd";
    if (!fg->same_device && !te_mem) goto fail;
    fg->shared_sync = fg->same_device ||
        (info->semaphore_fd && te_sem && fg->GetSemaphoreFdKHR);
    if (!fg->shared_sync) {
        why = "shared-timeline-fd-unavailable";
        goto fail;
    }

    /* timeline on the app device: exportable in shared mode */
    why = "timeline";
    VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkSemaphoreTypeCreateInfo stci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = fg->shared_sync && !fg->same_device ? &esci : NULL,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo tsci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci};
    if (!ok(fg->CreateSemaphore(fg->device, &tsci, NULL, &fg->timeline))) goto fail;
    int sync_fd = -1;
    if (fg->shared_sync && !fg->same_device) {
        VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = fg->timeline, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        if (!ok(fg->GetSemaphoreFdKHR(fg->device, &gi, &sync_fd)) || sync_fd < 0) goto fail;
    }

    why = "te-context";
    te_context_desc cd;
    te_context_desc_default(&cd, fg->extent.width, fg->extent.height, 2);
    fg->display_timing=info->display_timing;
    if(fg->display_timing) {
        fg->refresh_cycle=(PFN_vkGetRefreshCycleDurationGOOGLE)info->gdpa(info->device,"vkGetRefreshCycleDurationGOOGLE");
        fg->past_timing=(PFN_vkGetPastPresentationTimingGOOGLE)info->gdpa(info->device,"vkGetPastPresentationTimingGOOGLE");
    }
    fprintf(stderr,"[gfg-native] display_timing enabled=%d namespace=%s refresh_query_available=%d physical_scanout=unverified\n",
        fg->display_timing,fg->display_timing==2?"layer":fg->display_timing==1?"application":"unavailable",
        fg->refresh_cycle!=NULL);
    const char *algorithm = getenv("GFG_NATIVE_ALGO");
    cd.params.algorithm=algorithm && !strcmp(algorithm,"lmc-gpu")?TE_ALGO_LMC_GPU:
        algorithm && !strcmp(algorithm,"lmc")?TE_ALGO_LMC:
        algorithm && !strcmp(algorithm,"gsf")?TE_ALGO_SHARED_FLOW:TE_ALGO_TILES;
    fg->shared_flow=cd.params.algorithm==TE_ALGO_SHARED_FLOW;
    fg->multiplier=3;
    const char *multiplier=getenv("GFG_NATIVE_MODE");
    if(multiplier && !strcmp(multiplier,"2"))fg->multiplier=2;
    static atomic_uint_fast64_t generations;
    fg->generation=atomic_fetch_add(&generations,1)+1;
    fg->status_dir=getenv("GFG_NATIVE_STATUS_DIR");
    FILE *proc=fopen("/proc/self/stat","r");
    if(proc) {
        char buf[4096];
        if(fgets(buf,sizeof(buf),proc)) {
            char *end=strrchr(buf,')'),*save=NULL,*token=end?strtok_r(end+2," ",&save):NULL;
            for(int field=3;token && field<22;++field)token=strtok_r(NULL," ",&save);
            if(token)fg->process_start_ticks=strtoull(token,NULL,10);
        }
        fclose(proc);
    }
    const char *telemetry=getenv("GFG_NATIVE_TELEMETRY");
    if(telemetry && *telemetry) {
        int fd=open(telemetry,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0) {
            fg->telemetry=fdopen(fd,"a");
            if(!fg->telemetry)close(fd);
            else {
                (void)setvbuf(fg->telemetry,NULL,_IOFBF,65536);
                fg->telemetry_flush_ns=(uint64_t)(now_us()*1000.0);
            }
        }
        if(!fg->telemetry)fprintf(stderr,"[gfg-native] telemetry unavailable errno=%d\n",errno);
    }
    cd.shared_flow_dense=cd.params.algorithm==TE_ALGO_SHARED_FLOW?4:0;
    if(cd.params.algorithm==TE_ALGO_SHARED_FLOW)fg->clock.period_ns=1000000000ull/90;
    /* In-game LMC follows measured source intervals and the compositor's
     * refresh query. This fallback seeds the clock; unlike OUTPUT_HZ it does
     * not suppress WSI refresh updates or impose a source FPS target. */
    if(cd.params.algorithm==TE_ALGO_LMC_GPU && env_flag("GFG_NATIVE_PACING"))
        fg->clock.period_ns=1000000000ull/90;
    const char *rate=getenv("GFG_NATIVE_OUTPUT_HZ");
    int refresh_override_applied=0;
    /* Explicit pacing also applies to GPU LMC; its default stays unpaced.
     * Correctness probes and visible cadence demos are separate contracts. */
    if(rate && (cd.params.algorithm==TE_ALGO_SHARED_FLOW || cd.params.algorithm==TE_ALGO_LMC_GPU)) {
        char *end=NULL;long hz=strtol(rate,&end,10);
        if(end!=rate && !*end && hz>=1 && hz<=240) {
            fg->clock.period_ns=1000000000ull/(uint64_t)hz;refresh_override_applied=1;
        }
    }
    gsf_refresh_init(&fg->refresh_observation,fg->clock.period_ns,refresh_override_applied);
    if(fg->telemetry)fprintf(fg->telemetry,
        "{\"schema\":1,\"event\":\"bridge_context\",\"pid\":%ld,\"generation\":%llu,"
        "\"frontend_hint\":\"%s\",\"engine_version\":%u,\"instance_api_version\":%u,"
        "\"present_queue_family\":%u,\"te_uses_present_queue\":%s,"
        "\"source_timestamp_semantics\":\"cpu_vulkan_present_entry\",\"source_clock\":\"CLOCK_MONOTONIC\","
        "\"source_gpu_ready_ns\":null,\"physical_display_hz\":null}\n",
        (long)getpid(),(unsigned long long)fg->generation,info->frontend_hint?info->frontend_hint:"unspecified",
        info->engine_version,info->instance_api_version,info->family,fg->same_device?"true":"false");
    const char *diagnostic=getenv("GFG_NATIVE_DIAGNOSTIC_MODE");
    fg->diagnostic=diagnostic && !strcmp(diagnostic,"source-only")?1:
        diagnostic && !strcmp(diagnostic,"compute-only")?2:
        diagnostic && !strcmp(diagnostic,"matched-compute")?3:0;
    if(fg->diagnostic)fprintf(stderr,"[gfg-native] diagnostic_mode=%s game_performance_claim=0\n",diagnostic);
    cd.export_fds = !fg->same_device;
    cd.application_timeline = fg->same_device ? fg->timeline : VK_NULL_HANDLE;
    cd.host_io = 0;
    cd.source_bgra = fg->src_bgra;
    cd.source_format = fg->hdr10 ? info->format : VK_FORMAT_UNDEFINED;
    cd.output_fp16 = fg->hdr10;
    cd.color_transfer=info->color_space==VK_COLOR_SPACE_HDR10_ST2084_EXT?TE_TRANSFER_PQ:TE_TRANSFER_CODED;
    cd.output_bgra = fg->same_device && fg->src_bgra;
    cd.sync_fd = sync_fd; /* ownership passes to TE (closed on failure too) */
    if (te_context_create(fg->engine, &cd, &fg->context) != VK_SUCCESS) {
        fprintf(stderr, "[gfg-native] gpu-bridge TE context: %s\n", te_last_error());
        goto fail;
    }
    why = "shared-timeline-selftest";
    if (!shared_timeline_selftest(fg)) goto fail;
    const char *tier = getenv("GFG_NATIVE_LMC_TIER");
    if (tier && tier[0] >= '0' && tier[0] <= '2' && tier[1] == '\0') fg->lmc_tier = tier[0] - '0';

    why = "import";
    const VkFormat src_format = fg->src_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if(fg->same_device){
        for(uint32_t s=0;s<2;++s) fg->src[s].image=te_source_image(fg->context,s);
        for(uint32_t o=0;o<2;++o) fg->dst[o].image=te_output_image(fg->context,o);
    } else {
    for (uint32_t s = 0; s < 2; ++s)
        if (!import_image(fg, &fg->src[s], src_format,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                te_export_source_fd(fg->context, s)))
            goto fail;
    /* TE-created (exported) destinations add TRANSFER_SRC; must match exactly */
    for (uint32_t o = 0; o < 2; ++o)
        if (!import_image(fg, &fg->dst[o], VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                te_export_dest_fd(fg->context, o)))
            goto fail;
    }
    if (fg->out_mode == OUT_BLIT_COPY &&
        !local_image(fg, &fg->scratch, &fg->scratch_memory, VK_FORMAT_B8G8R8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        goto fail;

    why = "commands";
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = fg->family};
    if (!ok(fg->CreateCommandPool(fg->device, &pci, NULL, &fg->pool))) goto fail;
    fg->ring_size = env_uint("GFG_NATIVE_GPU_INFLIGHT", 2, 1, RING_MAX);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t r = 0; r < fg->ring_size; ++r) {
        ring_slot *rs = &fg->ring[r];
        if (!alloc_cb(fg, &rs->real_cb)) goto fail;
        for (int k = 0; k < 2; ++k) {
            if (!alloc_cb(fg, &rs->gen_cb[k])) goto fail;
            if (!ok(fg->CreateSemaphore(fg->device, &bsci, NULL, &rs->acquire_sem[k]))) goto fail;
        }
        if (!ok(fg->CreateFence(fg->device, &fci, NULL, &rs->fence))) goto fail;
        if(env_flag("GFG_TEST_CB_HANDLES"))
            fprintf(stderr,"GFG test transport ring=%u real=%p gen0=%p gen1=%p\n",
                    r,(void *)rs->real_cb,(void *)rs->gen_cb[0],(void *)rs->gen_cb[1]);
    }
    if (!alloc_cb(fg, &fg->init_cb)) goto fail;
    if (!ok(fg->CreateFence(fg->device, &fci, NULL, &fg->init_fence))) goto fail;

    fg->verify = env_flag("GFG_NATIVE_GPU_BRIDGE_VERIFY");
    fg->verify_every = env_uint("GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY", 8, 1, 100000);
    fg->fault=getenv("GFG_NATIVE_GPU_FAULT");
    fg->inject_every = env_uint("GFG_NATIVE_GPU_BRIDGE_INJECT_SKIP", 0, 2, 100000);
    why = "verify";
    if (fg->verify && !verify_setup(fg)) goto fail;

    /* One-time: give our side of the shared images a defined GENERAL layout
     * and hand them to EXTERNAL (TE acquires GENERAL->GENERAL). No CPU wait:
     * queue order puts it before the first real copy. */
    if (!begin(fg, fg->init_cb)) goto fail;
    {
        VkImage shared[4] = {fg->src[0].image, fg->src[1].image, fg->dst[0].image, fg->dst[1].image};
        VkImageMemoryBarrier tr[7], rel[4];
        uint32_t nt = 0;
        for (int i = 0; !fg->same_device && i < 4; ++i) {
            tr[nt++] = image_barrier(shared[i], 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
            rel[i] = release_external(fg, shared[i], 0);
        }
        if (fg->scratch)
            tr[nt++] = image_barrier(fg->scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        for (int i = 0; i < 2; ++i)
            if (fg->verify_hist[i])
                tr[nt++] = image_barrier(fg->verify_hist[i], 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        cmd_barrier(fg, fg->init_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, nt, tr);
        cmd_barrier(fg, fg->init_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg->same_device?0u:4u, rel);
    }
    if (!ok(fg->EndCommandBuffer(fg->init_cb))) goto fail;
    VkSubmitInfo isi = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
        .pCommandBuffers = &fg->init_cb};
    if (!ok(fg->QueueSubmit(fg->queue, 1, &isi, fg->init_fence))) goto fail;


    fg->next_value = fg->same_device ? 1 : 3;
    fg->latency_admission=env_uint("GFG_NATIVE_LATENCY_ADMISSION",1,0,1);
    fg->timing_total_us = calloc(TIMING_CAP, sizeof(double));
    fg->timing_own_us = calloc(TIMING_CAP, sizeof(double));
    if (fg->log || fg->telemetry || getenv("GFG_NATIVE_TIMING")) {
        fg->gpu_first_ms = calloc(TIMING_CAP, sizeof(double));
        fg->gpu_pair_ms = calloc(TIMING_CAP, sizeof(double));
    }
    gsf_calibration_init(&fg->calibrated,info->instance,info->physical,info->device,
        info->gipa,info->gdpa,info->calibrated_mode,
        env_uint("GFG_NATIVE_CALIBRATION_EVERY_FRAMES",10,1,1000),fg->telemetry);
    if(env_flag("GFG_NATIVE_TIMELINE_WATCH") && fg->same_device &&
       cd.params.algorithm==TE_ALGO_LMC_GPU) {
        fg->timeline_observer=tw_start(fg->device,fg->timeline,fg->WaitSemaphores,
            fg->GetSemaphoreCounterValue,fg->telemetry,(uint64_t)getpid(),fg->generation);
        fprintf(stderr,"[gfg-native] timeline_watch enabled=%d poll_ms=2 observation_max_ms=200\n",
            fg->timeline_observer!=NULL);
    }
    STAT_ADD(bridges_created, 1);
    STAT_SET(sync_mode, fg->same_device ? 3 : 1);
    fprintf(stderr,
        "[gfg-native] GPU bridge active: %ux%u format=%d out=%s sync=%s images=%u"
        " present_mode=%d inflight=%u algo=%s verify=%d startup_ms=%.2f color_space=%d transfer=%s linear_cache_bytes=%llu\n",
        fg->extent.width, fg->extent.height, (int)fg->format,
        fg->out_mode == OUT_COPY ? "copy" : fg->out_mode == OUT_BLIT ? "blit" : "blit+copy",
        fg->same_device ? "same-device" : "shared-timeline-fd", fg->image_count,
        (int)fg->present_mode, fg->ring_size,
        cd.params.algorithm == TE_ALGO_SHARED_FLOW ? "gsf" : cd.params.algorithm == TE_ALGO_LMC_GPU ? "lmc-gpu" : cd.params.algorithm == TE_ALGO_LMC ? "lmc" : "tiles", fg->verify,
        (now_us() - t0) / 1000.0,(int)fg->color_space,fg->linear_pq?"pq-linear-nits":"coded",
        (unsigned long long)(fg->linear_pq?(uint64_t)fg->extent.width*fg->extent.height*16:0));
    return fg;
fail:
    fprintf(stderr, "[gfg-native] GPU bridge unavailable stage=%s\n", why);
    gfg_gpu_fg_destroy(fg);
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static void percentile(double *v, uint32_t n, double *median, double *p95) {
    qsort(v, n, sizeof(double), cmp_double);
    if(!n){*median=*p95=0;return;}
    double mid=(double)(n-1)*.5,hi=(double)(n-1)*.95;
    uint32_t a=(uint32_t)mid,b=(uint32_t)hi;
    *median=v[a]+(v[a+(a+1<n)]-v[a])*(mid-a);
    *p95=v[b]+(v[b+(b+1<n)]-v[b])*(hi-b);
}

void gfg_gpu_fg_destroy(gfg_gpu_fg *fg) {
    if (!fg) return;
    tw_stop(fg->timeline_observer);fg->timeline_observer=NULL;
    if(fg->diagnostic)fprintf(stderr,"[gfg-native] diagnostic_retired mode=%d dispatched_outputs=%llu\n",
        fg->diagnostic,(unsigned long long)fg->diagnostic_outputs);
    if(fg->clock.period_ns)fprintf(stderr,"[gfg-native] output_clock hz=%.3f handoff_waits=%llu late_handoffs=%llu resyncs=%llu skipped_deadline=%llu skipped_cut=%llu physical_scanout=unverified\n",
        1e9/(double)fg->clock.period_ns,(unsigned long long)fg->pacing_waits,(unsigned long long)fg->pacing_late,
        (unsigned long long)fg->clock.resyncs,(unsigned long long)fg->drop_outputs[DROP_DEADLINE],
        (unsigned long long)fg->drop_outputs[DROP_CUT]);
    if(fg->status_dir) {
        char path[4096];
        int n=snprintf(path,sizeof(path),"%s/%ld-%llu.json",fg->status_dir,(long)getpid(),(unsigned long long)fg->generation);
        if(n>0 && n<(int)sizeof(path))unlink(path);
    }
    fg->in_present = 0;
    /* The stopped worker may have finished with a pass-through job. Its
     * private gate and downstream binary waits then have no transport-ring
     * fence. Drain queue operations once at retirement before destroying any
     * semaphores. The supplied async dispatch serializes this host access. */
    if(fg->output_wsi && fg->QueueWaitIdle) {
        count_wait(fg);(void)fg->QueueWaitIdle(fg->queue);
    }
    if (fg->verify) verify_collect(fg, 1);
    if (fg->device && fg->WaitForFences) {
        if (fg->init_fence && fg->init_cb) (void)wait_fence(fg, fg->init_fence, 2000000000ull);
        for (uint32_t r = 0; r < fg->ring_size; ++r)
            if (fg->ring[r].busy) (void)wait_fence(fg, fg->ring[r].fence, 2000000000ull);
    }
    if (fg->context) { te_poll(fg->context); gpu_sample(fg); }
    if(fg->telemetry){fclose(fg->telemetry);fg->telemetry=NULL;}
    gfg_gpu_stats st;
    gfg_gpu_fg_stats(&st);
    if (fg->log || getenv("GFG_NATIVE_TIMING")) {
        fprintf(stderr,
            "[gfg-native] GPU bridge retired real=%llu synthetic=%llu skipped=%llu te_busy=%llu"
            " ring_busy=%llu acquire_not_ready=%llu holds=%llu errors=%llu"
            " present_path_host_pixel_reads=%llu present_path_cpu_waits=%llu"
            " verify_checks=%llu verify_distinct=%llu pid=%ld bridge_frames=%llu\n",
            (unsigned long long)st.real_presents, (unsigned long long)st.synthetic_presents,
            (unsigned long long)st.skipped_synthetic, (unsigned long long)st.te_busy,
            (unsigned long long)st.ring_busy, (unsigned long long)st.acquire_not_ready,
            (unsigned long long)st.holds, (unsigned long long)st.errors,
            (unsigned long long)st.present_path_host_pixel_reads,
            (unsigned long long)st.present_path_cpu_waits,
            (unsigned long long)st.verify_checks, (unsigned long long)st.verify_distinct,
            (long)getpid(), (unsigned long long)fg->frame);
        fprintf(stderr, "[gfg-native] gpu-bridge skipped_outputs ring=%llu te=%llu history=%llu"
                " acquire=%llu bypass=%llu error=%llu ring_reclaimed=%llu pid=%ld bridge_frames=%llu"
                " history_only_submits=%llu output_thread_waits=%llu\n",
                (unsigned long long)fg->drop_outputs[DROP_RING],
                (unsigned long long)fg->drop_outputs[DROP_TE],
                (unsigned long long)fg->drop_outputs[DROP_HISTORY],
                (unsigned long long)fg->drop_outputs[DROP_ACQUIRE],
                (unsigned long long)fg->drop_outputs[DROP_BYPASS],
                (unsigned long long)fg->drop_outputs[DROP_ERROR],
                (unsigned long long)fg->ring_reclaimed, (long)getpid(),
                (unsigned long long)fg->frame, (unsigned long long)fg->history_only_submits,
                (unsigned long long)fg->output_waits);
        if (fg->gpu_count) {
            double fm, fp, pm, pp;
            percentile(fg->gpu_first_ms, fg->gpu_count, &fm, &fp);
            percentile(fg->gpu_pair_ms, fg->gpu_count, &pm, &pp);
            fprintf(stderr, "[gfg-native] gpu-bridge gpu_elapsed samples=%u held_samples=%u"
                    " output1_median_ms=%.3f output1_p95_ms=%.3f"
                    " output2_median_ms=%.3f output2_p95_ms=%.3f"
                    " scope=analysis-start-to-output-ready pid=%ld\n",
                    fg->gpu_count, fg->gpu_held_samples, fm, fp, pm, pp, (long)getpid());
            fprintf(stderr, "[gfg-native] gpu-bridge gpu_analysis_mean_ms pyramid=%.3f"
                    " analyze=%.3f classify=%.3f\n",
                    fg->gpu_stage_ms[0] / fg->gpu_count, fg->gpu_stage_ms[1] / fg->gpu_count,
                    fg->gpu_stage_ms[2] / fg->gpu_count);
            static const char *const names[TE_LMC_PROF_COUNT] = {
                "search_b", "search_a", "layers", "model", "field", "masked", "orphan", "field2"};
            for (unsigned k = 0; k < TE_LMC_PROF_COUNT; ++k)
                if (fg->gpu_lmc_count[k])
                    fprintf(stderr, "[gfg-native] gpu-bridge gpu_lmc_mean_ms stage=%s ms=%.3f samples=%llu\n",
                            names[k], fg->gpu_lmc_ms[k] / fg->gpu_lmc_count[k],
                            (unsigned long long)fg->gpu_lmc_count[k]);
        } else fprintf(stderr, "[gfg-native] gpu-bridge gpu_elapsed samples=0 unavailable=1\n");
        if (fg->timing_total_us && fg->timing_count > 2) {
            /* first two frames carry one-time work (TE warm-up); excluded */
            uint32_t n = fg->timing_count - 2;
            double tm, tp, om, op;
            percentile(fg->timing_total_us + 2, n, &tm, &tp);
            percentile(fg->timing_own_us + 2, n, &om, &op);
            fprintf(stderr,
                "[gfg-native] gpu-bridge present_cpu frames=%u %ux%u total_median_us=%.1f"
                " total_p95_us=%.1f layer_own_median_us=%.1f layer_own_p95_us=%.1f\n",
                n, fg->extent.width, fg->extent.height, tm, tp, om, op);
            if (fg->phase_n) {
                const double d = (double)fg->phase_n;
                fprintf(stderr,
                    "[gfg-native] gpu-bridge phase_mean_us real_submit=%.1f te_submit=%.1f"
                    " acquire=%.1f synthetic_submit=%.1f fence_submit=%.1f\n",
                    fg->phase_us[0] / d, fg->phase_us[1] / d, fg->phase_us[2] / d,
                    fg->phase_us[3] / d, fg->phase_us[4] / d);
            }
        }
    }
    if (fg->context) te_context_destroy(fg->context);
    if (fg->context && fg->engine && getenv("GFG_NATIVE_TE_VALIDATION"))
        fprintf(stderr, "[gfg-native] gpu-bridge TE validation_errors=%u\n",
                te_device_validation_errors(fg->engine));
    if (fg->engine) te_device_destroy(fg->engine);
    if (fg->device && fg->DestroySemaphore) {
        if (fg->ready)
            for (uint32_t i = 0; i < fg->image_count; ++i)
                if (fg->ready[i]) fg->DestroySemaphore(fg->device, fg->ready[i], NULL);
        for (uint32_t r = 0; r < RING_MAX; ++r)
            for (int k = 0; k < 2; ++k)
                if (fg->ring[r].acquire_sem[k])
                    fg->DestroySemaphore(fg->device, fg->ring[r].acquire_sem[k], NULL);
        if (fg->timeline) fg->DestroySemaphore(fg->device, fg->timeline, NULL);
    }
    if (fg->device && fg->DestroyFence) {
        for (uint32_t r = 0; r < RING_MAX; ++r)
            if (fg->ring[r].fence) fg->DestroyFence(fg->device, fg->ring[r].fence, NULL);
        if (fg->init_fence) fg->DestroyFence(fg->device, fg->init_fence, NULL);
        if (fg->verify_fence) fg->DestroyFence(fg->device, fg->verify_fence, NULL);
    }
    if (fg->device && fg->DestroyCommandPool && fg->pool)
        fg->DestroyCommandPool(fg->device, fg->pool, NULL);
    if (fg->device && fg->DestroyImage && fg->FreeMemory) {
        for (int i = 0; !fg->same_device && i < 2; ++i) {
            if (fg->src[i].image) fg->DestroyImage(fg->device, fg->src[i].image, NULL);
            if (fg->src[i].memory) fg->FreeMemory(fg->device, fg->src[i].memory, NULL);
            if (fg->dst[i].image) fg->DestroyImage(fg->device, fg->dst[i].image, NULL);
            if (fg->dst[i].memory) fg->FreeMemory(fg->device, fg->dst[i].memory, NULL);
        }
        if (fg->scratch) fg->DestroyImage(fg->device, fg->scratch, NULL);
        for (int i = 0; i < 2; ++i) {
            if (fg->verify_hist[i]) fg->DestroyImage(fg->device, fg->verify_hist[i], NULL);
            if (fg->verify_hist_memory[i]) fg->FreeMemory(fg->device, fg->verify_hist_memory[i], NULL);
        }
        if (fg->scratch_memory) fg->FreeMemory(fg->device, fg->scratch_memory, NULL);
    }
    if (fg->device && fg->DestroyBuffer && fg->verify_buffer)
        fg->DestroyBuffer(fg->device, fg->verify_buffer, NULL);
    if (fg->device && fg->FreeMemory && fg->verify_memory)
        fg->FreeMemory(fg->device, fg->verify_memory, NULL);
    free(fg->images);
    free(fg->ready);
    free(fg->timing_total_us);
    free(fg->timing_own_us);
    free(fg->gpu_first_ms);
    free(fg->gpu_pair_ms);
    free(fg);
}

/* ------------------------------------------------------------ present */

static VkResult submit_real(gfg_gpu_fg *fg, ring_slot *rs, const VkPresentInfoKHR *app,
                            uint32_t idx, uint64_t v) {
    if(env_flag("GFG_TEST_CB_HANDLES")) fprintf(stderr,"GFG transport real frame=%llu idx=%u image=%p cb=%p wait=%p\n",(unsigned long long)fg->frame,idx,(void*)fg->images[idx],(void*)rs->real_cb,app->waitSemaphoreCount?(void*)app->pWaitSemaphores[0]:NULL);
    VkSemaphore waits[17];
    VkPipelineStageFlags stages[17];
    uint64_t wait_values[17];
    uint32_t nw = 0;
    if (app->waitSemaphoreCount > 16) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < app->waitSemaphoreCount; ++i) {
        waits[nw] = app->pWaitSemaphores[i];
        stages[nw] = fg->same_device ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
        wait_values[nw++] = 0; /* binary: ignored */
    }
    if (fg->last_te_value && !fg->same_device) {
        /* Cross-device TE needs the timeline wait. Same-queue TE reads are
         * ordered by the ALL_COMMANDS source-image barrier. */
        waits[nw] = fg->timeline;
        stages[nw] = fg->same_device ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
        wait_values[nw++] = fg->last_te_value;
    }
    VkSemaphore signals[2] = {fg->timeline, fg->ready[idx]};
    uint64_t signal_values[2] = {v, 0};
    VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = nw, .pWaitSemaphoreValues = wait_values,
        .signalSemaphoreValueCount = 2, .pSignalSemaphoreValues = signal_values};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
        .waitSemaphoreCount = nw, .pWaitSemaphores = waits, .pWaitDstStageMask = stages,
        .commandBufferCount = 1, .pCommandBuffers = &rs->real_cb,
        .signalSemaphoreCount = 2, .pSignalSemaphores = signals};
    return fg->QueueSubmit(fg->queue, 1, &si, VK_NULL_HANDLE);
}

/* Keep caller-owned real-present metadata out of synthetic calls. Known
 * chains are copied synchronously; unknown metadata preserves the entire app
 * present and is diagnosed once rather than silently reporting FG active. */
static void bypass_log(gfg_gpu_fg *fg, unsigned bit, const char *reason,
                       const VkPresentInfoKHR *app, int id_mode) {
    if (fg->bypass_logged & bit) return;
    fg->bypass_logged |= bit;
    char types[256] = {0};
    size_t used = 0;
    unsigned walked = 0;
    for (const VkBaseInStructure *n = app->pNext; n && walked < GFG_PRESENT_CHAIN_MAX;
         n = n->pNext, ++walked) {
        const int wrote = snprintf(types + used, sizeof(types) - used,
                                   "%s%d", walked ? "," : "", (int)n->sType);
        if (wrote < 0 || (size_t)wrote >= sizeof(types) - used) break;
        used += (size_t)wrote;
    }
    fprintf(stderr, "[gfg-native] fg_bypass reason=%s pid=%ld frame=%llu"
            " swapchains=%u wait_semaphores=%u id_mode=%d pnext_types=[%s]\n",
            reason, (long)getpid(), (unsigned long long)fg->frame,
            app->swapchainCount, app->waitSemaphoreCount, id_mode, types);
}

static VkResult forward_real(gfg_gpu_fg *fg, VkQueue queue, const VkPresentInfoKHR *app, const VkSemaphore *wait,
                             PFN_vkQueuePresentKHR next, const gfg_present_id_hooks *ids,
                             double *next_us) {
    VkPresentInfoKHR real = *app;
    if (wait) {
        real.waitSemaphoreCount = 1;
        real.pWaitSemaphores = wait;
    }
    gfg_present_chain chain;
    const int mode = ids && ids->assign ? ids->mode : 0;
    if (!gfg_pc_prepare(&chain, app, mode)) {
        if (mode) gfg_pc_set_id(&chain, mode, ids->assign(ids->user, 0, chain.app_id));
        real.pNext = gfg_pc_head(&chain);
    } else if (fg->force_fifo && !gfg_pc_fifo_fallback(&chain,app,&real)) {
        /* A dynamic mode incompatible with the replacement swapchain cannot
         * be forwarded safely through an uncopyable prefix. Retire explicitly. */
        fg->disabled=fg->recreate_required=1;
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const double t = now_us();
    VkResult r = output_present(fg, queue, &real, next,2);
    if (next_us) *next_us += now_us() - t;
    return r;
}

/* acquire + copy + present one synthetic output; 1 presented, 0 skipped, -1 error */
static int present_synthetic(gfg_gpu_fg *fg, ring_slot *rs, int k, int output_index, uint64_t v, uint32_t real_index,
                             PFN_vkQueuePresentKHR next, const gfg_present_id_hooks *ids,
                             int dynamic_fifo, double *next_us) {
    uint32_t img = UINT32_MAX;
    double tp = now_us();
    VkResult r;
    double target_us=fg->output_deadline_us;
    const int probe=fg->probe_active && k==0;
    if(atomic_load(&fg->output_pressure)) {
        fg->slot_detail[k]="source-backlog";fg->pressure_drop=1;return 0;
    }
    if(fg->pacing_active && fg->clock.period_ns) {
        const uint64_t target=gfg_frame_clock_target(&fg->clock,(unsigned)k)-(fg->display_timing==2?fg->clock.period_ns/4:0);
        uint64_t now=(uint64_t)(now_us()*1000.0),value=v+1+(uint64_t)output_index;
        /* The probe gets at most 12ms for GPU readiness, independent of a
         * past output deadline. Normal scheduling retains strict deadlines. */
        const uint64_t ready_until=probe?now+12000000ull:target;
        tw_deadline(fg->timeline_observer,v,(unsigned)output_index,ready_until);
        if(fg->cut_drop || fg->duplicate_drop)return 0;
        if(!probe && !(gfg_frame_clock_compute_mask(&fg->clock,now,fg->display_timing==2?fg->clock.period_ns/4:0)&(1u<<k))) {
            fg->slot_detail[k]="expired-before-readiness";fg->deadline_drop=1;return 0;
        }
        VkSemaphoreWaitInfo wait={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount=1,.pSemaphores=&fg->timeline,.pValues=&value};
        count_wait(fg);
        do {
            if(atomic_load(&fg->output_pressure)) {
                fg->slot_detail[k]="source-backlog";fg->pressure_drop=1;return 0;
            }
            uint64_t current=(uint64_t)(now_us()*1000.0);
            uint64_t remaining=ready_until>current?ready_until-current:0;
            r=fg->WaitSemaphores(fg->device,&wait,remaining>500000?500000:remaining);
        }while(r==VK_TIMEOUT && (uint64_t)(now_us()*1000.0)<ready_until);
        const uint64_t observed=(uint64_t)(now_us()*1000.0);
        fg->readiness_wait_ns[k]=observed-now;
        if(r==VK_SUCCESS)fg->readiness_ns[k]=observed;
        if(r==VK_TIMEOUT || (!probe && r==VK_SUCCESS && (uint64_t)(now_us()*1000.0)>target)) {
            fg->slot_detail[k]="gpu-readiness-deadline";fg->deadline_drop=1;return 0;
        }
        if(r!=VK_SUCCESS){fail_log(fg,"output readiness",r);return -1;}
        fg->readiness_ns[k]=(uint64_t)(now_us()*1000.0);
        /* Only SharedFlow exports its cut/duplicate control header.
         * LMC pacing uses readiness and deadlines, as its unpaced path does. */
        if(fg->shared_flow && !fg->control_checked) {
            uint32_t cut=0;int held=0,duplicate=0;
            r=te_output_control(fg->context,(uint32_t)output_index,value,&cut,&held,&duplicate);
            if(r!=VK_SUCCESS){fail_log(fg,"output control",r);return -1;}
            fg->control_checked=1;
            if(cut || held){fg->cut_drop=1;return 0;}
            if(duplicate){fg->duplicate_drop=1;return 0;}
        }
        target_us=probe?now_us()+2000.0:(double)target/1000.0;
    }
    /* WSI acquisition is polled with the host lock released between attempts.
     * Never block an app acquire while holding that same swapchain lock. Give
     * waiting output priority over a new app acquire; share one real-period budget. */
    const double acquire_end = fg->output_wsi ? target_us : now_us();
    /* In the direct real-WSI path, a zero-timeout acquire can miss a free
     * image between vblanks even if generation has finished. Wait a bounded
     * slice of this frame's source budget, never UINT64_MAX as that could
     * stall a misbehaving Gamescope indefinitely. The async worker retains
     * its separate nonblocking deadline/poll logic. */
    const uint64_t source_period = gfg_gpu_fg_source_period_ns(fg);
    const uint64_t acquire_budget_ns = fg->output_wsi ? 0 :
        (source_period / fg->multiplier < 8000000ull
            ? source_period / fg->multiplier : 8000000ull);
    if (fg->output_wsi) atomic_store(&fg->waiting_image, 1);
    for (;;) {
        if (fg->output_wsi) pthread_mutex_lock(fg->output_wsi);
        r = fg->AcquireNextImageKHR(fg->device, fg->swapchain,
                                    acquire_budget_ns, rs->acquire_sem[k],
                                   VK_NULL_HANDLE, &img);
        if (fg->output_wsi) pthread_mutex_unlock(fg->output_wsi);
        if ((r != VK_NOT_READY && r != VK_TIMEOUT) || !fg->output_wsi || now_us() >= acquire_end) break;
        ++fg->output_waits;
        struct timespec delay = {.tv_nsec = 250000}; nanosleep(&delay, NULL);
    }
    if (fg->output_wsi) atomic_store(&fg->waiting_image, 0);
    fg->phase_us[2] += now_us() - tp;
    tp = now_us();
    if (r == VK_NOT_READY || r == VK_TIMEOUT) {
        fg->slot_detail[k]="WSI-acquire-unavailable";
        STAT_ADD(acquire_not_ready, 1);
        return 0;
    }
    if ((r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) || img >= fg->image_count) {
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            /* impossible index: acquire semaphore is pending, never reuse it */
            retire_acquired(fg,rs->acquire_sem[k]);
        }
        return -1;
    }
    /* From here the acquire semaphore is signalled: it MUST be waited. */
    if(inject_fault(fg,"index") || inject_fault(fg,"submit")){
        retire_acquired(fg,rs->acquire_sem[k]);return -1;
    }
    const int generated=!inject_fault(fg,"record") &&
        record_generated(fg,rs->gen_cb[k],fg->dst[output_index].image,fg->images[img]);
    if(!generated){
        fg->disabled=1;
        if(!record_duplicate(fg,rs->gen_cb[k],fg->images[real_index],fg->images[img])){
            retire_acquired(fg,rs->acquire_sem[k]);return -1;
        }
    }
    const uint64_t value = v + 1 + (uint64_t)output_index;
    VkSemaphore waits[2] = {rs->acquire_sem[k], fg->timeline};
    /* same-device barriers include ALL_COMMANDS to cover previous TE shader
     * reads. Acquired-image layout transitions must wait at that same scope,
     * not a later TRANSFER-only point (WRITE_AFTER_PRESENT under load). */
    const VkPipelineStageFlags wait_stage = fg->same_device ?
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPipelineStageFlags stages[2] = {wait_stage, wait_stage};
    uint64_t wait_values[2] = {0, value};
    VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 2, .pWaitSemaphoreValues = wait_values};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
        /* Same-device TE work precedes this on the same queue; its shader
         * writes are made visible by record_generated's image barrier. Only
         * the acquired swapchain payload needs a semaphore wait here. */
        .waitSemaphoreCount = generated && !fg->same_device ? 2u : 1u,
        .pWaitSemaphores = waits, .pWaitDstStageMask = stages,
        .commandBufferCount = 1, .pCommandBuffers = &rs->gen_cb[k],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &fg->ready[img]};
    tsi.waitSemaphoreValueCount=si.waitSemaphoreCount;
    /* Recovery copies the real image and never claims an intermediate. */
    VkSemaphore signals[2]={fg->ready[img],fg->ready[real_index]};
    if(!generated){
        /* The recovery also transitions the retained real image. Consume its
         * capture-ready signal and replace it after recovery, so the real
         * presentation cannot race that later layout transition. */
        waits[1]=fg->ready[real_index];wait_values[1]=0;
        stages[0]=stages[1]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        si.waitSemaphoreCount=tsi.waitSemaphoreValueCount=2;
        si.signalSemaphoreCount=2;si.pSignalSemaphores=signals;
    }
    r = fg->QueueSubmit(fg->queue, 1, &si, VK_NULL_HANDLE);
    fg->phase_us[3] += now_us() - tp;
    if (!ok(r)) { fail_log(fg, "synthetic submit", r); fg->disabled = 1; fg->recreate_required=1; return -1; }
    fg->transport_submit_mask|=1u<<(unsigned)k;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &fg->ready[img],
        .swapchainCount = 1, .pSwapchains = &fg->swapchain, .pImageIndices = &img};
    uint64_t layer_id = 0;
    union {
        VkPresentIdKHR v1;
        VkPresentId2KHR v2;
    } pid = {0};
    if (ids && ids->mode && ids->assign && (layer_id = ids->assign(ids->user, generated?k+1:3, 0)) != 0) {
        if (ids->mode == 2) {
            pid.v2.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
            pid.v2.swapchainCount = 1;
            pid.v2.pPresentIds = &layer_id;
            pi.pNext = &pid.v2;
        } else {
            pid.v1.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
            pid.v1.swapchainCount = 1;
            pid.v1.pPresentIds = &layer_id;
            pi.pNext = &pid.v1;
        }
    }
    /* A prior dynamic override can persist on the swapchain. Select FIFO for
     * each generated call too; never borrow the application's fence or ID. */
    const VkPresentModeKHR fifo = VK_PRESENT_MODE_FIFO_KHR;
    const VkSwapchainPresentModeInfoEXT present_mode = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext = pi.pNext, .swapchainCount = 1, .pPresentModes = &fifo};
    if (dynamic_fifo) pi.pNext = &present_mode;
    const double t = now_us();
    r = output_present(fg, fg->queue, &pi, next,(unsigned)k);
    *next_us += now_us() - t;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        fail_log(fg, "synthetic present", r);
        fg->disabled=1;fg->recreate_required=1;
        return -1;
    }
    if(!generated){
        fprintf(stderr,"[gfg-native] fallback_duplicate=1 synthetic=0 reason=record-failure\n");
        return -1;
    }
    STAT_ADD(synthetic_presents, 1);
    if (!fg->generation_logged) {
        fg->generation_logged = 1;
        fprintf(stderr, "[gfg-native] generation_started pid=%ld real_frame=%llu\n",
                (long)getpid(), (unsigned long long)fg->frame);
    }
    return 1;
}

VkResult gfg_gpu_fg_present(gfg_gpu_fg *fg, VkQueue queue, const VkPresentInfoKHR *app,
                            PFN_vkQueuePresentKHR next, const gfg_present_id_hooks *ids) {
    if (!fg || !app || !next) return VK_ERROR_INITIALIZATION_FAILED;
    const double t0 = now_us();
    double phase_before[5];memcpy(phase_before,fg->phase_us,sizeof(phase_before));
    fg->deadline_drop=0;fg->pressure_drop=0;
    fg->cut_drop=0;fg->duplicate_drop=0;fg->control_checked=0;
    memset(fg->readiness_ns,0,sizeof(fg->readiness_ns));
    memset(fg->accepted_ns,0,sizeof(fg->accepted_ns));
    memset(fg->wsi_ns,0,sizeof(fg->wsi_ns));
    fg->slot_drop[0]=fg->slot_drop[1]=DROP_HISTORY;
    fg->slot_detail[0]=fg->slot_detail[1]="not-admitted";
    memset(fg->readiness_wait_ns,0,sizeof(fg->readiness_wait_ns));
    memset(fg->google_ids,0,sizeof(fg->google_ids));
    fg->transport_submit_mask=0;
    if(!fg->source_ns)fg->source_ns=(uint64_t)(t0*1000.0);
    fg->source_serial=fg->frame+1;
    const uint64_t source0=fg->previous_source_ns,serial0=fg->previous_serial;
    fg->previous_source_ns=fg->source_ns;fg->previous_serial=fg->source_serial;
    gsf_refresh_begin(&fg->refresh_observation,fg->pacing_active,fg->clock.period_ns,
        getenv("GFG_NATIVE_OUTPUT_HZ")!=NULL,fg->refresh_cycle!=NULL);
    if(fg->pacing_active && fg->clock.period_ns) {
        /* Record the WSI response, not a claim about physical panel refresh.
         * A callable GDPA pointer alone is insufficient: the extension was enabled by
         * CreateDevice. Explicit OUTPUT_HZ remains a lab/user override. */
        if(fg->refresh_cycle && !getenv("GFG_NATIVE_OUTPUT_HZ")) {
            VkRefreshCycleDurationGOOGLE cycle={0};
            const uint64_t query_start=(uint64_t)(now_us()*1000.0);
            if(fg->output_wsi)pthread_mutex_lock(fg->output_wsi);
            VkResult q=fg->refresh_cycle(fg->device,fg->swapchain,&cycle);
            if(fg->output_wsi)pthread_mutex_unlock(fg->output_wsi);
            const uint64_t query_end=(uint64_t)(now_us()*1000.0);
            if(gsf_refresh_record(&fg->refresh_observation,q,cycle.refreshDuration,
                                  query_end-query_start,query_end))
                fg->clock.period_ns=cycle.refreshDuration;
        }
        gfg_frame_clock_pair(&fg->clock,fg->source_ns,fg->multiplier);
        if(fg->clock.discontinuity)fg->te_gap=1;
    }
    fg->output_deadline_us = t0 + (double)gfg_gpu_fg_source_period_ns(fg)/1000.0;
    unsigned dispatched_outputs=0,dispatched_mask=0;
    gsf_queue_snapshot queue_snapshot={0};
    unsigned generation_candidate_mask=0;
    const char *admission_decision="not_evaluated";
    uint64_t te_submit_sequence=0; /* this pair only; zero on bypass/failure */
    uint64_t compute_admission_ns=0;
    unsigned deferred_g2=0;uint64_t g2_submit_return_ns=0;
    double next_us = 0.0;
    STAT_ADD(real_presents, 1);
    const uint64_t frame = fg->frame++;
    /* Before calibration, probe every twelfth eligible source. Once a
     * trustworthy delay exists, use the same bounded recovery retry as
     * normal admission, never an expired slot or source backlog. */
    fg->probe_active=fg->visibility_probe && frame>=3 && frame%12==0 &&
        (fg->clock.mask&1u) && !fg->clock.overspeed &&
        !atomic_load(&fg->output_pressure) &&
        gsf_generation_probe_allowed(&fg->generation_budget,fg->last_generation_dispatch_ns,(uint64_t)(t0*1000.0)) &&
        (!fg->generation_budget.calibrated ||
         (gfg_frame_clock_compute_mask(&fg->clock,(uint64_t)(t0*1000.0),
            fg->display_timing==2?fg->clock.period_ns/4:0)&1u));
    const uint32_t idx = app->pImageIndices[0];
    VkResult result;
    gfg_present_chain app_chain;
    const int id_mode = ids && ids->assign ? ids->mode : 0;
    const char *chain_reason = gfg_pc_prepare(&app_chain, app, id_mode);
    if (chain_reason) {
        bypass_log(fg, 1u, chain_reason, app, id_mode);
        fg->te_gap = 1;
        if (frame) skip_outputs(fg, DROP_BYPASS, fg->multiplier-1);
        result = forward_real(fg, queue, app, NULL, next, NULL, &next_us);
        goto timing;
    }
    te_submit budget_preview={0};
    const uint64_t preview_ns=(uint64_t)(now_us()*1000.0);
    const int budget_skip=(fg->diagnostic==0 || fg->diagnostic==3) && fg->clock.period_ns && fg->gpu_budget_ns[0] &&
        gfg_frame_clock_compute_mask(&fg->clock,preview_ns,fg->display_timing==2?fg->clock.period_ns/4:0) &&
        !prepare_generation(fg,1,0,preview_ns,&budget_preview);
    if(fg->diagnostic==1 || budget_skip || ((fg->diagnostic==0 || fg->diagnostic==3) && atomic_load(&fg->output_pressure))) {
        /* Keep the queued real frame and its consumed private gate. Only
         * optional generation/history work is dropped; never overwrite WSI. */
        fg->te_gap=1;
        admission_decision=fg->diagnostic==1?"diagnostic_source_only":budget_skip?"budget_blocked":"source_pressure";
        const unsigned reason=budget_skip && !atomic_load(&fg->output_pressure)?DROP_RATE:DROP_BYPASS;
        if(frame)skip_outputs(fg,reason,fg->multiplier-1);
        for(unsigned k=0;k<fg->multiplier-1;++k) {
            fg->slot_drop[k]=(int)reason;
            fg->slot_detail[k]=fg->diagnostic?"diagnostic-source-only":reason==DROP_RATE?"gpu-budget":"source-backlog";
        }
        result=forward_real(fg,queue,app,NULL,next,ids,&next_us);
        goto timing;
    }
    if(fg->clock.period_ns && !fg->pacing_active) {
        /* GSF's cut/deadline admission belongs to the output worker. A
         * synchronous fallback must neither block the app nor count endpoint
         * recovery as generated content. Preserve this real present. */
        bypass_log(fg,4u,"gsf-requires-output-worker",app,id_mode);
        fg->te_gap=1;if(frame)skip_outputs(fg,DROP_BYPASS,fg->multiplier-1);
        result=forward_real(fg,queue,app,NULL,next,ids,&next_us);
        goto timing;
    }
    if (fg->disabled || queue != fg->queue || idx >= fg->image_count ||
        app->waitSemaphoreCount > 16) {
        const char *reason = fg->disabled ? "bridge-disabled" : queue != fg->queue ?
            "different-present-queue" : idx >= fg->image_count ? "invalid-image-index" : "too-many-wait-semaphores";
        bypass_log(fg, 2u, reason, app, id_mode);
        fg->te_gap = 1;
        if (frame) skip_outputs(fg, DROP_BYPASS, fg->multiplier-1);
        result = forward_real(fg, queue, app, NULL, next, ids, &next_us);
        goto timing;
    }
    if (app_chain.dynamic_mode && app_chain.requested_mode != VK_PRESENT_MODE_FIFO_KHR &&
        !fg->mode_override_logged) {
        fg->mode_override_logged = 1;
        fprintf(stderr, "[gfg-native] present-mode override requested=%d effective=%d reason=fg-fifo-contract\n",
                (int)app_chain.requested_mode, (int)VK_PRESENT_MODE_FIFO_KHR);
    }
    fg->in_present = 1;
    if (fg->verify) verify_collect(fg, 0);
    ring_slot *rs = NULL;
    int pass_through = 0;
    unsigned drop_reason = DROP_RING;
    /* test-only: alternate the two skip paths on every inject_every-th frame */
    const int inject = fg->inject_every && frame % fg->inject_every == fg->inject_every - 1;
    const int inject_te_busy = inject && (frame / fg->inject_every) % 2 == 0;
    if (inject && !inject_te_busy) { STAT_ADD(ring_busy, 1); pass_through = 1; }
    /* Optional generation never waits for a busy ring. Preserve the real
     * frame immediately; its resources and original waits stay untouched. */
    if(!pass_through)rs=reserve_ring(fg);
    if (!pass_through && !rs) {
        if (!fg->disabled) STAT_ADD(ring_busy, 1);
        else { drop_reason = DROP_ERROR; STAT_ADD(errors, 1); }
        pass_through = 1;
    }
    if (!pass_through && !ok(fg->ResetFences(fg->device, 1, &rs->fence))) {
        fg->disabled = 1;
        STAT_ADD(errors, 1);
        drop_reason = DROP_ERROR;
        pass_through = 1;
    }
    if (!pass_through && !record_real(fg, rs->real_cb, fg->images[idx], fg->src[fg->te_slot].image,
                                     fg->verify ? fg->verify_hist[fg->te_slot] : VK_NULL_HANDLE)) {
        fg->disabled = 1;
        STAT_ADD(errors, 1);
        drop_reason = DROP_ERROR;
        pass_through = 1;
    }
    if (pass_through) {
        /* app semaphores untouched: forward unmodified */
        fg->te_gap = 1;
        if (frame) skip_outputs(fg, drop_reason, fg->multiplier-1);
        fg->in_present = 0;
        result = forward_real(fg, queue, app, NULL, next, ids, &next_us);
        goto timing;
    }
    const uint64_t v = fg->next_value;
    double tp = now_us();
    VkResult r = submit_real(fg, rs, app, idx, v);
    fg->phase_us[0] += now_us() - tp;
    if (!ok(r)) {
        /* A failed vkQueueSubmit may have consumed the app's waits; never
         * replay them. Report it like the driver would. */
        fail_log(fg, "real submit", r);
        STAT_ADD(errors, 1);
        fg->disabled = 1;
        fg->in_present = 0;
        if (frame) skip_outputs(fg, DROP_ERROR, fg->multiplier-1);
        result = r;
        goto timing;
    }
    fg->next_value += 3;
    /* From here ready[idx] is signalled: the real present below waits it. */
    const int have_pair = fg->te_frames > 0 && !fg->te_gap;
    compute_admission_ns=(uint64_t)(now_us()*1000.0);
    te_submit s = {0};
    const unsigned compute_mask=prepare_generation(fg,have_pair,v,compute_admission_ns,&s);
    const unsigned timely_mask=fg->clock.period_ns?gfg_frame_clock_compute_mask(&fg->clock,compute_admission_ns,
        fg->display_timing==2?fg->clock.period_ns/4:0):compute_mask;
    const int generate=have_pair && compute_mask;
    generation_candidate_mask=compute_mask;
    admission_decision=generate?"admitted":!have_pair?"history_refresh":
        atomic_load(&fg->output_pressure)?"source_pressure":!fg->clock.mask?"not_planned":
        !timely_mask?"deadline_blocked":"budget_blocked";
    /* Observations do not prove individual submission when a later signal
     * jumps over a skipped timeline value. */
    if(generate && s.count) {
        const uint64_t lead=fg->display_timing==2?fg->clock.period_ns/4:0;
        for(unsigned j=0;j<s.count;++j) {
            unsigned k=0,seen=0;
            for(;k<fg->multiplier-1;++k)if(compute_mask&(1u<<k)) {
                if(seen++==j)break;
            }
            const uint64_t deadline=fg->probe_active?
                compute_admission_ns+12000000ull:
                fg->clock.target_ns[k]>lead?fg->clock.target_ns[k]-lead:0;
            tw_register(fg->timeline_observer,v,fg->te_frames+1,j,
                v+1+(uint64_t)j,s.signal_first+j,deadline,fg->probe_active);
        }
    }
    tp = now_us();
    if(generate && fg->queue_probe)fg->queue_probe(fg->queue,&queue_snapshot);
    VkResult tr = inject_te_busy ? VK_NOT_READY : te_submit_frame(fg->context, &s);
    if(generate && fg->queue_probe)fg->queue_probe(fg->queue,NULL);
    fg->phase_us[1] += now_us() - tp;
    if(tr==VK_SUCCESS && generate && s.count)
        gsf_calibration_maybe_sample(&fg->calibrated,fg->frame);
    if(generate && s.count)tw_submitted(fg->timeline_observer,v,tr==VK_SUCCESS);
    gpu_sample(fg);
    int presented = 0;
    drop_reason = have_pair && !compute_mask?(atomic_load(&fg->output_pressure)?DROP_BYPASS:
        !timely_mask && fg->clock.mask?DROP_DEADLINE:DROP_RATE):DROP_HISTORY;
    if (tr == VK_SUCCESS) {
        if(generate)fg->last_generation_dispatch_ns=compute_admission_ns;
        if(fg->diagnostic==2 || fg->diagnostic==3)fg->diagnostic_outputs+=s.count;
        deferred_g2=(unsigned)s.defer_second;
        dispatched_outputs=s.count;dispatched_mask=generate?compute_mask:0;
        fg->last_te_value = v + 2;
        fg->te_slot ^= 1u;
        fg->te_frames++;
        te_submit_sequence=fg->te_frames;
        fg->te_gap = 0;
        if (!generate) STAT_ADD(holds, 1);
        if (!s.count) ++fg->history_only_submits;
        if (generate) {
            unsigned generated_index=0;
            unsigned canceled_second=DROP_COUNT;
            for (unsigned k = 0; k < fg->multiplier-1; ++k) {
                fg->deadline_drop=0;
                if(fg->diagnostic==2 || fg->diagnostic==3) {
                    fg->slot_detail[k]=fg->diagnostic==3?"diagnostic-matched-compute":"diagnostic-compute-only";fg->slot_drop[k]=DROP_BYPASS;
                    skip_outputs(fg,DROP_BYPASS,1);continue;
                }
                if(k==1 && canceled_second<DROP_COUNT) {
                    fg->slot_drop[k]=(int)canceled_second;
                    fg->slot_detail[k]=canceled_second==DROP_CUT?"G2-not-dispatched-cut":
                        canceled_second==DROP_DUPLICATE?"G2-not-dispatched-duplicate":
                        canceled_second==DROP_DEADLINE?"G2-not-dispatched-deadline":"G2-not-dispatched-error";
                    skip_outputs(fg,canceled_second,1);continue;
                }
                if(fg->clock.period_ns && !(fg->clock.mask&(1u<<k))) {
                    fg->slot_detail[k]="display-refresh-budget";fg->slot_drop[k]=DROP_RATE;skip_outputs(fg,DROP_RATE,1);continue;
                }
                if(!(compute_mask&(1u<<k))) {
                    const unsigned reason=timely_mask&(1u<<k)?DROP_RATE:DROP_DEADLINE;
                    fg->slot_detail[k]=reason==DROP_RATE?"gpu-budget":"expired-before-dispatch";
                    fg->slot_drop[k]=(int)reason;skip_outputs(fg,reason,1);continue;
                }
                int g = present_synthetic(fg, rs, (int)k, (int)generated_index++, v+2-s.count, idx, next, ids, app_chain.dynamic_mode, &next_us);
                VkResult resumed=VK_SUCCESS;
                if(s.defer_second && k==0) {
                    /* QueuePresent itself may append a driver synchronization
                     * submit. Resume G2 AFTER that call, not merely after the
                     * G1 copy, so lower WSI cannot put G1 behind G2 again. A
                     * skipped G1 still releases G2 to its own later deadline. */
                    const uint64_t now=(uint64_t)(now_us()*1000.0);
                    const uint64_t target=gfg_frame_clock_target(&fg->clock,1)-
                        (fg->display_timing==2?fg->clock.period_ns/4:0);
                    canceled_second=g<0?DROP_ERROR:fg->pressure_drop?DROP_BYPASS:fg->cut_drop?DROP_CUT:fg->duplicate_drop?DROP_DUPLICATE:
                        fg->clock.period_ns && now>=target?DROP_DEADLINE:DROP_COUNT;
                    resumed=canceled_second<DROP_COUNT?te_cancel_deferred_output(fg->context):te_resume_outputs(fg->context);
                    if(canceled_second<DROP_COUNT){dispatched_outputs=1;dispatched_mask=1;}
                    if(resumed==VK_SUCCESS) {
                        if(canceled_second==DROP_COUNT)g2_submit_return_ns=(uint64_t)(now_us()*1000.0);
                    } else {
                        fg->disabled=1;STAT_ADD(errors,1);
                        dispatched_outputs=1;dispatched_mask=1;
                        fail_log(fg,"deferred G2 submit",resumed);
                    }
                }
                if (g <= 0) {
                    if (g < 0) STAT_ADD(errors, 1);
                    unsigned reason=g<0?DROP_ERROR:fg->pressure_drop?DROP_BYPASS:fg->cut_drop?DROP_CUT:fg->duplicate_drop?DROP_DUPLICATE:fg->deadline_drop?DROP_DEADLINE:DROP_ACQUIRE;
                    fg->slot_drop[k]=(int)reason;skip_outputs(fg,reason,1);
                    /* G1 lateness does not cancel G2. A fatal transport error
                     * does; never reuse a poisoned semaphore/image. */
                    if(g<0) {
                        for(++k;k<fg->multiplier-1;++k){fg->slot_drop[k]=DROP_ERROR;skip_outputs(fg,DROP_ERROR,1);}
                        break;
                    }
                } else {++presented;fg->slot_drop[k]=-1;fg->slot_detail[k]="WSI-accepted";}
                if(resumed!=VK_SUCCESS) {
                    for(++k;k<fg->multiplier-1;++k){fg->slot_drop[k]=DROP_ERROR;skip_outputs(fg,DROP_ERROR,1);}
                    break;
                }
            }
            /* Defensive flush if a future admission policy bypasses G1. The
             * normal path already resumed/canceled before reaching this. */
            if(s.defer_second && te_resume_outputs(fg->context)!=VK_SUCCESS) {
                fg->disabled=1;STAT_ADD(errors,1);
            }
            if (fg->verify && presented == (int)(fg->multiplier - 1) && frame % fg->verify_every == 0)
                verify_submit(fg, v, fg->te_slot ^ 1u);
        }
    } else {
        drop_reason = tr == VK_NOT_READY ? DROP_TE : DROP_ERROR;
        if (tr == VK_NOT_READY) STAT_ADD(te_busy, 1);
        else {
            fprintf(stderr, "[gfg-native] gpu-bridge te_submit: %s (%d)\n", te_last_error(), (int)tr);
            STAT_ADD(errors, 1);
            fg->disabled = 1;
        }
        fg->te_gap = 1;
    }
    if (frame && !(tr==VK_SUCCESS && generate)) {
        skip_outputs(fg,drop_reason,fg->multiplier-1);
        fg->slot_drop[0]=fg->slot_drop[1]=(int)drop_reason;
        if(tr==VK_SUCCESS && have_pair && !compute_mask && fg->clock.mask)
            for(unsigned k=0;k<fg->multiplier-1;++k)if(fg->clock.mask&(1u<<k))
                fg->slot_detail[k]=drop_reason==DROP_BYPASS?"source-backlog":
                    timely_mask&(1u<<k)?"gpu-budget":"expired-before-dispatch";
    }
    /* the ring slot's fence covers everything submitted for this frame */
    tp = now_us();
    const VkResult fence_submit = fg->QueueSubmit(fg->queue, 0, NULL, rs->fence);
    if (ok(fence_submit)) rs->busy = 1;
    else { fail_log(fg, "ring fence submit", fence_submit); STAT_ADD(errors, 1); fg->disabled = 1; }
    fg->phase_us[4] += now_us() - tp;
    fg->phase_n++;
    fg->in_present = 0;
    /* Real-only fences/timing and immutable copied metadata stay on the app's
     * real present; synthetic calls never borrow those completion objects. */
    fg->pace_real=presented>0 && !fg->probe_active;
    result = forward_real(fg, queue, app, &fg->ready[idx], next, ids, &next_us);
    if(fg->recreate_required && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)){
        result=VK_ERROR_OUT_OF_DATE_KHR;
        if(app->pResults) app->pResults[0]=result;
    }
    if (fg->log && (frame < 4 || frame % 120 == 0))
        fprintf(stderr, "[gfg-native] gpu-bridge frame=%llu te=%d generated=%d presented=%d\n",
                (unsigned long long)frame, (int)tr, generate, presented);
timing:
    if(fg->probe_active)
        fprintf(stderr,"[gfg-native] visibility_probe frame=%llu synthetic_wsi=%d real_wsi=%d detail=%s bounded_wait_ms=12\n",
            (unsigned long long)frame,fg->slot_drop[0]==-1,fg->accepted_ns[2]!=0,fg->slot_detail[0]);
    if(fg->telemetry) {
        if(queue_snapshot.captures)fprintf(fg->telemetry,
            "{\"schema\":1,\"event\":\"queue_observation\",\"pid\":%ld,\"generation\":%llu,"
            "\"te_sequence\":%llu,\"source1_id\":%llu,\"sample_ns\":%llu,\"query_cpu_ns\":%llu,"
            "\"available\":%u,\"te_submit_calls\":%u,\"app_submits_total\":%llu,\"confirmed_complete_through\":%llu,"
            "\"app_submits_unretired_upper_bound\":%llu,\"tracked_fences_not_ready\":%u,"
            "\"unfenced_unretired\":%llu,\"tracking_lost\":%u,\"query_errors\":%u,"
            "\"other_queue_unretired_upper_bound\":%llu,\"hardware_queue_depth\":null,\"pure_queue_wait_ns\":null}\n",
            (long)getpid(),(unsigned long long)fg->generation,(unsigned long long)te_submit_sequence,
            (unsigned long long)fg->source_serial,(unsigned long long)queue_snapshot.captured_ns,
            (unsigned long long)queue_snapshot.query_cpu_ns,queue_snapshot.available,queue_snapshot.captures,
            (unsigned long long)queue_snapshot.submitted,(unsigned long long)queue_snapshot.confirmed_complete_through,
            (unsigned long long)queue_snapshot.unretired_upper_bound,queue_snapshot.tracked_not_ready,
            (unsigned long long)queue_snapshot.unfenced_unretired,queue_snapshot.tracking_lost,queue_snapshot.query_errors,
            (unsigned long long)queue_snapshot.other_queue_unretired);
        char refresh_result[32]="null";
        if(fg->refresh_observation.attempted)snprintf(refresh_result,sizeof(refresh_result),"%d",
            (int)fg->refresh_observation.result);
        fprintf(fg->telemetry,"{\"schema\":1,\"event\":\"pair\",\"pid\":%ld,\"generation\":%llu,\"pair\":%llu,"
            "\"source0_id\":%llu,\"source1_id\":%llu,\"source0_ns\":%llu,\"source1_ns\":%llu,"
            "\"admitted_ns\":%llu,\"worker_start_ns\":%llu,\"worker_end_ns\":%llu,\"interval_ns\":%llu,"
            "\"refresh_period_ns\":%llu,\"refresh_origin\":\"%s\",\"refresh_query_state\":\"%s\","
            "\"refresh_query_result\":%s,\"refresh_query_raw_ns\":%llu,\"refresh_query_cpu_ns\":%llu,"
            "\"refresh_last_success_ns\":%llu,\"refresh_override_present\":%u,\"multiplier\":%u,\"planned_mask\":%u,\"dispatched_outputs\":%u,\"compute_mask\":%u,\"transport_submit_mask\":%u,\"source_discontinuity\":%u,"
            "\"admission_decision\":\"%s\",\"generation_candidate_mask\":%u,\"compute_admission_ns\":%llu,\"gpu_budget_ns\":[%llu,%llu],\"submit_delay_budget_ns\":%llu,\"submit_delay_budget_calibrated\":%d,\"deferred_g2\":%u,\"g2_submit_return_ns\":%llu,\"te_submit_sequence\":%llu,"
            "\"target_ns\":[%llu,%llu,%llu],\"readiness_observed_ns\":[%llu,%llu],"
            "\"wsi_accepted_ns\":[%llu,%llu,%llu],\"wsi_call_ns\":[%llu,%llu,%llu],"
            "\"readiness_wait_ns\":[%llu,%llu],\"google_present_ids\":[%u,%u,%u],\"drop_detail\":[\"%s\",\"%s\"],\"cpu_phase_us\":[%.3f,%.3f,%.3f,%.3f,%.3f],\"te_sequence\":%llu,\"drop_code\":[%d,%d],\"cut\":%d,\"duplicate\":%d,\"physical_displayed\":null,\"visually_unique\":null}\n",
            (long)getpid(),(unsigned long long)fg->generation,(unsigned long long)fg->frame,
            (unsigned long long)serial0,(unsigned long long)fg->source_serial,
            (unsigned long long)source0,(unsigned long long)fg->source_ns,
            (unsigned long long)fg->admitted_ns,(unsigned long long)(t0*1000.0),(unsigned long long)(now_us()*1000.0),
            (unsigned long long)fg->clock.interval_ns,(unsigned long long)fg->clock.period_ns,
            fg->refresh_observation.origin,fg->refresh_observation.query_state,refresh_result,
            (unsigned long long)fg->refresh_observation.raw_ns,(unsigned long long)fg->refresh_observation.query_cpu_ns,
            (unsigned long long)fg->refresh_observation.last_success_ns,fg->refresh_observation.override_present,fg->multiplier,fg->clock.mask,dispatched_outputs,dispatched_mask,fg->transport_submit_mask,fg->clock.discontinuity,
            admission_decision,generation_candidate_mask,(unsigned long long)compute_admission_ns,(unsigned long long)fg->gpu_budget_ns[0],
            (unsigned long long)fg->gpu_budget_ns[1],(unsigned long long)fg->generation_budget.submit_delay_ns,
            fg->generation_budget.calibrated,deferred_g2,(unsigned long long)g2_submit_return_ns,
            (unsigned long long)te_submit_sequence,
            (unsigned long long)fg->clock.target_ns[0],(unsigned long long)fg->clock.target_ns[1],(unsigned long long)fg->clock.target_ns[2],
            (unsigned long long)fg->readiness_ns[0],(unsigned long long)fg->readiness_ns[1],
            (unsigned long long)fg->accepted_ns[0],(unsigned long long)fg->accepted_ns[1],(unsigned long long)fg->accepted_ns[2],
            (unsigned long long)fg->wsi_ns[0],(unsigned long long)fg->wsi_ns[1],(unsigned long long)fg->wsi_ns[2],
            (unsigned long long)fg->readiness_wait_ns[0],(unsigned long long)fg->readiness_wait_ns[1],
            fg->google_ids[0],fg->google_ids[1],fg->google_ids[2],fg->slot_detail[0],fg->slot_detail[1],
            fg->phase_us[0]-phase_before[0],fg->phase_us[1]-phase_before[1],fg->phase_us[2]-phase_before[2],
            fg->phase_us[3]-phase_before[3],fg->phase_us[4]-phase_before[4],(unsigned long long)fg->te_frames,
            fg->slot_drop[0],fg->slot_drop[1],fg->cut_drop,fg->duplicate_drop);
    }
    if(fg->display_timing==2 && fg->past_timing && fg->telemetry) {
        VkPastPresentationTimingGOOGLE past[16];uint32_t n=0;
        if(fg->output_wsi)pthread_mutex_lock(fg->output_wsi);
        VkResult feedback=gfg_past_timing_read(fg->past_timing,fg->device,fg->swapchain,16,past,&n);
        if(fg->output_wsi)pthread_mutex_unlock(fg->output_wsi);
        if(feedback==VK_SUCCESS || feedback==VK_INCOMPLETE)for(uint32_t j=0;j<n && j<16;++j)
            fprintf(fg->telemetry,"{\"schema\":1,\"event\":\"compositor_feedback\",\"pid\":%ld,\"generation\":%llu,"
                "\"present_id\":%u,\"desired_ns\":%llu,\"actual_ns\":%llu,\"earliest_ns\":%llu,\"margin_ns\":%llu,"
                "\"physical_displayed\":null,\"visually_unique\":null}\n",(long)getpid(),(unsigned long long)fg->generation,
                past[j].presentID,(unsigned long long)past[j].desiredPresentTime,(unsigned long long)past[j].actualPresentTime,
                (unsigned long long)past[j].earliestPresentTime,(unsigned long long)past[j].presentMargin);
    }
    fg->real_accepted+=fg->accepted_ns[2]!=0;
    fg->generated_accepted+=(fg->slot_drop[0]==-1)+(fg->multiplier==3 && fg->slot_drop[1]==-1);
    uint64_t status_now=(uint64_t)(now_us()*1000.0);
    telemetry_flush(fg,status_now);
    if(fg->status_dir && status_now-fg->status_last_ns>=250000000ull) {
        char path[4096],temp[4096];
        int n=snprintf(path,sizeof(path),"%s/%ld-%llu.json",fg->status_dir,(long)getpid(),(unsigned long long)fg->generation);
        int m=snprintf(temp,sizeof(temp),"%s/.%ld-%llu.tmp",fg->status_dir,(long)getpid(),(unsigned long long)fg->generation);
        if(n>0 && n<(int)sizeof(path) && m>0 && m<(int)sizeof(temp)) {
            int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
            FILE *status=fd>=0?fdopen(fd,"w"):NULL;
            if(status) {
                struct timespec real;clock_gettime(CLOCK_REALTIME,&real);
                const char *reason=fg->disabled?"bridge-disabled":fg->cut_drop?"scene-cut":fg->duplicate_drop?"duplicate-source":
                    fg->accepted_ns[0]||fg->accepted_ns[1]?"WSI-accepted":fg->deadline_drop?"deadline-miss":"history-or-display-budget";
                fprintf(status,"{\"schema\":1,\"engine\":\"gfg-native-gsf\",\"pid\":%ld,\"generation\":%llu,"
                    "\"process_start_ticks\":%llu,\"updated_unix_ms\":%llu,\"multiplier\":%u,\"reason\":\"%s\","
                    "\"source_fps\":%.3f,\"real_wsi_accepted\":%llu,\"generated_wsi_accepted\":%llu,"
                    "\"color_space\":%d,\"color_transfer\":\"%s\",\"physical_displayed_fps\":null}\n",
                    (long)getpid(),(unsigned long long)fg->generation,(unsigned long long)fg->process_start_ticks,
                    (unsigned long long)((uint64_t)real.tv_sec*1000ull+(uint64_t)real.tv_nsec/1000000ull),fg->multiplier,reason,
                    fg->clock.interval_ns?1e9/(double)fg->clock.interval_ns:0.0,
                    (unsigned long long)fg->real_accepted,(unsigned long long)fg->generated_accepted,
                    (int)fg->color_space,fg->linear_pq?"pq-linear-nits":"coded");
                if(fclose(status)==0)rename(temp,path);else unlink(temp);
            }else if(fd>=0)close(fd);
        }
        fg->status_last_ns=status_now;
    }
    fg->source_ns=fg->source_serial=fg->admitted_ns=0;
    fg->probe_active=0;
    if (fg->timing_total_us && fg->timing_count < TIMING_CAP) {
        const double total = now_us() - t0;
        fg->timing_total_us[fg->timing_count] = total;
        fg->timing_own_us[fg->timing_count] = total - next_us;
        fg->timing_count++;
    }
    return result;
}
