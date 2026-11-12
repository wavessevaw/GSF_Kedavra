/* SPDX-License-Identifier: GPL-3.0-or-later
 * Diagnostic only. Caller serializes submissions, fence reset/destruction and
 * snapshots. Counts API calls, never physical hardware queue entries or time.
 */
#ifndef GSF_QUEUE_OBSERVATION_H
#define GSF_QUEUE_OBSERVATION_H
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define GSF_QO_QUEUES 16
#define GSF_QO_FENCES 128
typedef struct {
    VkQueue handle;
    uint64_t submitted, completed, unfenced, completed_unfenced, lost_through;
} gsf_qo_queue;
typedef struct {
    VkFence handle;
    unsigned queue;
    uint64_t sequence, unfenced;
} gsf_qo_fence;
typedef struct {
    gsf_qo_queue queues[GSF_QO_QUEUES];
    gsf_qo_fence fences[GSF_QO_FENCES];
    uint64_t queue_overflow;
} gsf_queue_observer;
typedef struct {
    uint64_t captured_ns, query_cpu_ns, unretired_upper_bound, unfenced_unretired;
    uint64_t submitted, confirmed_complete_through, other_queue_unretired;
    unsigned captures, tracked_not_ready, query_errors, tracking_lost, available;
} gsf_queue_snapshot;
/* A scope pointer is installed on the synchronous TE submit thread. The
 * worker's real queue wrapper fills it under the existing queue host lock. */
typedef void (*gsf_queue_probe_fn)(VkQueue, gsf_queue_snapshot *);
static inline int gsf_qo_find(gsf_queue_observer *o,VkQueue q) {
    for(unsigned i=0;i<GSF_QO_QUEUES;++i)if(o->queues[i].handle==q)return (int)i;
    for(unsigned i=0;i<GSF_QO_QUEUES;++i)if(!o->queues[i].handle){o->queues[i].handle=q;return (int)i;}
    ++o->queue_overflow;return -1;
}
static inline void gsf_qo_forget(gsf_queue_observer *o,VkFence f) {
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle==f && f) {
        gsf_qo_fence *t=&o->fences[i];gsf_qo_queue *q=&o->queues[t->queue];
        if(t->sequence>q->lost_through)q->lost_through=t->sequence;
        memset(t,0,sizeof(*t));
    }
}
static inline void gsf_qo_submit(gsf_queue_observer *o,VkQueue handle,VkFence f,VkResult result) {
    if(result!=VK_SUCCESS)return;
    int k=gsf_qo_find(o,handle);if(k<0)return;
    gsf_qo_queue *q=&o->queues[k];++q->submitted;
    if(!f){++q->unfenced;return;}
    gsf_qo_forget(o,f);
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(!o->fences[i].handle) {
        o->fences[i]=(gsf_qo_fence){f,(unsigned)k,q->submitted,q->unfenced};return;
    }
    q->lost_through=q->submitted;
}
static inline void gsf_qo_idle(gsf_queue_observer *o,VkQueue handle) {
    for(unsigned k=0;k<GSF_QO_QUEUES;++k)if(!handle || o->queues[k].handle==handle) {
        gsf_qo_queue *q=&o->queues[k];q->completed=q->submitted;q->completed_unfenced=q->unfenced;
        for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle && o->fences[i].queue==k)
            memset(&o->fences[i],0,sizeof(o->fences[i]));
    }
}
static inline void gsf_qo_signaled(gsf_queue_observer *o,VkFence handle) {
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle==handle && handle) {
        gsf_qo_fence *t=&o->fences[i];gsf_qo_queue *q=&o->queues[t->queue];
        if(t->sequence>q->completed){q->completed=t->sequence;q->completed_unfenced=t->unfenced;}
    }
}
static inline void gsf_qo_snapshot(gsf_queue_observer *o,VkQueue handle,VkDevice device,
                                  PFN_vkGetFenceStatus status,gsf_queue_snapshot *s) {
    int k=gsf_qo_find(o,handle);if(k<0 || !status)return;
    unsigned char not_ready[GSF_QO_FENCES]={0};
    /* Success on a submit fence confirms preceding commands on THAT queue.
     * NOT_READY is not a count of remaining commands or proof of a stall. */
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle) {
        gsf_qo_fence *t=&o->fences[i];gsf_qo_queue *q=&o->queues[t->queue];
        VkResult r=status(device,t->handle);
        if(r==VK_SUCCESS && t->sequence>q->completed) {
            q->completed=t->sequence;q->completed_unfenced=t->unfenced;
        } else if(r==VK_NOT_READY)not_ready[i]=1;
        else if(r!=VK_SUCCESS && r!=VK_NOT_READY)++s->query_errors;
    }
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle &&
        o->fences[i].sequence<=o->queues[o->fences[i].queue].completed)
        memset(&o->fences[i],0,sizeof(o->fences[i]));
    for(unsigned i=0;i<GSF_QO_FENCES;++i)if(o->fences[i].handle &&
        o->fences[i].queue==(unsigned)k && not_ready[i])++s->tracked_not_ready;
    gsf_qo_queue *q=&o->queues[k];
    s->submitted=q->submitted;s->confirmed_complete_through=q->completed;
    s->unretired_upper_bound=q->submitted-q->completed;
    s->unfenced_unretired=q->unfenced-q->completed_unfenced;
    s->tracking_lost=q->lost_through>q->completed || o->queue_overflow!=0;
    for(unsigned i=0;i<GSF_QO_QUEUES;++i)if(i!=(unsigned)k)
        s->other_queue_unretired+=o->queues[i].submitted-o->queues[i].completed;
    s->available=1;
}
#endif
