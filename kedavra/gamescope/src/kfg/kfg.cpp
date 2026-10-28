// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation inside Gamescope. See kfg.hpp for the data flow.

#include "kfg.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "convar.h"
#include "log.hpp"
#include "rendervulkan.hpp"

#include "kfg_plan.h"
#include "kfg_post.h"
#include "kfg_smaa_tex.h"
#include "kfg_auto.h"
#include "kfg_sched.h"

uint64_t get_time_in_nanos();

namespace kfg
{
	static LogScope kfg_log( "kfg" );

	gamescope::ConVar<int> cv_kfg_multiplier{ "kfg_multiplier", 0, "KEDAVRA frame generation: 0 = off, 1 = auto (x2 to x3 from the game's frame rate; to x3.75 with kfg_auto_floor), 2 = x2, 3 = x3." };
	gamescope::ConVar<int> cv_kfg_base_fps{ "kfg_base_fps", 0, "KEDAVRA base frame cap: the game is limited to this many frames per second before generation fills the refresh (any value from 20 up; 0 = from kfg_multiplier). Needs kfg_multiplier != 0." };

	// String convars are set by gamescopectl on the Wayland server thread while the
	// compositor thread reads them; assigning a std::string under a concurrent reader is
	// a use-after-free (stopping a trace crashed the session on the Deck). The callback
	// copies each new value under a lock and the compositor reads only that copy.
	static std::mutex g_CvarMutex;
	static std::string g_sTracePath, g_sStatusPath, g_sHudPath = "/dev/shm/kedavra-hud.raw", g_sPost;

	static std::string CvarCopy( const std::string &s )
	{
		std::scoped_lock lock( g_CvarMutex );
		return s;
	}

	static auto CopyTo( std::string &dst )
	{
		return [&dst]( gamescope::ConVar<std::string> &cv )
		{
			std::scoped_lock lock( g_CvarMutex );
			dst = cv.Get();
		};
	}

	gamescope::ConVar<std::string> cv_kfg_trace{ "kfg_trace", "", "CSV trace of every KMS flip while frame generation is on (kedavra/tools/kfg_pacing.py). Empty = off.", CopyTo( g_sTracePath ) };
	gamescope::ConVar<std::string> cv_kfg_status{ "kfg_status", "", "JSON status file, rewritten once per second. Empty = $XDG_RUNTIME_DIR/kedavra-status.json.", CopyTo( g_sStatusPath ) };
	gamescope::ConVar<bool> cv_kfg_debug_marker{ "kfg_debug_marker", false, "Mark generated frames with a corner square, for camera checks." };
	gamescope::ConVar<int> cv_kfg_budget_us{ "kfg_budget_us", 4000, "GPU time of KEDAVRA passes per composite above which a frame counts as over budget." };
	gamescope::ConVar<std::string> cv_kfg_post{ "kfg_post", "", "KEDAVRA filters on the frame each vblank shows, after generation (kfg_post.h), e.g. \"vibrance,cas=0.5,smaa\". Empty = off.", CopyTo( g_sPost ) };
	// A controller sets kfg_multiplier, kfg_base_fps and kfg_post, then kfg_policy to a new
	// revision; the status file echoes the revision with the values in force, so the
	// controller knows its whole policy arrived.
	gamescope::ConVar<int> cv_kfg_auto_floor{ "kfg_auto_floor", 0, "Lowest base cap Auto may use, in fps: 0 = refresh / 3 (x3, at least 30); down to 24 or refresh / 4, whichever is higher (90 Hz: 24, x3.75; 60 Hz: 24, x2.5; 120 Hz: 30, x4)." };
	gamescope::ConVar<int> cv_kfg_analysis_pixels{ "kfg_analysis_pixels", 1536 * 960, "Pixel count KEDAVRA's motion analysis works at: larger games (1080p, 4K) are analysed scaled down to it and only the final warp runs at their own size. 0 = always the game's own size." };
	gamescope::ConVar<bool> cv_kfg_hold_vrr{ "kfg_hold_vrr", true, "With generation on (Auto or a fixed mode), keep the display at its fixed refresh rate: generated frames need a vblank grid, which adaptive sync (VRR) does not give." };
	gamescope::ConVar<bool> cv_kfg_hdr{ "kfg_hdr", true, "Generate and filter HDR games too (HDR10 PQ and scRGB, in float frames). false = HDR games pass through untouched." };
	gamescope::ConVar<int> cv_kfg_policy{ "kfg_policy", 0, "Policy revision of the controller that set the kfg convars, echoed in the status file." };

	// Frames kept on the GPU. The scheduler may show content up to max_delay vblanks
	// behind the newest latch, which at x3 is up to five source frames.
	static constexpr uint32_t k_uRing = 6;
	static constexpr uint32_t k_uTimingSlots = 16;
	static constexpr uint32_t k_uTimingQueries = 3;  // start, after generation, after filters
	static constexpr uint32_t k_uAnalysisSlots = 8;  // analysis submissions timed in flight (start, end each)

	struct Frame_t
	{
		uint64_t ulID = 0;
		uint64_t ulPrevID = 0;      // the frame the pair analysis of this one used; 0 = none
		bool bValid = false;
		gamescope::OwningRc<CVulkanTexture> pColor;
		gamescope::OwningRc<CVulkanTexture> pLuma[ KFG_LEVELS + 1 ];
		gamescope::OwningRc<CVulkanTexture> pFlow[ KFG_LEVELS + 1 ];  // pair (ulPrevID, ulID)
		gamescope::OwningRc<CVulkanTexture> pStats;
		gamescope::OwningRc<CVulkanTexture> pPairSelect;  // the pair's selection field, for all its phases
	};

	struct TraceRow_t
	{
		int64_t nVBlank;
		uint64_t ulFlipNs;
		kfg_decision decision;
		bool bBypass;          // the composite did not apply the decision
		uint64_t ulRefreshNs;
		uint32_t uMultiplier;
	};

	struct State_t
	{
		kfg_sched sched;
		bool bInit = false;
		uint32_t uMultiplier = 1;

		// Vblank grid: index = round((target - origin) / period) + base.
		uint64_t ulGridOrigin = 0;
		uint64_t ulGridPeriod = 0;
		int64_t nGridBase = 0;
		int64_t nLastVBlank = -1;

		uint64_t ulLastCommitID = 0;
		uint32_t uLastAppID = 0;
		uint32_t uWidth = 0, uHeight = 0;
		int nTransfer = KFG_TRANSFER_SDR;  // of the ring and outputs (enum kfg_transfer)

		Frame_t frames[ k_uRing ];
		uint32_t uNextFrame = 0;
		gamescope::OwningRc<CVulkanTexture> pSelect[ 2 ];  // selection field and its repair ping-pong
		gamescope::OwningRc<CVulkanTexture> pOutput[ 2 ];
		uint32_t uNextOutput = 0;
		gamescope::Rc<CVulkanTexture> pLastShown;  // what REPEAT shows again

		// Paint in progress.
		bool bPending = false;
		// Motion analysis of the newest source, submitted after this paint's composite.
		// It goes in one submission when it takes well under a refresh (GSF 1.4.2: ~2.5 ms on
		// the Deck at 90 Hz), so the pair is usable on the next vblank; a slower one is split
		// over two vblanks (k_uAnalysisSplit) so it never delays the next composite.
		kfg_plan analysisPlan = {};
		bool bAnalysisQueued = false;
		bool bAnalysisOnePass = true;
		double flAnalysisUsEma = 0;  // recent analysis time per pair, both parts
		uint32_t uAnalysisNext = 0;  // first pass not yet submitted
		// Multiplier picked from the game's own frame rate (kfg_multiplier 1).
		kfg_auto autoc = {};
		bool bAutoInit = false;
		int nAutoFloor = 0;       // kfg_auto_floor the Auto controller was built with
		bool bApplied = false;
		bool bNewSource = false;
		uint64_t ulPendingID = 0;
		int64_t nPendingVBlank = 0;
		kfg_decision pending{};

		// Counters (status file).
		uint64_t ulPaints = 0, ulApplied = 0, ulBypass = 0, ulRingMiss = 0, ulUnsupported = 0;
		uint64_t ulOverBudget = 0, ulSources = 0, ulLastSourceNs = 0;
		bool bSteamUI = false;  // the Steam client is the base plane (its menus, not the game)
		bool bVRRHeld = false;  // adaptive sync was asked for and is held off for generation
		uint32_t uAnalysisPixels = 0;  // kfg_analysis_pixels the images were made for
		uint32_t uPaintAppID = 0;  // app id of the base plane at the last paint (diagnostics)
		int nLayer0Format = 0, nLayer0Colorspace = 0;  // last layer 0 that generation could not take
		const char *pszLastProblem = "none";
		double flGpuUsLast = 0, flGpuUsMax = 0, flGpuUsSum = 0;
		uint64_t ulGpuSamples = 0;
		uint64_t ulLastStatusNs = 0;

