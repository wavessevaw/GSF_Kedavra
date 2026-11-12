/* SPDX-License-Identifier: GPL-3.0-or-later
 * Passive, opt-in Vulkan CPU/GPU timestamp calibration for 0.1.6.
 * CLOCK_MONOTONIC is used for both calibration and TE submit timestamps.
 * This is NOT a pure GPU queue-wait estimator; Vulkan command delivery,
 * dependencies, TOP_OF_PIPE timestamp delay and other factors are included.
 */
#ifndef GSF_CALIBRATED_CLOCK_H
#define GSF_CALIBRATED_CLOCK_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define GSF_CALIB_HISTORY 64u
#define GSF_CALIB_INTERVAL_NS 100000000ull
#define GSF_CALIB_MAX_STALE_NS 250000000ull
#define GSF_CALIB_MAX_DEVIATION_NS 1000000ull

typedef VkResult (VKAPI_PTR *gsf_calibrated_domains_fn)(VkPhysicalDevice,uint32_t*,VkTimeDomainKHR*);
typedef VkResult (VKAPI_PTR *gsf_calibrated_get_fn)(VkDevice,uint32_t,
    const VkCalibratedTimestampInfoKHR*,uint64_t*,uint64_t*);

typedef struct {
    uint64_t cpu_ns, gpu_ticks, deviation_ns;
} gsf_calibration_pair;

typedef struct {
    VkDevice device;
    VkPhysicalDevice physical;
    gsf_calibrated_get_fn calibrated_get;
    gsf_calibration_pair samples[GSF_CALIB_HISTORY];
    uint32_t next, size, every_frames;
    uint64_t last_ns, last_frame;
    FILE *log;
    int enabled, extension; /* 1 KHR, 2 EXT */
} gsf_calibrated_clock;

typedef struct {
    char start_text[32], delay_text[32], calibration_text[32], deviation_text[32];
    char delta_text[32]; /* signed GPU start minus host submit return */
    uint64_t start_ns, delay_ns; /* valid only: delay clamps already-started work to zero */
    const char *reason;
    int valid;
} gsf_clock_estimate;

static uint64_t gsf_calib_monotonic(void) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}

static void gsf_calibration_init(gsf_calibrated_clock *state,
    VkInstance instance,VkPhysicalDevice physical,VkDevice device,
    PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa,
    int extension,uint32_t every_frames,FILE *telemetry) {
    memset(state,0,sizeof(*state));
    state->log=telemetry;state->every_frames=every_frames?every_frames:10;
    const char *status="not_enabled";
    if(extension && gipa && gdpa) {
        const char *domains_name=extension==1?
            "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR":
            "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT";
        const char *get_name=extension==1?
            "vkGetCalibratedTimestampsKHR":"vkGetCalibratedTimestampsEXT";
        gsf_calibrated_domains_fn domains=(gsf_calibrated_domains_fn)gipa(instance,domains_name);
        gsf_calibrated_get_fn get=(gsf_calibrated_get_fn)gdpa(device,get_name);
        status="entrypoint_unavailable";
        if(domains && get) {
            VkTimeDomainKHR values[16];uint32_t n=16;
            status="domain_unavailable";
            VkResult queried=domains(physical,&n,values);
            int device_ok=0,host_ok=0;
            if(queried==VK_SUCCESS || queried==VK_INCOMPLETE) {
                for(uint32_t i=0;i<n && i<16;++i){
                    if(values[i]==VK_TIME_DOMAIN_DEVICE_KHR)device_ok=1;
                    if(values[i]==VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)host_ok=1;
                }
                if(device_ok && host_ok) {
                    state->device=device;state->physical=physical;
                    state->calibrated_get=get;state->extension=extension;
                    state->enabled=1;status="enabled";
                }
            }
        }
    }
    if(telemetry)fprintf(telemetry,
        "{\"schema\":1,\"event\":\"calibration_init\",\"status\":\"%s\","
        "\"extension\":\"%s\",\"host_domain\":\"CLOCK_MONOTONIC\","
        "\"max_deviation_limit_ns\":%llu}\n",
        status,extension==1?"KHR":extension==2?"EXT":"none",
        (unsigned long long)GSF_CALIB_MAX_DEVIATION_NS);
}

