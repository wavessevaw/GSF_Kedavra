/* SPDX-License-Identifier: GPL-3.0-or-later
 * Refresh-query provenance. No inference from measured presents, GPU waits,
 * source FPS or display model. Preserve the existing period-selection bounds.
 */
#ifndef GSF_REFRESH_OBSERVATION_H
#define GSF_REFRESH_OBSERVATION_H
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    const char *origin;             /* retained across failed/missing queries */
    const char *query_state;        /* describes this pair, not the last success */
    VkResult result;
    unsigned attempted, override_present;
    uint64_t raw_ns, query_cpu_ns, last_success_ns;
} gsf_refresh_observation;

static inline void gsf_refresh_init(gsf_refresh_observation *o,uint64_t period,int explicit_override) {
    *o=(gsf_refresh_observation){.origin=explicit_override?"explicit_override":
        period?"configured_fallback":"unset",.query_state="not_sampled"};
}
static inline void gsf_refresh_begin(gsf_refresh_observation *o,int pacing,uint64_t period,
                                     int override_present,int callable) {
    o->attempted=0;o->result=VK_SUCCESS;o->raw_ns=0;o->query_cpu_ns=0;
    o->override_present=override_present!=0;
    o->query_state=!pacing || !period?"inactive":override_present?"override_present":
        !callable?"unavailable":"pending";
}
static inline int gsf_refresh_record(gsf_refresh_observation *o,VkResult result,uint64_t raw_ns,
                                     uint64_t cpu_ns,uint64_t now_ns) {
    o->attempted=1;o->result=result;o->raw_ns=raw_ns;o->query_cpu_ns=cpu_ns;
    if(result!=VK_SUCCESS){o->query_state="query_failed";return 0;}
    if(raw_ns<1000000000ull/240 || raw_ns>1000000000ull) {
        o->query_state="rejected_range";return 0;
    }
    o->origin="successful_wsi_query";o->query_state="accepted";o->last_success_ns=now_ns;
    return 1;
}
#endif
