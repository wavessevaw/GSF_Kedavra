/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/calibrated_clock.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int has_host=1;
static uint64_t deviation=500000;
static VkResult fake_domains(VkPhysicalDevice phys,uint32_t *n,VkTimeDomainKHR *values){
    (void)phys;
    if(*n<2)return VK_INCOMPLETE;
    *n=has_host?2:1;
    values[0]=VK_TIME_DOMAIN_DEVICE_KHR;
    if(has_host)values[1]=VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    return VK_SUCCESS;
}
static VkResult fake_get(VkDevice device,uint32_t n,const VkCalibratedTimestampInfoKHR *infos,
                         uint64_t *values,uint64_t *maxdev){
    (void)device;
    assert(n==2);
    assert(infos[0].timeDomain==VK_TIME_DOMAIN_DEVICE_KHR);
    assert(infos[1].timeDomain==VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
    values[0]=1000;values[1]=20000000000ull;*maxdev=deviation;return VK_SUCCESS;
}
static PFN_vkVoidFunction fake_gipa(VkInstance i,const char *n){
    (void)i;
    return strstr(n,"CalibrateableTimeDomains")?(PFN_vkVoidFunction)fake_domains:NULL;
}
static PFN_vkVoidFunction fake_gdpa(VkDevice d,const char *n){
    (void)d;
    return strstr(n,"CalibratedTimestamps")?(PFN_vkVoidFunction)fake_get:NULL;
}
static void replay_overlap(const char *path) {
    FILE *f=fopen(path,"r");assert(f);char line[1024];unsigned checked=0;
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='#' || line[0]=='t')continue;
        uint64_t seq,cpu,gpu,dev,start,returned;int64_t expected;
        assert(sscanf(line,"%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64
            ",%" SCNu64 ",%" SCNu64 ",%" SCNd64,
            &seq,&cpu,&gpu,&dev,&start,&returned,&expected)==7);
        (void)seq;
        gsf_calibrated_clock c={.enabled=1,.size=1,
            .samples={{.cpu_ns=cpu,.gpu_ticks=gpu,.deviation_ns=dev}}};
        gsf_clock_estimate out=gsf_calibration_estimate(&c,start,1.0,returned);
        assert(out.valid && !strcmp(out.reason,"calibrated"));
        assert(expected<0 && strtoll(out.delta_text,NULL,10)==expected);
        assert(out.start_ns<returned && out.delay_ns==0 && !strcmp(out.delay_text,"0"));
        ++checked;
    }
    fclose(f);assert(checked==15);
    printf("GSF real submit-overlap replay: %u previously rejected samples now calibrated, zero remaining start delay PASS\n",checked);
}
int main(int argc,char **argv) {
    FILE *log=tmpfile();assert(log);
    gsf_calibrated_clock c={0};
    gsf_calibration_init(&c,(VkInstance)1,(VkPhysicalDevice)1,(VkDevice)1,
        fake_gipa,fake_gdpa,1,10,log);
    assert(c.enabled);
    gsf_calibration_maybe_sample(&c,1);
    assert(c.size==1);
    gsf_clock_estimate out=gsf_calibration_estimate(&c,1020,1000.0,19999999000ull);
    assert(out.valid && !strcmp(out.delay_text,"21000"));
    assert(!strcmp(out.start_text,"20000020000"));
    assert(out.start_ns==20000020000ull && out.delay_ns==21000);
    assert(!strcmp(out.reason,"calibrated"));
    /* Regression: do not claim precise time when maxDeviation > 1ms. */
    deviation=2000000;
    gsf_calibration_maybe_sample(&c,11);
    out=gsf_calibration_estimate(&c,1000,1000.0,19999999000ull);
    assert(!out.valid && !strcmp(out.reason,"maxDeviation_over_limit"));
    deviation=500000;
    gsf_calibration_maybe_sample(&c,21);
    out=gsf_calibration_estimate(&c,1000000000,1000.0,19999999000ull);
    assert(!out.valid && !strcmp(out.reason,"calibration_stale"));
    has_host=0;
    gsf_calibrated_clock missing={0};
    gsf_calibration_init(&missing,(VkInstance)1,(VkPhysicalDevice)1,(VkDevice)1,
        fake_gipa,fake_gdpa,1,10,log);
    assert(!missing.enabled);
    out=gsf_calibration_estimate(&missing,1020,1000.0,19999999000ull);
    assert(!out.valid && !strcmp(out.reason,"not_calibrated"));
    /* GPU may legally begin before queue-submit returns on the CPU. Keep
     * the signed delta, budget no negative time, retain plausibility bounds. */
    gsf_calibrated_clock overlap={.enabled=1,.size=1,
        .samples={{.cpu_ns=20000000000ull,.gpu_ticks=1000,.deviation_ns=1}}};
    out=gsf_calibration_estimate(&overlap,1000,1.0,20000001000ull);
    assert(out.valid && out.start_ns==20000000000ull && out.delay_ns==0);
    assert(!strcmp(out.delta_text,"-1000") && !strcmp(out.delay_text,"0"));
    out=gsf_calibration_estimate(&overlap,1000,1.0,20000000000ull);
    assert(out.valid && out.delay_ns==0 && !strcmp(out.delta_text,"0"));
    out=gsf_calibration_estimate(&overlap,1000,1.0,22000000001ull);
    assert(!out.valid && !strcmp(out.reason,"inconsistent_timestamps"));
    out=gsf_calibration_estimate(&overlap,1000,1.0,17999999999ull);
    assert(!out.valid && !strcmp(out.reason,"inconsistent_timestamps"));
    if(argc==2)replay_overlap(argv[1]);
    fclose(log);
    puts("GSF calibration: paired clocks, maxDeviation, stale, missing domain PASS");
    return 0;
}
