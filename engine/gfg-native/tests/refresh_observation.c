/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/refresh_observation.h"

int main(void) {
    const uint64_t hz90=1000000000ull/90,hz60=16666666;
    uint64_t period=hz90;gsf_refresh_observation o;
    gsf_refresh_init(&o,period,0);
    gsf_refresh_begin(&o,1,period,0,0);
    assert(!o.attempted && !strcmp(o.query_state,"unavailable"));
    assert(!strcmp(o.origin,"configured_fallback"));
    /* An apparently sensible value with a failed result is not a success. */
    if(gsf_refresh_record(&o,VK_ERROR_SURFACE_LOST_KHR,hz60,37,100))period=hz60;
    assert(period==hz90 && !o.last_success_ns && !strcmp(o.origin,"configured_fallback"));
    /* A real 60 Hz result is accepted verbatim; no Deck-90 heuristic. */
    gsf_refresh_begin(&o,1,period,0,1);
    if(gsf_refresh_record(&o,VK_SUCCESS,hz60,41,200))period=hz60;
    assert(period==hz60 && o.last_success_ns==200 && o.query_cpu_ns==41);
    assert(!strcmp(o.origin,"successful_wsi_query"));
    /* Missing, rejected and failed later queries preserve the selected period
     * AND its actual origin. They do not become a new fallback or success. */
    const uint64_t invalid[]={0,1000000000ull/240-1,1000000001ull,UINT64_MAX};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        gsf_refresh_begin(&o,1,period,0,1);
        assert(!gsf_refresh_record(&o,VK_SUCCESS,invalid[i],1,300));
        assert(o.last_success_ns==200 && !strcmp(o.origin,"successful_wsi_query"));
        assert(!strcmp(o.query_state,"rejected_range"));
    }
    gsf_refresh_begin(&o,1,period,0,0);
    assert(!o.attempted && o.last_success_ns==200 && !o.raw_ns);
    assert(!strcmp(o.origin,"successful_wsi_query"));
    /* Retain existing getenv-presence semantics, including an invalid override:
     * this observer must not secretly change which queries the layer performs. */
    gsf_refresh_begin(&o,1,period,1,1);
    assert(o.override_present && !o.attempted && !strcmp(o.query_state,"override_present"));
    gsf_refresh_init(&o,hz90,1);
    gsf_refresh_begin(&o,0,hz90,1,1);
    assert(!strcmp(o.origin,"explicit_override") && !strcmp(o.query_state,"inactive"));
    /* Both boundaries of the existing 1..240 Hz acceptance range are valid. */
    assert(gsf_refresh_record(&o,VK_SUCCESS,1000000000ull/240,0,400));
    assert(gsf_refresh_record(&o,VK_SUCCESS,1000000000ull,0,500));
    puts("refresh-observation PASS: real 60 Hz, fallback, retained origin, errors, overrides, bounds");
}
