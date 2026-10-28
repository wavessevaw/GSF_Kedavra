// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation inside Gamescope.
//
// Gamescope owns the realtime compute queue, the vblank timer and the KMS page
// flips, so frame generation runs here instead of in the game process:
//   * every base-plane commit of the focused window is copied into a ring and
//     analysed (motion between it and the previous commit) in the composite
//     command buffer of the paint that latches it;
//   * while generation is on, steamcompmgr paints and composites on every vblank
//     and the vblank-indexed scheduler (kedavra/sched/kfg_sched.h) decides which
//     real or generated frame that vblank shows;
//   * the page-flip handler stamps every decision with its KMS flip time, which is
//     the trace kedavra/tools/kfg_pacing.py analyses.
//   * filters (kfg_post) run on the frame each vblank shows, after generation, so real and
//     generated frames look the same and the motion analysis never sees them.
// Control: gamescopectl kfg_multiplier 0|1|2|3, kfg_base_fps, kfg_post <filters>,
// kfg_policy <revision>, kfg_trace <csv path>, kfg_status <json path>.
#pragma once

#include <cstdint>
#include <span>

struct FrameInfo_t;
struct kfg_params;
class CVulkanCmdBuffer;

namespace kfg
{
	// steamcompmgr: true while generation needs a composite on every vblank.
	// Frame limit the game gets while generation is on: refresh / multiplier, so the source
	// lands on a fixed vblank cadence without a manual FPS limit. 0 = no KEDAVRA limit.
	int SourceCapFPS( int nRefreshHz );
	bool WantsEveryVBlank();
	// DRM backend: true while every frame must go through the composite (generation, or
	// filters on the game's frames), so it is never scanned out directly.
	bool WantsComposite();

	// steamcompmgr paint_all, before the connector presents frameInfo. Feeds the
	// scheduler with the base plane commit and the vblank this paint targets.
	void OnPaint( FrameInfo_t *pFrameInfo, uint64_t ulBaseCommitID, uint32_t uAppID, uint64_t ulTargetVBlankNs, uint64_t ulRefreshCycleNs );
	// steamcompmgr paint_all, after the present: whether it reached the backend.
	void OnPresented( bool bPresented );

	// vulkan_composite, before the upscalers: records copy, analysis and generation
	// passes into the composite command buffer and substitutes layer 0.
	void RecordComposite( CVulkanCmdBuffer *pCmd, FrameInfo_t *pFrameInfo );

	// KMS thread: page flip completed at ulVBlankNs (kernel timestamp).
	void OnPageFlip( uint64_t ulVBlankNs, uint32_t uSequence );

	// rendervulkan.cpp: uploads pass constants (CVulkanCmdBuffer::uploadConstants lives there).
	void UploadParams( CVulkanCmdBuffer *pCmd, const kfg_params &params );

	// steamcompmgr: Steam's own frame limit (GAMESCOPE_FPS_LIMIT, 0 = none), each loop.
	void SetSteamFPSLimit( int nFPS );
	// steamcompmgr paint_all: the refresh generation wants on the Steam Deck LCD (kfg_refresh:
	// multiplier x base, from the panel's rates), 0 = leave Steam's choice. The OLED, external
	// displays and VRR are never switched.
	int WantedRefreshHz( std::span<const uint32_t> rates, bool bInternal );
}
