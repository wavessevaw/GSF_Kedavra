/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/queue_observation.h"
static VkResult states[256];
static unsigned queries;
static VKAPI_ATTR VkResult VKAPI_CALL status(VkDevice d,VkFence f) {
    (void)d;++queries;return states[(uintptr_t)f];
}
static gsf_queue_snapshot sample(gsf_queue_observer *o,VkQueue q) {
    gsf_queue_snapshot s={0};gsf_qo_snapshot(o,q,VK_NULL_HANDLE,status,&s);return s;
}
int main(void) {
    VkQueue a=(VkQueue)(uintptr_t)1,b=(VkQueue)(uintptr_t)2;
    VkFence f=(VkFence)(uintptr_t)1,g=(VkFence)(uintptr_t)2;
    gsf_queue_observer o={0};gsf_queue_snapshot s;
    states[1]=states[2]=VK_NOT_READY;
    gsf_qo_submit(&o,a,f,VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(sample(&o,a).submitted==0);
    gsf_qo_submit(&o,a,f,VK_SUCCESS);s=sample(&o,a);
    assert(s.unretired_upper_bound==1 && s.tracked_not_ready==1);
    gsf_qo_submit(&o,a,VK_NULL_HANDLE,VK_SUCCESS);
    gsf_qo_submit(&o,a,g,VK_SUCCESS);s=sample(&o,a);
    assert(s.unfenced_unretired==1 && s.unretired_upper_bound==3);
    states[2]=VK_SUCCESS;s=sample(&o,a);
    assert(s.confirmed_complete_through==3 && !s.unretired_upper_bound && !s.tracked_not_ready);
    assert(!s.unfenced_unretired); /* later fence covers prior unfenced work */
    gsf_qo_submit(&o,b,f,VK_SUCCESS);s=sample(&o,a);
    assert(!s.unretired_upper_bound && s.other_queue_unretired==1);
    gsf_qo_forget(&o,f);queries=0;s=sample(&o,b);
    assert(!queries && s.tracking_lost && s.unretired_upper_bound==1);
    gsf_qo_submit(&o,b,f,VK_SUCCESS);states[1]=VK_ERROR_DEVICE_LOST;s=sample(&o,b);
    assert(s.query_errors==1 && s.unretired_upper_bound==2);
    states[1]=VK_SUCCESS;s=sample(&o,b);
    assert(!s.tracking_lost && !s.unretired_upper_bound);
    /* Bounded capacity must report lost evidence, never a false empty queue. */
    memset(&o,0,sizeof(o));
    for(unsigned i=1;i<=GSF_QO_FENCES+1;++i){states[i]=VK_NOT_READY;gsf_qo_submit(&o,a,(VkFence)(uintptr_t)i,VK_SUCCESS);}
    s=sample(&o,a);assert(s.tracking_lost && s.unretired_upper_bound==GSF_QO_FENCES+1);
    gsf_qo_idle(&o,a);queries=0;s=sample(&o,a);
    assert(!queries && !s.tracking_lost && !s.unretired_upper_bound);
    for(unsigned i=3;i<3+GSF_QO_QUEUES;++i)gsf_qo_submit(&o,(VkQueue)(uintptr_t)i,f,VK_SUCCESS);
    s=sample(&o,(VkQueue)(uintptr_t)99);assert(!s.available);
    puts("queue-observation PASS: failure, fencing, reuse, loss, overflow, multiple queues, idle");
}