		// GPU timestamps.
		VkQueryPool queryPool = VK_NULL_HANDLE;
		bool bQueryUnsupported = false;
		double flPeriodNs = 1.0;
		uint64_t ulValidMask = ~0ull;
		uint32_t uNextQuery = 0;
		std::deque<uint32_t> pendingQueries;
		// Analysis submissions (not in gpu_us): their own pool, {slot, part 2} in flight.
		VkQueryPool analysisPool = VK_NULL_HANDLE;
		uint32_t uNextAnalysisQuery = 0;
		std::deque<std::pair<uint32_t, uint8_t>> pendingAnalysis;  // slot, 0 part 1, 1 part 2, 2 whole pair
		double flAnalysisUsPair = 0, flAnalysisUsLast = 0, flAnalysisUsSum = 0;
		uint64_t ulAnalysisPairs = 0;

		// Post-generation filters (kfg_post).
		kfg_post_cfg post = {};
		std::string sPostSpec, sPostError;
		bool bPostPending = false, bPostApplied = false;
		bool bShownFiltered = false;  // this composite shows a ring frame or an output: filtered already
		gamescope::OwningRc<CVulkanTexture> pPost[ 2 ];
		gamescope::OwningRc<CVulkanTexture> pSmaaEdges, pSmaaWeights;
		gamescope::OwningRc<CVulkanTexture> pSmaaArea, pSmaaSearch;  // constant, uploaded once
		bool bSmaaUploaded = false;
		uint32_t uPostWidth = 0, uPostHeight = 0;
		int nPostTransfer = KFG_TRANSFER_SDR;  // of the post images
		bool bPostLimited = false;  // HDR frame: colour looks left out (last filtered frame)
		uint64_t ulPostFrames = 0, ulPostSkipped = 0;
		double flPostUsLast = 0, flPostUsSum = 0;
		uint64_t ulPostSamples = 0;
	};

	static State_t g_State;
	static int RefreshHz();
	static int BaseCapFPS( int nRefreshHz );
	static int ManualBaseFPS( int nRefreshHz );

	// Presented decisions waiting for their page flip (steamcompmgr -> KMS thread)
	// and flipped rows waiting to be written (KMS thread -> steamcompmgr).
	static std::mutex g_TraceMutex;
	static std::deque<TraceRow_t> g_InFlight;
	static std::vector<TraceRow_t> g_Flipped;
	// The trace file belongs to its own writer thread (TraceWriterMain): opening, writing,
	// flushing and closing it never run on the compositor or the KMS thread, so a slow disk
	// cannot hold a composite, and stopping a trace does no work in either of them.
	static std::condition_variable g_TraceWake;
	static std::atomic<bool> g_bTraceOn{ false };  // a trace file is open: record rows
	static std::string g_sTraceOpen;                // its path (under g_TraceMutex), for status
	// Every KMS page flip of the output (KMS thread), for the displayed frame rate.
	static std::atomic<uint64_t> g_ulFlips{ 0 };

	// ---------------------------------------------------------------- on-screen HUD
	//
	// The Decky plugin renders the HUD (rings with the displayed and the game's frame
	// rate) into a BGRA bitmap and publishes it atomically. Gamescope shows it as one
	// more layer of every composited frame, so it counts what reaches the screen,
	// generated frames included. File: 8 x u32 header {magic 'KFGH', version 1, width,
	// height, corner 0-3 (TL, TR, BL, BR) or 4 (centre: a message panel), margin px, sequence, 0},
	// then w*h*4 bytes.

	gamescope::ConVar<std::string> cv_kfg_hud{ "kfg_hud", "/dev/shm/kedavra-hud.raw", "KEDAVRA HUD bitmap written by the Decky plugin; empty = no HUD.", CopyTo( g_sHudPath ) };

	struct Hud_t
	{
		gamescope::OwningRc<CVulkanTexture> pTex;
		std::vector<uint8_t> pixels;
		uint32_t uWidth = 0, uHeight = 0, uCorner = 0, uMargin = 12;
		uint32_t uOutputHeight = 0;  // of the last composite (a TV can be 2160 with a 1080p game)
		uint64_t ulLastPollNs = 0;
		int64_t nMtimeNs = -1;
		bool bVisible = false;
		bool bUpload = false;  // pixels changed, copy them in the next composite
	};
	static Hud_t g_Hud;
	static constexpr uint32_t k_uAnalysisSplit = KFG_LEVELS + 3;  // reduce x4, motion level 4, global, motion level 3
	static constexpr uint32_t k_uHudMagic = 0x4847464b;  // "KFGH"
	static constexpr uint32_t k_uHudMaxBytes = 1024u * 1024u;
	static constexpr int k_nHudZpos = 4;  // with the cursor, above the game and overlays

