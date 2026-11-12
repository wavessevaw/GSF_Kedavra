# Application submit observation, not hardware queue depth

This is an opt-in diagnostic on the 0.1.7 scheduling policy. It changes neither
`gfg_frame_clock_pair`, generation admission arithmetic, shaders, TE ABI, nor
the game's fences/submissions. Enable with `GFG_NATIVE_QUEUE_OBSERVE=1` and the
existing telemetry setting on an asynchronous same-device bridge. It is off by
default; this PR is not a new Deck release or a request for another Deck run.
The Decky diagnostic profile `normal-queue-observe` enables only this observer
and records it in `LAUNCH.json`; the launcher filters inherited GFG_NATIVE
variables, so prefixing its launch command is not a supported way to enable it.

## What the 0.1.7 result establishes

Reducing optional dispatch from about 9.6/s to 0.49/s without reducing the
post-60s calibrated start delay weakens GSF self-backlog as the sole dominant
cause. It does not prove that the remainder is exclusively game rendering, or
that every queue has drained between samples. Game commands, synchronization
dependencies, driver scheduling/batching and presentation remain candidates.
The measured ~41 ms is submit-return to GPU **start**; the additional ~8 ms LMC
span is outside that interval and cannot be included in an explanation of it.

The event-based software experiment shows where that particular wait lands in
that driver. It is not proof that lavapipe cannot ever show after-submit work.
Heavy device commands would be a separate controlled experiment, not a reason
to relabel the successful input-readiness recovery test as a Deck reproduction.

## Collection and lifetime

Application `QueueSubmit`, core/KHR `QueueSubmit2`, and `QueueBindSparse` count
successful API calls, not command buffers, frames, batches or GPU hardware
entries. GSF worker entrypoints are distinct and never increment these counts.
Each logical queue has its own submitted/completion watermark. At the first
actual queue submit inside a synchronous TE generation attempt, the observer
polls existing application fences with `vkGetFenceStatus` under the existing
queue host lock. It adds no GPU waits, fences, commands, threads or semaphore
dependencies. JSONL is written later, outside that queue lock.

Successful application fence-status calls and wait-all/single-fence waits also
advance completion evidence. The application's original wait and timeout are
forwarded unchanged, with no queue lock held across that wait. Captured tokens
are checked for lifetime/sequence changes after returning; wait-any over multiple
fences is not falsely attributed to every fence. This avoids an ever-growing
counter when the game itself observes completion and immediately resets fences.

The same lock protects opt-in application fence reset, destruction, and POSIX
fence import/export wrappers. Those operations forget tokens conservatively;
old handles are never polled after destruction or reused as an old completion.
A later successful submit fence can confirm earlier commands on its logical
queue. An existing successful queue/device idle also retires known commands.
There are at most 16 queues and 128 fence tokens. Overflow, forgotten evidence
and query errors are explicit. No background thread polls game-owned objects.

`queue_observation` joins pairs and GPU observations by pid, bridge generation
and TE sequence. `sample_ns` is recorded under the queue lock immediately before
the first TE queue submit; `query_cpu_ns` exposes the observer's CPU cost.
`te_submit_calls` reports whether an attempt used multiple queue calls; its
snapshot describes the first one, not an invented aggregate queue position.

| Field | Meaning |
| --- | --- |
| `app_submits_total` | Successful application submission API calls on this logical queue |
| `confirmed_complete_through` | Latest sequence whose preceding commands were observed complete |
| `app_submits_unretired_upper_bound` | Submitted minus confirmed-complete; includes potentially finished but unobserved calls |
| `tracked_fences_not_ready` | Known application fences observed unsignaled; not a number of queued GPU commands |
| `unfenced_unretired` | Calls without a fence not yet covered by a later confirmed completion |
| `tracking_lost`, `query_errors`, `available` | Coverage/availability; unknown data must not become a zero-depth claim |
| `other_queue_unretired_upper_bound` | Application evidence on other logical queues; not cross-queue execution order |
| `hardware_queue_depth`, `pure_queue_wait_ns` | Always null: these quantities are not measured |

Positive counts support coexistence of not-yet-confirmed application work with
TE submission; they do not prove that work blocked TE. Zero on one logical
queue cannot exclude cross-queue dependencies, shared hardware scheduling,
present synchronization, or driver work. Vulkan logical queues need not map to
distinct hardware queues. Fence observations are host observations, not GPU end
timestamps. Calibrating the TE start timestamp does not supply the missing GPU
end timestamp of the last game command, so their exact GPU time gap remains
unmeasured. Do not automatically select a separate queue from this counter.

Normative references:
[queues](https://docs.vulkan.org/guide/latest/queues.html),
[fence status](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceStatus.html),
[submit synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html),
[fence import](https://docs.vulkan.org/refpages/latest/refpages/source/vkImportFenceFdKHR.html).

## Recovery outcomes

The layer reports `admission_decision` and `generation_candidate_mask` per pair.
These observe the existing decision; there is no new COOLDOWN state machine.
The software report separately counts observed block-to-admit exits, subsequent
admit-to-block reentries, dispatched work and accepted intermediate WSI handoffs.
Unplanned slots, history refresh and source pressure are separate reasons.

| Status | Observation after injected dependency removal |
| --- | --- |
| `blocked_after_release` | Still budget/deadline blocked; no observed readmission |
| `admitted_no_dispatch` | Generation admitted but no output submitted |
| `dispatched_no_wsi` | GPU work submitted but no accepted intermediate in recovery window |
| `unstable_readmission` | At least two observed reentries after admission; inspect reasons before calling this correct or erroneous |
| `recovered` | Readmission, dispatch and accepted intermediate in recovery window |
| `insufficient_evidence` | No complete explicit decision evidence; includes older 0.1.7 logs |

The test additionally requires a viable pre-delay baseline, zero accepted
intermediates while delayed, and at least three in the final recovery window.
Admission exits alone are never a successful recovery or a working multiplier.
`generation_started` is a cumulative first-start marker, not a restart counter.
New queue diagnostics are tested both enabled and disabled, in both layer orders.

## Proton and refresh-source audit

See [the pinned DXVK/VKD3D/Gamescope audit](research/GSF_PROTON_PRESENT_AUDIT_20261019.md).
In particular, ordinary VKD3D Submit2 batches may have no fence and use timeline
signals instead. A fence-only observer then has incomplete completion coverage;
its growing upper bound must not be called measured hardware backlog.

Each bridge now records its declared frontend hint and source timestamp semantics.
Pair records distinguish an actual successful refresh query from a missing/error
query, retained value, explicit override or initial fallback. A successful 60 Hz
response is preserved; the user's game may legitimately be configured for 60 Hz.
No automatic 90 Hz substitution or presentation-policy change is part of this audit.


A discovered calibration defect is fixed separately from refresh selection:
GPU start before CPU submit-return is a valid asynchronous overlap, not inherently
inconsistent timestamps. The signed delta is retained as
`gpu_start_minus_submit_return_ns`; admission uses `max(0, delta)` in the existing
`submit_to_gpu_start_ns` field. Precision/staleness/plausibility gates remain.
The pinned audit includes the original failed CI artifact and a 15-sample replay.
This does not explain away the positive 34–41 ms Deck observations.
