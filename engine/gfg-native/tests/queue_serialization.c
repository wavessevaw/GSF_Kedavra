/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual dispatch wrappers resolved through GIPA/GDPA, concurrent producers.
 * The fake lower driver rejects overlapping host access to the same queue.
 */
#define _GNU_SOURCE
#define GFG_NATIVE_TEST_QUEUE_OBSERVE
#include "../src/layer.c"
#include <assert.h>
#include <stdatomic.h>

static atomic_int inside, calls;
static void lower_enter(void) {
    assert(atomic_fetch_add(&inside,1)==0);
    struct timespec t={.tv_nsec=100000}; nanosleep(&t,NULL);
    atomic_fetch_add(&calls,1); assert(atomic_fetch_sub(&inside,1)==1);
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_submit(VkQueue q,uint32_t n,const VkSubmitInfo *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_submit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_bind(VkQueue q,uint32_t n,const VkBindSparseInfo *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_present(VkQueue q,const VkPresentInfoKHR *p) {
    (void)q;(void)p;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_idle(VkQueue q) { (void)q;lower_enter();return VK_SUCCESS; }
static void *producer(void *queue) {
    for(unsigned i=0;i<20;++i) {
        assert(native_QueueSubmit(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueSubmit2(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueSubmit2KHR(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueBindSparse(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_serialized_present(queue,NULL)==VK_SUCCESS);
        assert(native_QueueWaitIdle(queue)==VK_SUCCESS);
    }
    return NULL;
}
static unsigned polled;
static native_device *waiting_device;
static VkResult wait_result=VK_SUCCESS;
static VKAPI_ATTR VkResult VKAPI_CALL lower_fence_status(VkDevice d,VkFence f) {
    (void)d;(void)f;++polled;return VK_NOT_READY;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_wait(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
    (void)d;(void)n;(void)f;(void)all;(void)timeout;
    assert(pthread_mutex_trylock(&waiting_device->queue_lock)==0);
    pthread_mutex_unlock(&waiting_device->queue_lock);return wait_result;
}
int main(void) {
    void *key=(void *)(uintptr_t)123;
    VkQueue queue=(VkQueue)&key; VkDevice device=(VkDevice)&key;
    native_device d={.key=key,.handle=device,.submit=lower_submit,.submit2=lower_submit2,
        .submit2_khr=lower_submit2,.bind_sparse=lower_bind,.present=lower_present,.queue_idle=lower_idle};
    pthread_mutex_init(&d.lock,NULL);pthread_mutex_init(&d.queue_lock,NULL);devices=&d;
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit")==native_gdpa(device,"vkQueueSubmit"));
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit2")==native_gdpa(device,"vkQueueSubmit2"));
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit2KHR")==native_gdpa(device,"vkQueueSubmit2KHR"));
    pthread_t threads[3];
    for(unsigned i=0;i<3;++i) assert(pthread_create(&threads[i],NULL,producer,queue)==0);
    for(unsigned i=0;i<3;++i) pthread_join(threads[i],NULL);
    assert(atomic_load(&calls)==360 && !atomic_load(&inside));
    gsf_queue_observer observation={0};d.queue_observer=&observation;d.fence_status=lower_fence_status;
    VkFence f=(VkFence)(uintptr_t)11;
    assert(native_QueueSubmit(queue,0,NULL,f)==VK_SUCCESS);
    assert(native_QueueSubmit2(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(native_QueueSubmit2KHR(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(native_QueueBindSparse(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    gsf_queue_snapshot snapshot={0};native_queue_probe(queue,&snapshot);
    PFN_vkQueueSubmit worker=(PFN_vkQueueSubmit)native_worker_gdpa(device,"vkQueueSubmit");
    PFN_vkQueueSubmit2 worker2=(PFN_vkQueueSubmit2)native_worker_gdpa(device,"vkQueueSubmit2");
    PFN_vkQueueSubmit2KHR worker_khr=(PFN_vkQueueSubmit2KHR)native_worker_gdpa(device,"vkQueueSubmit2KHR");
    assert(worker(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(worker2(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(worker_khr(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    native_queue_probe(queue,NULL);
    assert(snapshot.available && snapshot.captures==3 && snapshot.submitted==4);
    assert(snapshot.unretired_upper_bound==4 && snapshot.tracked_not_ready==1 && polled==1);
    assert(observation.queues[0].submitted==4); /* none of the three GSF submits counted */
    VkFence later=(VkFence)(uintptr_t)12;
    assert(native_QueueSubmit(queue,0,NULL,later)==VK_SUCCESS);
    d.wait_fences=lower_wait;waiting_device=&d;
    wait_result=VK_TIMEOUT;
    assert(native_WaitForFences(device,1,&later,VK_TRUE,0)==VK_TIMEOUT);
    assert(observation.queues[0].completed==0);
    wait_result=VK_SUCCESS;
    VkFence both[2]={f,later};
    assert(native_WaitForFences(device,2,both,VK_FALSE,0)==VK_SUCCESS);
    assert(observation.queues[0].completed==0); /* wait-any does not identify the fence */
    assert(native_WaitForFences(device,1,&later,VK_TRUE,0)==VK_SUCCESS);
    assert(observation.queues[0].completed==5); /* app's wait confirms prior unfenced calls */
    assert(native_worker_QueueWaitIdle(queue)==VK_SUCCESS);
    assert(observation.queues[0].completed==5);devices=NULL;
    pthread_mutex_destroy(&d.queue_lock);pthread_mutex_destroy(&d.lock);
    puts("GFG native: concurrent submit/submit2/KHR/bind/present/idle host serialization + GIPA/GDPA PASS");
    return 0;
}