	static void HudPoll( uint64_t ulNow )
	{
		Hud_t &h = g_Hud;
		if ( ulNow - h.ulLastPollNs < 200'000'000ull )
			return;
		h.ulLastPollNs = ulNow;
		const std::string sPath = CvarCopy( g_sHudPath );
		struct stat st;
		if ( sPath.empty() || stat( sPath.c_str(), &st ) != 0 )
		{
			h.bVisible = false;
			return;
		}
		const int64_t nMtime = int64_t( st.st_mtim.tv_sec ) * 1'000'000'000ll + st.st_mtim.tv_nsec;
		struct timespec now;
		clock_gettime( CLOCK_REALTIME, &now );
		const int64_t nNow = int64_t( now.tv_sec ) * 1'000'000'000ll + now.tv_nsec;
		if ( nNow - nMtime > 5'000'000'000ll )
		{
			h.bVisible = false;  // the plugin stopped refreshing it
			return;
		}
		if ( nMtime == h.nMtimeNs )
			return;
		h.nMtimeNs = nMtime;

		FILE *f = fopen( sPath.c_str(), "rb" );
		if ( !f )
			return;
		uint32_t hdr[ 8 ] = {};
		bool bOk = fread( hdr, sizeof( hdr ), 1, f ) == 1 && hdr[ 0 ] == k_uHudMagic && hdr[ 1 ] == 1;
		const uint64_t ulBytes = uint64_t( hdr[ 2 ] ) * hdr[ 3 ] * 4u;
		if ( bOk && ( hdr[ 2 ] == 0 || hdr[ 3 ] == 0 ) )
		{
			fclose( f );
			h.bVisible = false;
			return;
		}
		bOk = bOk && ulBytes <= k_uHudMaxBytes && hdr[ 2 ] <= 2048 && hdr[ 3 ] <= 2048;
		std::vector<uint8_t> px;
		if ( bOk )
		{
			px.resize( ulBytes );
			bOk = fread( px.data(), 1, ulBytes, f ) == ulBytes;
		}
		fclose( f );
		if ( !bOk )
			return;
		if ( !h.pTex || h.uWidth != hdr[ 2 ] || h.uHeight != hdr[ 3 ] )
		{
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;
			flags.bTransferDst = true;
			gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
			if ( !pTex->BInit( hdr[ 2 ], hdr[ 3 ], 1u, DRM_FORMAT_ARGB8888, flags ) )
			{
				h.bVisible = false;
				return;
			}
			h.pTex = std::move( pTex );
			h.uWidth = hdr[ 2 ];
			h.uHeight = hdr[ 3 ];
		}
		h.pixels = std::move( px );
		h.uCorner = hdr[ 4 ] == 4u ? 4u : hdr[ 4 ] & 3u;
		h.uMargin = std::min( hdr[ 5 ], 200u );
		h.bUpload = true;
		h.bVisible = true;
	}

	static void HudPushLayer( FrameInfo_t *pFrameInfo )
	{
		Hud_t &h = g_Hud;
		if ( !pFrameInfo->layers.count() )
			return;
		// Placed in output pixels, from the size of the composite target.
		const FrameInfo_t::Layer_t &base = pFrameInfo->layers.get( 0 );
		const float flOutW = base.tex ? float( base.tex->width() ) / base.scale.x : 1280.0f;
		const float flOutH = base.tex ? float( base.tex->height() ) / base.scale.y : 800.0f;
		h.uOutputHeight = uint32_t( std::lround( flOutH ) );  // the HUD's size follows the screen
		if ( !h.bVisible || !h.pTex || pFrameInfo->layers.count() >= k_nMaxLayers )
			return;
		const bool bCentre = h.uCorner == 4u;
		const float flX = bCentre ? std::floor( ( flOutW - float( h.uWidth ) ) / 2.0f )
		                : ( h.uCorner & 1u ) ? flOutW - float( h.uWidth + h.uMargin ) : float( h.uMargin );
		const float flY = bCentre ? std::floor( ( flOutH - float( h.uHeight ) ) / 2.0f )
		                : ( h.uCorner & 2u ) ? flOutH - float( h.uHeight + h.uMargin ) : float( h.uMargin );

		FrameInfo_t::Layer_t *layer = pFrameInfo->layers.push();
		if ( !layer )
			return;
		layer->tex = h.pTex.get();
		layer->zpos = k_nHudZpos;
		layer->opacity = 1.0f;
		layer->scale = vec2_t{ 1.0f, 1.0f };
		layer->offset = vec2_t{ -flX, -flY };
		layer->filter = GamescopeUpscaleFilter::NEAREST;
		layer->blackBorder = false;
		// On an HDR output the HUD is SDR content: with colour management it is mapped to SDR
		// brightness like Steam's overlays (a scanout plane without it would show sRGB values
		// as PQ). SDR keeps the behaviour the HUD always had.
		layer->applyColorMgmt = ColorspaceIsHDR( base.colorspace ) && base.applyColorMgmt;
		layer->ctm = nullptr;
		layer->hdr_metadata_blob = nullptr;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
		layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_COVERAGE;
	}

	static void HudRecordUpload( CVulkanCmdBuffer *pCmd )
	{
		Hud_t &h = g_Hud;
		if ( !h.bUpload || !h.pTex || h.pixels.size() != size_t( h.uWidth ) * h.uHeight * 4u )
			return;
		h.bUpload = false;
		auto [ pDst, uOffset ] = g_device.uploadBufferData( uint32_t( h.pixels.size() ) );
		memcpy( pDst, h.pixels.data(), h.pixels.size() );
		pCmd->copyBufferToImage( g_device.uploadBuffer(), uOffset, 0, h.pTex.get() );
	}

	static ShaderType ShaderFor( kfg_shader eShader )
	{
		switch ( eShader )
		{
			case KFG_SHADER_REDUCE: return SHADER_TYPE_KFG_REDUCE;
			case KFG_SHADER_ME:     return SHADER_TYPE_KFG_ME;
			case KFG_SHADER_STATS:  return SHADER_TYPE_KFG_STATS;
			case KFG_SHADER_SELECT: return SHADER_TYPE_KFG_SELECT;
			case KFG_SHADER_POST:        return SHADER_TYPE_KFG_POST;
			case KFG_SHADER_SMAA_EDGE:   return SHADER_TYPE_KFG_SMAA_EDGE;
			case KFG_SHADER_SMAA_WEIGHT: return SHADER_TYPE_KFG_SMAA_WEIGHT;
			case KFG_SHADER_SMAA_BLEND:  return SHADER_TYPE_KFG_SMAA_BLEND;
			case KFG_SHADER_WARP:
			default:                return SHADER_TYPE_KFG_WARP;
		}
	}

	static uint32_t DrmFormatFor( kfg_format eFormat )
	{
		switch ( eFormat )
		{
			case KFG_FORMAT_R8:      return DRM_FORMAT_R8;
			case KFG_FORMAT_RGBA16F: return DRM_FORMAT_ABGR16161616F;
			case KFG_FORMAT_RGBA8:
			default:                 return DRM_FORMAT_ABGR8888;
		}
	}

	static gamescope::OwningRc<CVulkanTexture> CreateImage( uint32_t uWidth, uint32_t uHeight, kfg_format eFormat )
	{
		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bStorage = true;
		gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
		if ( !pTex->BInit( uWidth, uHeight, 1u, DrmFormatFor( eFormat ), flags ) )
			return nullptr;
		return pTex;
	}

	// Transfer function of a game layer generation and filters can take, or -1. HDR layers
	// keep their encoded values in float frames (kfg_color_format); the composite decodes
	// generated frames with the layer's own colorspace, exactly like the game's frames.
	static int SourceTransfer( const FrameInfo_t::Layer_t &layer )
	{
		if ( !layer.tex || layer.isYcbcr() )
			return -1;
		const VkFormat eFormat = layer.tex->format();
		if ( layer.colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ )
		{
			if ( !cv_kfg_hdr )
				return -1;
			const bool bOk = eFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || eFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
				eFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
			return bOk ? KFG_TRANSFER_PQ : -1;
		}
		if ( layer.colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB )
			return cv_kfg_hdr && eFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? KFG_TRANSFER_SCRGB : -1;
		switch ( eFormat )
		{
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
			// Many games (DXVK, vkd3d-proton, native Vulkan) present 10-bit SDR swapchains. The
			// raw view samples them like the 8-bit ones; the ring keeps 8 bits per channel.
			case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
				return KFG_TRANSFER_SDR;
			default:
				return -1;
		}
	}

	static bool SupportedSource( const FrameInfo_t::Layer_t &layer )
	{
		return SourceTransfer( layer ) >= 0;
	}

	static void ReleaseImages( State_t &s )
	{
		for ( Frame_t &f : s.frames )
			f = Frame_t{};
		s.pSelect[ 0 ] = s.pSelect[ 1 ] = nullptr;
		s.pOutput[ 0 ] = s.pOutput[ 1 ] = nullptr;
		s.pLastShown = nullptr;
		s.bAnalysisQueued = false;
		s.uWidth = s.uHeight = 0;
	}

	static bool EnsureImages( State_t &s, uint32_t uWidth, uint32_t uHeight )
	{
		const uint32_t uAnalysisPixels = uint32_t( std::max( 0, cv_kfg_analysis_pixels.Get() ) );
		if ( s.uWidth == uWidth && s.uHeight == uHeight && s.pOutput[ 0 ] && s.uAnalysisPixels == uAnalysisPixels )
			return true;
		ReleaseImages( s );
		// The plans read the same value, so it only changes together with the images.
		kfg_analysis_pixels = uAnalysisPixels;
		s.uAnalysisPixels = uAnalysisPixels;
		uint32_t asz[ 2 ];
		kfg_analysis_size( uWidth, uHeight, asz );  // luma, flow and fields: analysis size
		for ( Frame_t &f : s.frames )
		{
			f.pColor = CreateImage( uWidth, uHeight, kfg_color_format( s.nTransfer ) );
			if ( !f.pColor )
				return false;
			for ( uint32_t l = 1; l <= KFG_LEVELS; l++ )
			{
				uint32_t sz[ 2 ];
				kfg_level_size( asz[ 0 ], asz[ 1 ], l, sz );
				f.pLuma[ l ] = CreateImage( sz[ 0 ], sz[ 1 ], KFG_FORMAT_R8 );
				if ( l >= 2 )
					f.pFlow[ l ] = CreateImage( sz[ 0 ], sz[ 1 ], KFG_FORMAT_RGBA16F );
				if ( !f.pLuma[ l ] || ( l >= 2 && !f.pFlow[ l ] ) )
					return false;
			}
			f.pStats = CreateImage( 4, 1, KFG_FORMAT_RGBA16F );  // cut test, camera motion
			if ( !f.pStats )
				return false;
		}
		uint32_t bs[ 2 ];
		kfg_select_size( asz[ 0 ], asz[ 1 ], bs );
		for ( Frame_t &f : s.frames )
		{
			f.pPairSelect = CreateImage( bs[ 0 ], bs[ 1 ], KFG_FORMAT_RGBA16F );
			if ( !f.pPairSelect )
				return false;
		}
		s.pSelect[ 0 ] = CreateImage( bs[ 0 ], bs[ 1 ], KFG_FORMAT_RGBA16F );
		s.pSelect[ 1 ] = CreateImage( bs[ 0 ], bs[ 1 ], KFG_FORMAT_RGBA16F );
		s.pOutput[ 0 ] = CreateImage( uWidth, uHeight, kfg_color_format( s.nTransfer ) );
		s.pOutput[ 1 ] = CreateImage( uWidth, uHeight, kfg_color_format( s.nTransfer ) );
		if ( !s.pSelect[ 0 ] || !s.pSelect[ 1 ] || !s.pOutput[ 0 ] || !s.pOutput[ 1 ] )
			return false;
		s.uWidth = uWidth;
		s.uHeight = uHeight;
		kfg_log.infof( "ring of %u frames at %ux%u allocated (%s)", k_uRing, uWidth, uHeight, kfg_transfer_names[ s.nTransfer ] );
		return true;
	}

	static Frame_t *FindFrame( State_t &s, uint64_t ulID )
	{
		for ( Frame_t &f : s.frames )
		{
			if ( f.bValid && f.ulID == ulID )
				return &f;
		}
		return nullptr;
	}

	static uint32_t FrameIndex( State_t &s, const Frame_t *pFrame )
	{
		return uint32_t( pFrame - s.frames );
	}

	static void ResetHistory( State_t &s, const char *pszWhy )
	{
		for ( Frame_t &f : s.frames )
		{
			f.bValid = false;
			f.ulPrevID = 0;
		}
		s.pLastShown = nullptr;
		if ( s.bInit )
			kfg_sched_reset_history( &s.sched );
		s.ulLastCommitID = 0;
		kfg_auto_reset_source( &s.autoc );
		kfg_log.debugf( "history reset: %s", pszWhy );
	}

	// ---------------------------------------------------------------- GPU timing

	static bool TimingInit( State_t &s )
	{
		if ( s.queryPool != VK_NULL_HANDLE || s.bQueryUnsupported )
			return s.queryPool != VK_NULL_HANDLE;

		uint32_t uFamilyCount = 0;
		g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, nullptr );
		std::vector<VkQueueFamilyProperties> families( uFamilyCount );
		g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, families.data() );
		if ( g_device.queueFamily() >= uFamilyCount || families[ g_device.queueFamily() ].timestampValidBits == 0 )
		{
			s.bQueryUnsupported = true;
			return false;
		}
		const uint32_t uBits = families[ g_device.queueFamily() ].timestampValidBits;
		s.ulValidMask = uBits >= 64 ? ~0ull : ( ( 1ull << uBits ) - 1 );

		VkPhysicalDeviceProperties props;
		g_device.vk.GetPhysicalDeviceProperties( g_device.physDev(), &props );
		s.flPeriodNs = props.limits.timestampPeriod;