static void gsf_calibration_maybe_sample(gsf_calibrated_clock *s,uint64_t frame) {
    if(!s->enabled)return;
    const uint64_t now=gsf_calib_monotonic();
    if(s->size && frame-s->last_frame<s->every_frames &&
        now-s->last_ns<GSF_CALIB_INTERVAL_NS)return;
    VkCalibratedTimestampInfoKHR infos[2]={
        {.sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,.timeDomain=VK_TIME_DOMAIN_DEVICE_KHR},
        {.sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,.timeDomain=VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}
    };
    uint64_t values[2]={0},deviation=UINT64_MAX;
    VkResult rc=s->calibrated_get(s->device,2,infos,values,&deviation);
    s->last_frame=frame;s->last_ns=gsf_calib_monotonic();
    if(rc!=VK_SUCCESS || !values[0] || !values[1]){
        if(s->log)fprintf(s->log,"{\"schema\":1,\"event\":\"calibration\",\"status\":\"failed\",\"vkresult\":%d}\n",(int)rc);
        return;
    }
    gsf_calibration_pair p={.gpu_ticks=values[0],.cpu_ns=values[1],.deviation_ns=deviation};
    s->samples[s->next]=p;s->next=(s->next+1)%GSF_CALIB_HISTORY;
    if(s->size<GSF_CALIB_HISTORY)++s->size;
    if(s->log)fprintf(s->log,
        "{\"schema\":1,\"event\":\"calibration\",\"status\":\"%s\","
        "\"calibration_ns\":%llu,\"calibration_gpu_ticks\":%llu,"
        "\"maxDeviation_ns\":%llu,\"frame\":%llu}\n",
        deviation<=GSF_CALIB_MAX_DEVIATION_NS?"usable":"imprecise",
        (unsigned long long)p.cpu_ns,(unsigned long long)p.gpu_ticks,
        (unsigned long long)p.deviation_ns,(unsigned long long)frame);
}

static gsf_clock_estimate gsf_calibration_estimate(
    const gsf_calibrated_clock *s,uint64_t gpu_start_ticks,
    double timestamp_period_ns,uint64_t submit_return_ns) {
    gsf_clock_estimate out={.start_text="null",.delay_text="null",
        .calibration_text="null",.deviation_text="null",.delta_text="null",.reason="not_calibrated"};
    if(!s->enabled || !s->size)return out;
    if(!timestamp_period_ns || timestamp_period_ns<0.001 || timestamp_period_ns>1000000.0 ||
       !submit_return_ns)return out;
    const gsf_calibration_pair *best=NULL;
    long double best_ns=1e30L;
    for(uint32_t i=0;i<s->size;++i){
        const gsf_calibration_pair *p=&s->samples[i];
        long double dist=p->gpu_ticks>=gpu_start_ticks?
            (long double)(p->gpu_ticks-gpu_start_ticks)*timestamp_period_ns:
            (long double)(gpu_start_ticks-p->gpu_ticks)*timestamp_period_ns;
        if(dist<=best_ns){best_ns=dist;best=p;}
    }
    if(!best)return out;
    snprintf(out.calibration_text,sizeof(out.calibration_text),"%llu",
        (unsigned long long)best->cpu_ns);
    snprintf(out.deviation_text,sizeof(out.deviation_text),"%llu",
        (unsigned long long)best->deviation_ns);
    if(best->deviation_ns>GSF_CALIB_MAX_DEVIATION_NS){
        out.reason="maxDeviation_over_limit";return out;
    }
    if(best_ns>GSF_CALIB_MAX_STALE_NS){
        out.reason="calibration_stale";return out;
    }
    long double offset=(gpu_start_ticks>=best->gpu_ticks?1.0L:-1.0L)*
        best_ns;
    long double cpu=(long double)best->cpu_ns+offset;
    if(cpu<0 || cpu>18446744073709551615.0L){out.reason="time_overflow";return out;}
    uint64_t start=(uint64_t)cpu;
    /* Device execution may start while vkQueueSubmit is still returning.
     * Preserve that signed observation. Admission only budgets the remaining
     * delay after host return, so already-started work contributes zero.
     * Keep the existing 2s plausibility bound; never wrap unsigned subtraction. */
    const uint64_t distance=start>=submit_return_ns?start-submit_return_ns:submit_return_ns-start;
    if(distance>2000000000ull){out.reason="inconsistent_timestamps";return out;}
    const int64_t delta=start>=submit_return_ns?(int64_t)distance:-(int64_t)distance;
    snprintf(out.delta_text,sizeof(out.delta_text),"%" PRId64,delta);
    snprintf(out.start_text,sizeof(out.start_text),"%llu",(unsigned long long)start);
    out.start_ns=start;out.delay_ns=start>=submit_return_ns?distance:0;
    snprintf(out.delay_text,sizeof(out.delay_text),"%llu",(unsigned long long)out.delay_ns);
    out.reason="calibrated";out.valid=1;return out;
}
#endif