		VkQueryPoolCreateInfo createInfo =
		{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = k_uTimingSlots * k_uTimingQueries,
		};
		if ( g_device.vk.CreateQueryPool( g_device.device(), &createInfo, nullptr, &s.queryPool ) != VK_SUCCESS )
		{
			s.bQueryUnsupported = true;
			return false;
		}
		createInfo.queryCount = k_uAnalysisSlots * 2;
		if ( g_device.vk.CreateQueryPool( g_device.device(), &createInfo, nullptr, &s.analysisPool ) != VK_SUCCESS )
			s.analysisPool = VK_NULL_HANDLE;  // composite timing still works
		return true;
	}

	// GPU time of one pair's analysis: part 1 plus part 2.
	static void AnalysisTimingDrain( State_t &s )
	{
		while ( !s.pendingAnalysis.empty() )
		{
			const auto [ uSlot, uPart ] = s.pendingAnalysis.front();
			uint64_t ulResults[ 4 ] = {};
			VkResult res = g_device.vk.GetQueryPoolResults( g_device.device(), s.analysisPool, uSlot * 2, 2,
				sizeof( ulResults ), ulResults, 2 * sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
			if ( res == VK_NOT_READY || !ulResults[ 1 ] || !ulResults[ 3 ] )
				return;
			s.pendingAnalysis.pop_front();
			const double flUs = double( ( ulResults[ 2 ] - ulResults[ 0 ] ) & s.ulValidMask ) * s.flPeriodNs / 1000.0;
			if ( uPart == 0 )
			{
				s.flAnalysisUsPair = flUs;  // a part 1 without its part 2 (history reset) is dropped here
				continue;
			}
			s.flAnalysisUsLast = ( uPart == 2 ? 0.0 : s.flAnalysisUsPair ) + flUs;
			s.flAnalysisUsSum += s.flAnalysisUsLast;
			s.flAnalysisUsEma = s.flAnalysisUsEma > 0 ? s.flAnalysisUsEma + ( s.flAnalysisUsLast - s.flAnalysisUsEma ) / 16.0
			                                          : s.flAnalysisUsLast;
			s.ulAnalysisPairs++;
		}
	}

	static void TimingDrain( State_t &s )
	{
		while ( !s.pendingQueries.empty() )
		{
			const uint32_t uSlot = s.pendingQueries.front();
			// {value, available} for start, after generation, after filters.
			uint64_t ulResults[ 2 * k_uTimingQueries ] = {};
			VkResult res = g_device.vk.GetQueryPoolResults( g_device.device(), s.queryPool, uSlot * k_uTimingQueries, k_uTimingQueries,
				sizeof( ulResults ), ulResults, 2 * sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
			if ( res == VK_NOT_READY || !ulResults[ 1 ] || !ulResults[ 3 ] || !ulResults[ 5 ] )
				return;
			s.pendingQueries.pop_front();
			const uint64_t ulTicks = ( ulResults[ 4 ] - ulResults[ 0 ] ) & s.ulValidMask;
			const double flUs = double( ulTicks ) * s.flPeriodNs / 1000.0;
			const double flPostUs = double( ( ulResults[ 4 ] - ulResults[ 2 ] ) & s.ulValidMask ) * s.flPeriodNs / 1000.0;
			if ( flPostUs > 0.5 )
			{
				s.flPostUsLast = flPostUs;
				s.flPostUsSum += flPostUs;
				s.ulPostSamples++;
			}
			s.flGpuUsLast = flUs;
			s.flGpuUsMax = std::max( s.flGpuUsMax, flUs );
			s.flGpuUsSum += flUs;
			s.ulGpuSamples++;
			if ( flUs > double( cv_kfg_budget_us.Get() ) )
				s.ulOverBudget++;
		}
	}

	// ---------------------------------------------------------------- recording

	struct Resolve_t
	{
		State_t *s;
		gamescope::Rc<CVulkanTexture> pSourceColor;  // KFG_ROLE_COLOR of the frame being copied
		uint32_t uOutput;
		CVulkanTexture *pPostIn = nullptr;  // KFG_ROLE_POST_IN: what this vblank shows before filters
	};

	static CVulkanTexture *ResolveRef( Resolve_t &r, kfg_image_ref ref )
	{
		State_t &s = *r.s;
		switch ( ref.role )
		{
			case KFG_ROLE_COLOR:  return s.frames[ ref.ring ].pColor.get();
			case KFG_ROLE_LUMA:   return s.frames[ ref.ring ].pLuma[ ref.level ].get();
			case KFG_ROLE_FLOW:   return s.frames[ ref.ring ].pFlow[ ref.level ].get();
			case KFG_ROLE_STATS:  return s.frames[ ref.ring ].pStats.get();
			case KFG_ROLE_SELECT: return s.pSelect[ ref.ring & 1 ].get();
			case KFG_ROLE_PAIR_SELECT: return s.frames[ ref.ring ].pPairSelect.get();
			case KFG_ROLE_OUTPUT: return s.pOutput[ r.uOutput ].get();
			case KFG_ROLE_POST_IN:      return r.pPostIn;
			case KFG_ROLE_POST:         return s.pPost[ ref.ring & 1 ].get();
			case KFG_ROLE_SMAA_EDGES:   return s.pSmaaEdges.get();
			case KFG_ROLE_SMAA_WEIGHTS: return s.pSmaaWeights.get();
			case KFG_ROLE_SMAA_AREA:    return s.pSmaaArea.get();
			case KFG_ROLE_SMAA_SEARCH:  return s.pSmaaSearch.get();
			default:              return nullptr;
		}
	}

	static void RecordPlan( CVulkanCmdBuffer *pCmd, const kfg_plan &plan, Resolve_t &r, uint32_t uFirst = 0, uint32_t uEnd = ~0u )
	{
		for ( uint32_t i = uFirst; i < std::min( plan.n, uEnd ); i++ )
		{
			const kfg_pass &pass = plan.pass[ i ];
			pCmd->bindPipeline( g_device.pipeline( ShaderFor( pass.shader ) ) );
			for ( uint32_t slot = 0; slot < KFG_MAX_SLOTS; slot++ )
			{
				CVulkanTexture *pTex = slot < pass.nslots ? ResolveRef( r, pass.slots[ slot ] ) : nullptr;
				pCmd->bindTexture( slot, pTex );
				pCmd->setTextureSrgb( slot, true );  // raw values, see RecordCopy
				pCmd->setSamplerNearest( slot, ( pass.nearest_mask >> slot ) & 1u );
				pCmd->setSamplerUnnormalized( slot, false );
			}
			pCmd->bindTarget( ResolveRef( r, pass.dst ) );
			UploadParams( pCmd, pass.params );
			pCmd->dispatch( pass.groups[ 0 ], pass.groups[ 1 ] );
		}
	}

	static void RecordCopy( CVulkanCmdBuffer *pCmd, gamescope::Rc<CVulkanTexture> pSrc, CVulkanTexture *pDst )
	{
		kfg_params p = {};
		p.dst_size[ 0 ] = int32_t( pDst->width() );
		p.dst_size[ 1 ] = int32_t( pDst->height() );
		p.src_size[ 0 ] = p.dst_size[ 0 ];
		p.src_size[ 1 ] = p.dst_size[ 1 ];
		pCmd->bindPipeline( g_device.pipeline( SHADER_TYPE_KFG_COPY ) );
		pCmd->bindTexture( 0, std::move( pSrc ) );
		// Gamescope's naming is inverted: setTextureSrgb( true ) binds the UNORM view
		// ("srgbView", the raw encoded values) and false the _SRGB view, which decodes
		// to linear. The ring must keep the raw values: the composite reuses the game
		// layer's colorspace and decodes them itself. With false the ring held linear
		// values that got decoded twice (dark, high contrast, flicker against
		// passthrough frames; seen on the Deck).
		pCmd->setTextureSrgb( 0, true );
		pCmd->setSamplerNearest( 0, true );
		pCmd->setSamplerUnnormalized( 0, false );
		for ( uint32_t slot = 1; slot < KFG_MAX_SLOTS; slot++ )
			pCmd->bindTexture( slot, nullptr );
		pCmd->bindTarget( pDst );
		UploadParams( pCmd, p );
		pCmd->dispatch( kfg_div_up( pDst->width(), 8 ), kfg_div_up( pDst->height(), 8 ) );
	}

	// Writes from earlier composites (ring frames, analysis) become visible to this one.
	// Gamescope's per-command-buffer tracking only emits barriers for images written
	// in the same command buffer.
	static void RecordHistoryBarrier( CVulkanCmdBuffer *pCmd )
	{
		VkMemoryBarrier barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		};
		g_device.vk.CmdPipelineBarrier( pCmd->rawBuffer(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &barrier, 0, nullptr, 0, nullptr );
	}

	// ---------------------------------------------------------------- post filters

	// Parses kfg_post when it changed. A bad string turns the filters off and is reported.
	static void PostUpdateConfig( State_t &s )
	{
		const std::string sSpec = CvarCopy( g_sPost );
		if ( sSpec == s.sPostSpec )
			return;
		s.sPostSpec = sSpec;
		char szErr[ 128 ];
		if ( kfg_post_parse( sSpec.c_str(), &s.post, szErr, sizeof( szErr ) ) != 0 )
		{
			s.sPostError = szErr;
			kfg_log.errorf( "kfg_post \"%s\": %s; filters off", sSpec.c_str(), szErr );
		}
		else
		{
			s.sPostError.clear();
			kfg_log.infof( "filters: %s", sSpec.empty() ? "off" : sSpec.c_str() );
		}
		if ( !kfg_post_active( &s.post ) )
		{
			// Off: free the images (the SMAA tables are small and stay).
			s.pPost[ 0 ] = s.pPost[ 1 ] = nullptr;
			s.pSmaaEdges = s.pSmaaWeights = nullptr;
			s.uPostWidth = s.uPostHeight = 0;
		}
	}

	static bool EnsurePostImages( State_t &s, uint32_t uWidth, uint32_t uHeight, bool bSmaa, int nTransfer )
	{
		if ( s.uPostWidth != uWidth || s.uPostHeight != uHeight || !s.pPost[ 0 ] || s.nPostTransfer != nTransfer )
		{
			s.pSmaaEdges = s.pSmaaWeights = nullptr;
			s.nPostTransfer = nTransfer;
			s.pPost[ 0 ] = CreateImage( uWidth, uHeight, kfg_color_format( nTransfer ) );
			s.pPost[ 1 ] = CreateImage( uWidth, uHeight, kfg_color_format( nTransfer ) );
			if ( !s.pPost[ 0 ] || !s.pPost[ 1 ] )
			{
				s.pPost[ 0 ] = s.pPost[ 1 ] = nullptr;
				s.uPostWidth = s.uPostHeight = 0;
				return false;
			}
			s.uPostWidth = uWidth;
			s.uPostHeight = uHeight;
		}
		if ( !bSmaa )
			return true;
		if ( !s.pSmaaEdges )
		{
			s.pSmaaEdges = CreateImage( uWidth, uHeight, KFG_FORMAT_RGBA8 );
			s.pSmaaWeights = CreateImage( uWidth, uHeight, KFG_FORMAT_RGBA8 );
		}
		if ( !s.pSmaaArea )
		{
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;
			flags.bTransferDst = true;
			gamescope::OwningRc<CVulkanTexture> pArea = new CVulkanTexture();
			gamescope::OwningRc<CVulkanTexture> pSearch = new CVulkanTexture();
			if ( pArea->BInit( KFG_SMAA_AREA_W, KFG_SMAA_AREA_H, 1u, DRM_FORMAT_ABGR8888, flags ) &&
				pSearch->BInit( KFG_SMAA_SEARCH_W, KFG_SMAA_SEARCH_H, 1u, DRM_FORMAT_R8, flags ) )
			{
				s.pSmaaArea = std::move( pArea );
				s.pSmaaSearch = std::move( pSearch );
				s.bSmaaUploaded = false;
			}
		}
		return s.pSmaaEdges && s.pSmaaWeights && s.pSmaaArea && s.pSmaaSearch;
	}

	static void RecordSmaaUpload( CVulkanCmdBuffer *pCmd, State_t &s )
	{
		if ( s.bSmaaUploaded )
			return;
		const uint32_t uAreaBytes = KFG_SMAA_AREA_W * KFG_SMAA_AREA_H * 4u;
		auto [ pArea, uAreaOffset ] = g_device.uploadBufferData( uAreaBytes );
		kfg_smaa_area_rgba8( static_cast<uint8_t *>( pArea ) );
		pCmd->copyBufferToImage( g_device.uploadBuffer(), uAreaOffset, 0, s.pSmaaArea.get() );
		const uint32_t uSearchBytes = KFG_SMAA_SEARCH_W * KFG_SMAA_SEARCH_H;
		auto [ pSearch, uSearchOffset ] = g_device.uploadBufferData( uSearchBytes );
		memcpy( pSearch, kfg_smaa_search_r8(), uSearchBytes );
		pCmd->copyBufferToImage( g_device.uploadBuffer(), uSearchOffset, 0, s.pSmaaSearch.get() );
		s.bSmaaUploaded = true;
	}

	// Filters layer 0 (the real or generated frame of this vblank) into the post images and
	// shows the result instead. Runs after generation, so the HUD and overlays (other
	// layers) and the motion analysis (ring frames) never see filtered pixels.
	// What of the filters runs on a frame of this transfer. The filters work on code values in
	// 0..1. HDR10 PQ code values are perceptual like sRGB ones, so sharpening, antialiasing and
	// the finish effects (vignette, grain, chromatic aberration) work on them; the colour
	// looks are SDR grades and would wreck HDR colour and brightness, so they are left out.
	// scRGB is linear with values above 1.0: no filter is made for it, shown unfiltered.
	static kfg_post_cfg PostFor( const kfg_post_cfg &cfg, int nTransfer )
	{
		kfg_post_cfg c = cfg;
		if ( nTransfer == KFG_TRANSFER_SCRGB )
			c = kfg_post_cfg{};
		else if ( nTransfer == KFG_TRANSFER_PQ )
			c.grade = 0;
		return c;
	}

	// Filters on layer 0. With pInto the result is copied there (a ring frame: the game's own
	// frame filtered once, before generation) instead of replacing layer 0; false when the
	// filters are off for this frame.
	static bool RecordPost( CVulkanCmdBuffer *pCmd, State_t &s, FrameInfo_t::Layer_t &layer0, CVulkanTexture *pInto = nullptr )
	{
		const uint32_t uWidth = layer0.tex->width(), uHeight = layer0.tex->height();
		const int nTransfer = SourceTransfer( layer0 );
		const kfg_post_cfg post = PostFor( s.post, nTransfer );
		s.bPostLimited = nTransfer != KFG_TRANSFER_SDR && post.grade != s.post.grade;
		if ( !kfg_post_active( &post ) )
		{
			s.ulPostSkipped++;
			return false;
		}
		if ( !EnsurePostImages( s, uWidth, uHeight, post.aa == KFG_AA_SMAA, nTransfer ) )
		{
			s.ulPostSkipped++;
			s.pszLastProblem = "post_allocation_failed";
			return false;
		}
		if ( post.aa == KFG_AA_SMAA )
			RecordSmaaUpload( pCmd, s );
		RecordHistoryBarrier( pCmd );  // the post images were last written by the previous composite
		// Film grain moves with time, like SweetFX's Timer * 0.0022337 (Timer in ms).
		const double flMs = double( get_time_in_nanos() ) / 1e6;
		const float flPhase = float( std::fmod( flMs * 0.0022337, 1.0 ) );
		kfg_plan plan;
		const int nRing = kfg_plan_post( &plan, uWidth, uHeight, &post, flPhase );
		if ( nRing < 0 )
			return false;
		Resolve_t r = { &s, nullptr, s.uNextOutput, layer0.tex.get() };
		RecordPlan( pCmd, plan, r );
		if ( pInto )
			RecordCopy( pCmd, s.pPost[ nRing ].get(), pInto );
		else
			layer0.tex = s.pPost[ nRing ].get();
		s.ulPostFrames++;
		return true;
	}

	// JSON string content: quotes, backslashes and control characters become '?'.
	static std::string JsonSafe( const std::string &str )
	{
		std::string out = str;
		for ( char &c : out )
		{
			if ( c == '"' || c == '\\' || static_cast<unsigned char>( c ) < 0x20 )
				c = '?';
		}
		return out;
	}

	// ---------------------------------------------------------------- trace and status

	static void WriteTraceRow( FILE *pFile, const TraceRow_t &row )
	{
		const kfg_decision &d = row.decision;
		const char *pszKind = row.bBypass ? "repeat" : kfg_show_name( d.show );
		const char *pszReason = row.bBypass ? "bypass" : kfg_reason_name( d.reason );
		fprintf( pFile, "%" PRId64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRId64 ",%" PRId64 ",%s,%s,%" PRIu64 ",%u,0,%d,%.3f\n",
			row.nVBlank, row.ulFlipNs, pszKind, d.src_a, d.src_b, d.phase_num, d.phase_den, d.content, d.delay,
			pszReason, kfg_state_name( d.state ), row.ulRefreshNs, row.uMultiplier, d.skipped ? 1 : 0,
			double( d.pos ) / KFG_SCHED_SUB );
	}

	static void TraceWriterMain()
	{
		FILE *pFile = nullptr;
		std::string sOpen;
		std::vector<TraceRow_t> rows;
		for ( ;; )
		{
			{
				std::unique_lock lock( g_TraceMutex );
				g_TraceWake.wait_for( lock, std::chrono::milliseconds( 100 ) );
				rows.clear();
				rows.swap( g_Flipped );
			}

			const std::string sPath = CvarCopy( g_sTracePath );
			if ( sPath != sOpen )
			{
				// Stop recording first, so no rows for the old file arrive after it is closed.
				g_bTraceOn.store( false, std::memory_order_relaxed );
				if ( pFile )
				{
					for ( const TraceRow_t &row : rows )
						WriteTraceRow( pFile, row );
					rows.clear();
					fclose( pFile );
					kfg_log.infof( "trace: closed %s", sOpen.c_str() );
				}
				pFile = nullptr;
				sOpen = sPath;
				if ( !sPath.empty() )
				{
					pFile = fopen( sPath.c_str(), "w" );
					if ( pFile )
					{
						fprintf( pFile, "vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped,pos\n" );
						kfg_log.infof( "trace: %s", sPath.c_str() );
					}
					else
						kfg_log.errorf( "trace: cannot open %s", sPath.c_str() );
				}
				{
					std::scoped_lock lock( g_TraceMutex );
					g_sTraceOpen = pFile ? sOpen : std::string();
					g_InFlight.clear();
					g_Flipped.clear();
				}
				g_bTraceOn.store( pFile != nullptr, std::memory_order_relaxed );
				continue;
			}
			if ( !pFile || rows.empty() )
				continue;
			for ( const TraceRow_t &row : rows )
				WriteTraceRow( pFile, row );
			fflush( pFile );
		}
	}

	static void StartTraceWriter()
	{
		static std::once_flag s_once;
		std::call_once( s_once, []
		{
			std::thread( TraceWriterMain ).detach();
		} );
	}

	static void WriteStatus( State_t &s, uint64_t ulNow )
	{
		if ( ulNow - s.ulLastStatusNs < 1'000'000'000ull )
			return;
		s.ulLastStatusNs = ulNow;

		std::string sPath = CvarCopy( g_sStatusPath );
		if ( sPath.empty() )
		{
			const char *pszRuntime = getenv( "XDG_RUNTIME_DIR" );
			if ( !pszRuntime )
				return;
			sPath = std::string( pszRuntime ) + "/kedavra-status.json";
		}
		const std::string sTmp = sPath + ".tmp";
		FILE *f = fopen( sTmp.c_str(), "w" );
		if ( !f )
			return;
		const kfg_sched &q = s.sched;
		std::string sTrace;
		{
			std::scoped_lock lock( g_TraceMutex );
			sTrace = g_sTraceOpen;
		}
		uint32_t asz[ 2 ] = { 0, 0 };
		if ( s.uWidth )
			kfg_analysis_size( s.uWidth, s.uHeight, asz );
		fprintf( f,
			"{\"engine\":\"kedavra-gamescope\",\"version\":1,\"time_ns\":%" PRIu64 ",\"multiplier\":%u,\"state\":\"%s\","
			"\"refresh_ns\":%" PRIu64 ",\"width\":%u,\"height\":%u,"
			"\"paints\":%" PRIu64 ",\"applied\":%" PRIu64 ",\"bypass\":%" PRIu64 ",\"ring_miss\":%" PRIu64 ",\"unsupported\":%" PRIu64 ","
			"\"sources\":%" PRIu64 ",\"real\":%" PRIu64 ",\"gen\":%" PRIu64 ",\"repeat\":%" PRIu64 ",\"skip\":%" PRIu64 ",\"resync\":%" PRIu64 ",\"degrade\":%" PRIu64 ",\"catchup\":%" PRIu64 ","
			"\"gpu_us_last\":%.1f,\"gpu_us_max\":%.1f,\"gpu_us_avg\":%.1f,\"over_budget\":%" PRIu64 ",\"timestamps\":%s,"
			"\"flips\":%" PRIu64 ",\"base_fps\":%d,\"unsupported_format\":%d,\"unsupported_colorspace\":%d,\"last_source_ns\":%" PRIu64 ",\"auto\":%s,\"auto_switches\":%" PRIu64 ",\"source_late\":%" PRIu64 ",\"cap_fps\":%d,\"steam_ui\":%s,\"app_id\":%u,"
			"\"last_problem\":\"%s\",\"trace\":\"%s\","
			"\"policy\":%d,\"req_multiplier\":%d,\"req_base_fps\":%d,\"req_auto_floor\":%d,\"auto_floor\":%u,"
			"\"post\":\"%s\",\"post_active\":%s,\"post_error\":\"%s\",\"post_frames\":%" PRIu64 ",\"post_skipped\":%" PRIu64 ","
			"\"gpu_post_us_last\":%.1f,\"gpu_post_us_avg\":%.1f,\"gpu_analysis_us_last\":%.1f,\"gpu_analysis_us_avg\":%.1f,\"analysis_pairs\":%" PRIu64 ",\"analysis_one_pass\":%s,"
			"\"hdr_enabled\":%s,\"transfer\":\"%s\",\"post_hdr_limited\":%s,"
			"\"analysis_width\":%u,\"analysis_height\":%u,\"vrr_held\":%s,\"output_height\":%u}\n",
			ulNow, s.uMultiplier, s.bInit ? kfg_state_name( q.state ) : "off",
			s.ulGridPeriod, s.uWidth, s.uHeight,
			s.ulPaints, s.ulApplied, s.ulBypass, s.ulRingMiss, s.ulUnsupported,
			s.ulSources, q.n_real, q.n_gen, q.n_repeat, q.n_skip, q.n_resync, q.n_degrade, q.n_catchup,
			s.flGpuUsLast, s.flGpuUsMax, s.ulGpuSamples ? s.flGpuUsSum / double( s.ulGpuSamples ) : 0.0, s.ulOverBudget,
			s.queryPool != VK_NULL_HANDLE ? "true" : "false",
			g_ulFlips.load( std::memory_order_relaxed ), ManualBaseFPS( RefreshHz() ), s.nLayer0Format, s.nLayer0Colorspace, s.ulLastSourceNs,
			s.bAutoInit ? "true" : "false", s.autoc.n_switch, s.autoc.lates,
			s.ulGridPeriod ? SourceCapFPS( int( std::llround( 1e9 / double( s.ulGridPeriod ) ) ) ) : 0,
			s.bSteamUI ? "true" : "false", s.uPaintAppID, s.pszLastProblem, JsonSafe( sTrace ).c_str(),
			cv_kfg_policy.Get(), cv_kfg_multiplier.Get(), cv_kfg_base_fps.Get(), cv_kfg_auto_floor.Get(),
			s.bAutoInit ? s.autoc.levels[0] : 0u,
			JsonSafe( s.sPostSpec ).c_str(), kfg_post_active( &s.post ) ? "true" : "false", JsonSafe( s.sPostError ).c_str(),
			s.ulPostFrames, s.ulPostSkipped,
			s.flPostUsLast, s.ulPostSamples ? s.flPostUsSum / double( s.ulPostSamples ) : 0.0,
			s.flAnalysisUsLast, s.ulAnalysisPairs ? s.flAnalysisUsSum / double( s.ulAnalysisPairs ) : 0.0, s.ulAnalysisPairs, s.bAnalysisOnePass ? "true" : "false",
			cv_kfg_hdr ? "true" : "false", kfg_transfer_names[ s.nTransfer ], s.bPostLimited ? "true" : "false",
			asz[ 0 ], asz[ 1 ], s.bVRRHeld ? "true" : "false", g_Hud.uOutputHeight );
		fclose( f );
		rename( sTmp.c_str(), sPath.c_str() );
	}

	// ---------------------------------------------------------------- entry points

	static int RefreshHz()
	{
		return g_State.ulGridPeriod ? int( std::llround( 1e9 / double( g_State.ulGridPeriod ) ) ) : 90;
	}

	// Fixed base cap from kfg_base_fps (Custom), clamped to what generation can fill; 0 = none.
	static int ManualBaseFPS( int nRefreshHz )
	{
		const int nBase = cv_kfg_base_fps.Get();
		if ( cv_kfg_multiplier.Get() == 0 || nBase <= 0 || nRefreshHz <= 0 )
			return 0;
		const int nMin = ( nRefreshHz + int( KFG_MAX_MULTIPLIER ) - 1 ) / int( KFG_MAX_MULTIPLIER );
		return std::clamp( nBase, std::max( 20, nMin ), nRefreshHz );
	}

	// Base cap in force: Custom's, else the one Auto picked from the game's frame rate.
	static int BaseCapFPS( int nRefreshHz )
	{
		if ( const int nBase = ManualBaseFPS( nRefreshHz ) )
			return nBase;
		if ( cv_kfg_multiplier.Get() == 1 && g_State.bAutoInit && nRefreshHz > 0 )
			return std::min( int( g_State.autoc.cap ), nRefreshHz );
		return 0;
	}

	static uint32_t RequestedMultiplier()
	{
		const int n = cv_kfg_multiplier.Get();
		const int nHz = RefreshHz();
		if ( const int nBase = BaseCapFPS( nHz ) )
		{
			// Longest source interval in vblanks: 40 at 90 Hz alternates 2 and 3, so 3. A cap
			// at the refresh rate needs no generation.
			return nBase >= nHz ? 1u : uint32_t( ( nHz + nBase - 1 ) / nBase );
		}
		if ( n == 1 )
			return g_State.bAutoInit ? g_State.autoc.mult : 3u;
		return ( n == 2 || n == 3 ) ? uint32_t( n ) : 1u;
	}

	int SourceCapFPS( int nRefreshHz )
	{
		if ( const int nBase = BaseCapFPS( nRefreshHz ) )
			return nBase < nRefreshHz ? nBase : 0;
		const uint32_t uMult = RequestedMultiplier();
		if ( uMult <= 1 || nRefreshHz <= 0 )
			return 0;
		return nRefreshHz / int( uMult );
	}

	bool WantsEveryVBlank()
	{
		return RequestedMultiplier() > 1 || ( g_State.bInit && g_State.sched.state != KFG_STATE_OFF );
	}

	bool WantsComposite()
	{
		return WantsEveryVBlank() || ( kfg_post_active( &g_State.post ) && !g_State.bSteamUI );
	}

	void OnPaint( FrameInfo_t *pFrameInfo, uint64_t ulBaseCommitID, uint32_t uAppID, uint64_t ulTargetVBlankNs, uint64_t ulRefreshCycleNs )
	{
		State_t &s = g_State;
		const uint64_t ulNow = get_time_in_nanos();
		s.bPending = false;
		s.bApplied = false;
		s.bPostPending = false;
		s.bPostApplied = false;
		PostUpdateConfig( s );

		// Generation needs a fixed vblank grid. Adaptive sync stays off for as long as a
		// generation mode is selected, not per game or menu: each switch is a modeset (a black
		// screen on many TVs). Mode Off gives it back.
		s.bVRRHeld = cv_kfg_hold_vrr && cv_kfg_multiplier.Get() != 0 && pFrameInfo->allowVRR;
		if ( s.bVRRHeld )
			pFrameInfo->allowVRR = false;

		StartTraceWriter();
		WriteStatus( s, ulNow );
		HudPoll( ulNow );
		HudPushLayer( pFrameInfo );

		// The game's own frame rate is counted whether generation is on or not: the HUD and
		// the plugin must show what the game really delivers, not the display's flip rate
		// (the Steam menu alone repaints at the refresh rate).
		s.bNewSource = false;
		// The Steam client is exempt from the frame limiter and repaints at the refresh rate
		// while its own UI is the base plane: those commits are not game frames, and frames
		// generated over them would be wrong. The game's state (app id, Auto) is kept for
		// when it comes back.
		constexpr uint32_t k_uSteamAppID = 769;
		const bool bSteamUI = uAppID == k_uSteamAppID;
		s.uPaintAppID = uAppID;
		if ( bSteamUI != s.bSteamUI )
		{
			s.bSteamUI = bSteamUI;
			if ( bSteamUI )
				ResetHistory( s, "steam ui" );
		}
		if ( bSteamUI )
			return;
		// Filters apply to the game's frames whether generation is on or not.
		if ( kfg_post_active( &s.post ) )
		{
			const int nTransfer = pFrameInfo->layers.count() ? SourceTransfer( pFrameInfo->layers.get( 0 ) ) : -1;
			const kfg_post_cfg post = PostFor( s.post, nTransfer );
			s.bPostPending = nTransfer >= 0 && kfg_post_active( &post );
			if ( !s.bPostPending )
				s.ulPostSkipped++;  // scRGB, kfg_hdr off or an unusual format: shown unfiltered
		}
		const bool bNewCommit = ulBaseCommitID != 0 && ulBaseCommitID != s.ulLastCommitID;
		if ( bNewCommit )
		{
			s.ulLastCommitID = ulBaseCommitID;
			s.ulSources++;
			s.ulLastSourceNs = ulNow;
		}

		const uint32_t uMult = RequestedMultiplier();
		if ( !s.bInit )
		{
			if ( uMult == 1 )
				return;
			kfg_sched_config cfg = kfg_sched_default_config( uMult );
			kfg_sched_init( &s.sched, &cfg );
			s.uMultiplier = uMult;
			s.bInit = true;
		}
		if ( uMult != s.uMultiplier )
		{
			kfg_sched_set_multiplier( &s.sched, uMult );
			s.uMultiplier = uMult;
			kfg_log.infof( "multiplier %u", uMult );
		}
		// The base cap's release pattern (40 at 90 Hz: 2, 2, 2, 3 vblanks); unchanged caps are a no-op.
		kfg_sched_set_cadence( &s.sched, uint32_t( RefreshHz() ), uint32_t( BaseCapFPS( RefreshHz() ) ) );
		if ( uMult == 1 && s.sched.state == KFG_STATE_OFF )
		{
			// Fully off: drop the ring so an idle feature costs no memory.
			if ( s.uWidth )
				ReleaseImages( s );
			return;
		}

		if ( !pFrameInfo->layers.count() || !SupportedSource( pFrameInfo->layers.get( 0 ) ) )
		{
			s.ulUnsupported++;
			s.pszLastProblem = "unsupported_layer0";
			if ( pFrameInfo->layers.count() && pFrameInfo->layers.get( 0 ).tex )
			{
				s.nLayer0Format = int( pFrameInfo->layers.get( 0 ).tex->format() );
				s.nLayer0Colorspace = int( pFrameInfo->layers.get( 0 ).colorspace );
			}
			ResetHistory( s, "unsupported layer 0" );
			return;
		}

		// Vblank index of this paint.
		const uint64_t ulPeriod = ulRefreshCycleNs ? ulRefreshCycleNs : 11'111'111ull;
		if ( !s.ulGridPeriod || ulPeriod != s.ulGridPeriod )
		{
			s.ulGridOrigin = ulTargetVBlankNs;
			s.ulGridPeriod = ulPeriod;
			s.nGridBase = s.nLastVBlank + 1;
			s.bAutoInit = false;
			ResetHistory( s, "refresh change" );
		}
		const double flSteps = ( double( int64_t( ulTargetVBlankNs - s.ulGridOrigin ) ) ) / double( s.ulGridPeriod );
		int64_t nVBlank = s.nGridBase + int64_t( std::llround( flSteps ) );
		if ( nVBlank <= s.nLastVBlank )
			nVBlank = s.nLastVBlank + 1;  // two paints for one vblank: keep indices strictly increasing
		s.nLastVBlank = nVBlank;

		if ( cv_kfg_multiplier.Get() == 1 && !ManualBaseFPS( RefreshHz() ) )
		{
			const int nFloor = std::max( 0, cv_kfg_auto_floor.Get() );
			if ( s.bAutoInit && nFloor != s.nAutoFloor )
				s.bAutoInit = false;  // a new floor rebuilds the cap steps
			if ( !s.bAutoInit )
			{
				kfg_auto_init_floor( &s.autoc, uint32_t( std::llround( 1e9 / double( ulPeriod ) ) ), uint32_t( nFloor ) );
				s.nAutoFloor = nFloor;
				s.bAutoInit = true;
			}
			const uint32_t uWas = s.autoc.cap;
			kfg_auto_step( &s.autoc, nVBlank );
			if ( s.autoc.cap != uWas )
				kfg_log.infof( "auto: cap %u fps (median %.1f)", s.autoc.cap, kfg_auto_median( &s.autoc ) );
		}
		else
			s.bAutoInit = false;

		if ( uAppID != s.uLastAppID )
		{
			s.uLastAppID = uAppID;
			s.bAutoInit = false;  // a new game starts Auto from scratch, without the old one's history
			ResetHistory( s, "focus change" );
		}

		const FrameInfo_t::Layer_t &layer0 = pFrameInfo->layers.get( 0 );
		const int nTransfer = SourceTransfer( layer0 );
		if ( nTransfer != s.nTransfer )
		{
			// SDR <-> HDR (the game or Steam switched HDR): frames of the old encoding must
			// not be blended with the new one, and the ring changes format.
			ResetHistory( s, "transfer change" );
			ReleaseImages( s );
			s.nTransfer = nTransfer;
		}
		if ( layer0.tex->width() != s.uWidth || layer0.tex->height() != s.uHeight )
		{
			ResetHistory( s, "size change" );
			if ( !EnsureImages( s, layer0.tex->width(), layer0.tex->height() ) )
			{
				s.pszLastProblem = "allocation_failed";
				ReleaseImages( s );
				return;
			}
		}

		s.bNewSource = bNewCommit;
		if ( s.bNewSource )
		{
			kfg_sched_on_latch( &s.sched, ulBaseCommitID, nVBlank );
			if ( s.bAutoInit )
				kfg_auto_on_latch( &s.autoc, nVBlank );
			// Analysis runs in its own submission after this paint's composite (see
			// SubmitAnalysis): in one pass while it takes under a third of the refresh, so the
			// pair is usable on the next vblank, else in two parts, usable two vblanks later.
			const double flPeriodUs = 1e6 / double( std::max( 1, RefreshHz() ) );
			s.bAnalysisOnePass = s.flAnalysisUsEma < flPeriodUs / 3.0 && s.flAnalysisUsLast < flPeriodUs / 2.0;
			kfg_sched_on_me( &s.sched, ulBaseCommitID, nVBlank + ( s.bAnalysisOnePass ? 1 : 2 ), true, false );
		}

		s.pending = kfg_sched_decide( &s.sched, nVBlank );
		s.ulPendingID = ulBaseCommitID;
		s.nPendingVBlank = nVBlank;
		s.bPending = true;
		s.ulPaints++;

		// The composite must run: generated content never matches a held commit.
		pFrameInfo->bRepeatFrame = false;
	}

	// Generation: picks or generates the frame of this vblank and puts it in layer 0.
	static void RecordGenerate( CVulkanCmdBuffer *pCmd, State_t &s, FrameInfo_t::Layer_t &layer0 )
	{
		RecordHistoryBarrier( pCmd );

		Resolve_t r = { &s, layer0.tex, s.uNextOutput };
		kfg_tuning tune = kfg_default_tuning();
		tune.transfer = s.nTransfer;
		kfg_plan plan;

		if ( s.bNewSource )
		{
			Frame_t *pPrev = nullptr;
			// The newest valid frame is the previous source (ids increase).
			for ( Frame_t &f : s.frames )
			{
				if ( f.bValid && f.ulID < s.ulPendingID && ( !pPrev || f.ulID > pPrev->ulID ) )
					pPrev = &f;
			}
			Frame_t &cur = s.frames[ s.uNextFrame ];
			s.uNextFrame = ( s.uNextFrame + 1 ) % k_uRing;
			if ( pPrev == &cur )
				pPrev = nullptr;

			// A source faster than the cap can arrive before the previous analysis is fully
			// submitted: its rest goes into this composite, ahead of the new frame's.
			if ( s.bAnalysisQueued )
			{
				RecordPlan( pCmd, s.analysisPlan, r, s.uAnalysisNext );
				s.bAnalysisQueued = false;
			}
			// With filters on, the game's frame is filtered once on its way into the ring: real
			// and generated frames then come out filtered, and the filters cost once per game
			// frame instead of once per displayed frame (x3.75: a quarter as often).
			if ( !s.bPostPending || !RecordPost( pCmd, s, layer0, cur.pColor.get() ) )
				RecordCopy( pCmd, layer0.tex, cur.pColor.get() );
			cur.ulID = s.ulPendingID;
			cur.bValid = true;
			cur.ulPrevID = pPrev ? pPrev->ulID : 0;
			const uint32_t fb = FrameIndex( s, &cur );
			const uint32_t fa = pPrev ? FrameIndex( s, pPrev ) : fb;
			// Not recorded here: ~5 ms of analysis inside the composite made the flip miss
			// its vblank (on the Deck a third of the vblanks had no flip).
			// The previous pair (ending at A) seeds the coarse search when A had its own pair.
			const int prevPair = pPrev && pPrev->ulPrevID ? int( fa ) : -1;
			kfg_plan_analyze( &s.analysisPlan, s.uWidth, s.uHeight, fa, fb, fb, pPrev ? 1 : 0, prevPair, &tune );
			s.bAnalysisQueued = true;
			s.uAnalysisNext = 0;
		}

		const kfg_decision &d = s.pending;
		gamescope::Rc<CVulkanTexture> pShow;
		switch ( d.show )
		{
			case KFG_SHOW_REAL:
			{
				Frame_t *pA = FindFrame( s, d.src_a );
				if ( pA )
					pShow = pA->pColor.get();
				else
					s.ulRingMiss++, s.pszLastProblem = "ring_miss_real";
				break;
			}
			case KFG_SHOW_GEN:
			{
				Frame_t *pA = FindFrame( s, d.src_a );
				Frame_t *pB = FindFrame( s, d.src_b );
				if ( pA && pB && pB->ulPrevID == pA->ulID )
				{
					kfg_plan_generate( &plan, s.uWidth, s.uHeight, FrameIndex( s, pA ), FrameIndex( s, pB ), FrameIndex( s, pB ),
						float( d.phase_num ) / float( d.phase_den ),
						kfg_transfer_flags( s.nTransfer ) | ( cv_kfg_debug_marker ? KFG_FLAG_DEBUG_MARKER : 0 ) );
					RecordPlan( pCmd, plan, r );
					pShow = s.pOutput[ s.uNextOutput ].get();
					s.uNextOutput ^= 1u;
				}
				else
				{
					s.ulRingMiss++;
					s.pszLastProblem = "ring_miss_pair";
					if ( pB )
						pShow = pB->pColor.get();
				}
				break;
			}
			case KFG_SHOW_REPEAT:
				pShow = s.pLastShown;
				break;
			case KFG_SHOW_NONE:
			default:
				break;
		}

		if ( d.state == KFG_STATE_OFF || !pShow )
		{
			// Passthrough: the newest commit as Gamescope would show it.
			s.pLastShown = nullptr;
			return;
		}
		layer0.tex = pShow;
		s.pLastShown = pShow;
		s.ulApplied++;
		s.bShownFiltered = s.bPostPending;  // ring frames and outputs made from them are filtered
	}

	void RecordComposite( CVulkanCmdBuffer *pCmd, FrameInfo_t *pFrameInfo )
	{
		State_t &s = g_State;
		HudRecordUpload( pCmd );
		if ( !pFrameInfo->layers.count() )
			return;
		const bool bGenerate = s.bPending && !s.bApplied;
		const bool bPost = s.bPostPending && !s.bPostApplied;
		if ( !bGenerate && !bPost )
			return;
		s.bApplied |= bGenerate;
		s.bPostApplied |= bPost;

		FrameInfo_t::Layer_t &layer0 = pFrameInfo->layers.get( 0 );
		if ( !SupportedSource( layer0 ) )
			return;
		const bool bSizeOk = layer0.tex->width() == s.uWidth && layer0.tex->height() == s.uHeight &&
			SourceTransfer( layer0 ) == s.nTransfer;
		if ( !bPost && !bSizeOk )
			return;

		const bool bTiming = TimingInit( s );
		if ( bTiming )
			TimingDrain( s );
		std::optional<uint32_t> oQuery;
		if ( bTiming && s.pendingQueries.size() < k_uTimingSlots )
		{
			oQuery = s.uNextQuery;
			s.uNextQuery = ( s.uNextQuery + 1 ) % k_uTimingSlots;
			g_device.vk.CmdResetQueryPool( pCmd->rawBuffer(), s.queryPool, *oQuery * k_uTimingQueries, k_uTimingQueries );
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queryPool, *oQuery * k_uTimingQueries );
		}

		s.bShownFiltered = false;
		if ( bGenerate && bSizeOk )
			RecordGenerate( pCmd, s, layer0 );
		if ( oQuery )
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queryPool, *oQuery * k_uTimingQueries + 1 );
		if ( bPost && !s.bShownFiltered )
			RecordPost( pCmd, s, layer0 );
		if ( oQuery )
		{
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queryPool, *oQuery * k_uTimingQueries + 2 );
			s.pendingQueries.push_back( *oQuery );
		}
	}

	// The composite for this vblank is submitted and waited for; analysis goes behind it
	// on the same queue, so it is complete before the next composite reads its results.
	static void SubmitAnalysis( State_t &s )
	{
		if ( !s.bAnalysisQueued )
			return;
		if ( !s.uWidth )
		{
			s.bAnalysisQueued = false;
			return;
		}
		// One pass, or split: part 1 the luma pyramid and the two coarse motion levels (~1.6 ms
		// on the Deck), part 2 one vblank later the finest motion level and the cut statistics.
		const uint32_t uStart = s.uAnalysisNext;
		const uint32_t uEnd = s.uAnalysisNext == 0 && !s.bAnalysisOnePass ? std::min( s.analysisPlan.n, k_uAnalysisSplit ) : s.analysisPlan.n;
		auto pCmd = g_device.commandBuffer();
		std::optional<uint32_t> oQuery;
		if ( s.analysisPool != VK_NULL_HANDLE )
		{
			AnalysisTimingDrain( s );
			if ( s.pendingAnalysis.size() < k_uAnalysisSlots )
			{
				oQuery = s.uNextAnalysisQuery;
				s.uNextAnalysisQuery = ( s.uNextAnalysisQuery + 1 ) % k_uAnalysisSlots;
				g_device.vk.CmdResetQueryPool( pCmd->rawBuffer(), s.analysisPool, *oQuery * 2, 2 );
				g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.analysisPool, *oQuery * 2 );
			}
		}
		RecordHistoryBarrier( pCmd.get() );
		Resolve_t r = { &s, nullptr, s.uNextOutput };
		RecordPlan( pCmd.get(), s.analysisPlan, r, s.uAnalysisNext, uEnd );
		if ( oQuery )
		{
			g_device.vk.CmdWriteTimestamp( pCmd->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.analysisPool, *oQuery * 2 + 1 );
			s.pendingAnalysis.push_back( { *oQuery, uint8_t( uEnd < s.analysisPlan.n ? 0 : uStart == 0 ? 2 : 1 ) } );
		}
		g_device.submit( std::move( pCmd ) );
		s.uAnalysisNext = uEnd;
		if ( uEnd >= s.analysisPlan.n )
			s.bAnalysisQueued = false;
	}

	void OnPresented( bool bPresented )
	{
		State_t &s = g_State;
		SubmitAnalysis( s );
		if ( !s.bPending )
			return;
		s.bPending = false;
		if ( !bPresented )
			return;
		const bool bBypass = !s.bApplied && s.pending.state != KFG_STATE_OFF;
		if ( bBypass )
		{
			s.ulBypass++;
			s.pszLastProblem = "composite_bypassed";
		}
		if ( !g_bTraceOn.load( std::memory_order_relaxed ) )
			return;
		std::scoped_lock lock( g_TraceMutex );
		if ( g_InFlight.size() > 64 )
			g_InFlight.clear();  // flips stopped arriving (suspend, DPMS)
		g_InFlight.push_back( TraceRow_t{ s.nPendingVBlank, 0, s.pending, bBypass, s.ulGridPeriod, s.uMultiplier } );
	}

	void OnPageFlip( uint64_t ulVBlankNs, uint32_t uSequence )
	{
		(void) uSequence;
		g_ulFlips.fetch_add( 1, std::memory_order_relaxed );
		std::scoped_lock lock( g_TraceMutex );
		if ( g_InFlight.empty() )
			return;
		TraceRow_t row = g_InFlight.front();
		g_InFlight.pop_front();
		row.ulFlipNs = ulVBlankNs;
		if ( g_Flipped.size() < 4096 )
			g_Flipped.push_back( row );
	}
}
