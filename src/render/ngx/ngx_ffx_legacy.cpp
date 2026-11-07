// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
/**
* This file is part of Special K.
*
* Special K is free software : you can redistribute it
* and/or modify it under the terms of the GNU General Public License
* as published by The Free Software Foundation, either version 3 of
* the License, or (at your option) any later version.
*
* Special K is distributed in the hope that it will be useful,
*
* But WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Special K.
*
*   If not, see <http://www.gnu.org/licenses/>.
*
**/

//
// Legacy FSR front end: FSR 2.x (ffx_fsr2_api_x64.dll) and the standalone
//   FSR 3.0 upscaler (FidelityFX SDK 1.0, ffx_fsr3upscaler_x64.dll).
//
//   Both expose ffxFsr{2,3Upscaler}Context{Create,Dispatch,Destroy}, but their
//     structures changed between point releases with no version field. Only
//       the head of the context description (flags, max render size, display
//         size) is stable, so:
//
//   * The description is copied verbatim and replayed into AMD's real
//       ffx*ContextCreate if DLSS cannot be used (the game owns the context
//         memory, which stays untouched until then).
//
//   * The dispatch layout (the size of FfxResource) is detected on the first
//       dispatch by validating each candidate; if none fits, AMD takes over.
//
//   FSR 3.0's combined ffxFsr3Context (frame interpolation) consumes internal
//     upscaler resources, so upscaler contexts created from inside the
//       FidelityFX DLLs themselves are left alone.
//

#include <SpecialK/stdafx.h>

#include <SpecialK/render/ngx/ngx.h>
#include <SpecialK/render/ngx/ngx_ffx.h>

#ifdef  __SK_SUBSYSTEM__
#undef  __SK_SUBSYSTEM__
#endif
#define __SK_SUBSYSTEM__ L"FFX->DLSS"

using FfxErrorCode = int32_t;

constexpr FfxErrorCode FFX_OK                      = 0;
constexpr FfxErrorCode FFX_ERROR_INVALID_POINTER   = static_cast <FfxErrorCode> (0x80000000);
constexpr FfxErrorCode FFX_ERROR_BACKEND_API_ERROR = static_cast <FfxErrorCode> (0x8000000d);

using ContextCreate_pfn               = FfxErrorCode (*)(void* context, const void* contextDescription);
using ContextDispatch_pfn             = FfxErrorCode (*)(void* context, const void* dispatchDescription);
using ContextGenerateReactiveMask_pfn = FfxErrorCode (*)(void* context, const void* params);
using ContextDestroy_pfn              = FfxErrorCode (*)(void* context);
using GetUpscaleRatio_pfn             = float        (*)(uint32_t qualityMode);
using GetRenderResolution_pfn         = FfxErrorCode (*)(uint32_t* renderWidth, uint32_t* renderHeight, uint32_t displayWidth, uint32_t displayHeight, uint32_t qualityMode);

struct SK_FFX_LegacyFamily
{
  const wchar_t*                  name;
  const wchar_t*                  modules     [2];
  const wchar_t*                  dx12_backend;
  const wchar_t*                  vk_backend;

  // Candidate sizeof (FfxResource), most likely first
  uint32_t                        strides     [2];

  const char*                     create_fn;
  const char*                     dispatch_fn;
  const char*                     reactive_fn;
  const char*                     destroy_fn;
  const char*                     ratio_fn;
  const char*                     resolution_fn;

  ContextCreate_pfn               Create_Original     = nullptr;
  ContextDispatch_pfn             Dispatch_Original   = nullptr;
  ContextGenerateReactiveMask_pfn Reactive_Original   = nullptr;
  ContextDestroy_pfn              Destroy_Original    = nullptr;
  GetUpscaleRatio_pfn             Ratio_Original      = nullptr;
  GetRenderResolution_pfn         Resolution_Original = nullptr;

  bool                            hooked              = false;
};

static SK_FFX_LegacyFamily SK_FFX_Families [] = {
  { L"FSR 2.x", { L"ffx_fsr2_api_x64.dll",     nullptr            },
                  L"ffx_fsr2_api_dx12_x64.dll", L"ffx_fsr2_api_vk_x64.dll", { 184, 176 },
                  "ffxFsr2ContextCreate",                  "ffxFsr2ContextDispatch",
                  "ffxFsr2ContextGenerateReactiveMask",    "ffxFsr2ContextDestroy",
                  "ffxFsr2GetUpscaleRatioFromQualityMode", "ffxFsr2GetRenderResolutionFromQualityMode" },

  { L"FSR 3.0", { L"ffx_fsr3upscaler_x64.dll", L"ffx_fsr3_x64.dll" },
                  L"ffx_backend_dx12_x64.dll",  L"ffx_backend_vk_x64.dll",  { 176, 184 },
                  "ffxFsr3UpscalerContextCreate",                  "ffxFsr3UpscalerContextDispatch",
                  "ffxFsr3UpscalerContextGenerateReactiveMask",    "ffxFsr3UpscalerContextDestroy",
                  "ffxFsr3UpscalerGetUpscaleRatioFromQualityMode", "ffxFsr3UpscalerGetRenderResolutionFromQualityMode" },
};

struct SK_FFX_LegacyContext
{
  int                    family            = 0;

  // Stable head of Ffx{Fsr2,Fsr3Upscaler}ContextDescription
  uint32_t               flags             = 0;
  uint32_t               max_render_width  = 0, max_render_height = 0;
  uint32_t               display_width     = 0, display_height    = 0;

  std::vector <uint8_t>  desc_copy;

  // Resolved on first dispatch
  uint32_t               stride            = 0;
  size_t                 tail              = 0;
  bool                   has_upscale_size  = false;
  bool                   ngx_ready         = false;

  SK_FFX_DLSSFeature     dlss;

  // AMD's upscaler owns the game's context memory once this is set
  bool                   fallback          = false;
};

static SK_Thread_HybridSpinlock                                          SK_FFX_LegacyLock;
static std::unordered_map <void*, std::unique_ptr <SK_FFX_LegacyContext>> SK_FFX_LegacyContexts;

static SK_FFX_LegacyContext*
SK_FFX_GetLegacyContext (void* context)
{
  if (context == nullptr)
    return nullptr;

  std::scoped_lock lock (SK_FFX_LegacyLock);

  const auto it = SK_FFX_LegacyContexts.find (context);

  return
    it != SK_FFX_LegacyContexts.cend () ? it->second.get ()
                                        : nullptr;
}

template <typename T>
static T
SK_FFX_Read (const uint8_t* base, size_t offset)
{
  T                          value;
  memcpy (&value, base + offset, sizeof (T));
  return                     value;
}

// Number of bytes (up to cap) that can be read starting at p without faulting
static size_t
SK_FFX_ReadableBytes (const void* p, size_t cap)
{
  MEMORY_BASIC_INFORMATION mbi = { };

  if ( VirtualQuery (p, &mbi, sizeof (mbi)) == 0 || mbi.State != MEM_COMMIT ||
       (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 )
  {
    return 0;
  }

  const uintptr_t end =
    reinterpret_cast <uintptr_t> (mbi.BaseAddress) + mbi.RegionSize;

  return
    std::min (cap, static_cast <size_t> (end - reinterpret_cast <uintptr_t> (p)));
}

// Contexts created by FidelityFX itself (i.e. FSR 3.0 frame interpolation) stay with AMD
static bool
SK_FFX_IsCalledByGame (LPCVOID pReturn)
{
  const std::wstring caller =
    SK_GetModuleName (SK_GetCallingDLL (pReturn));

  const wchar_t* wszCaller =
    PathFindFileNameW (caller.c_str ());

  return
    StrStrIW (wszCaller, L"ffx_")           == nullptr &&
    StrStrIW (wszCaller, L"amd_fidelityfx") == nullptr;
}

// Legacy FSR's backend is chosen by the game; only D3D12 maps onto our NGX path.
static bool
SK_FFX_IsD3D12Backend (const SK_FFX_LegacyFamily& family)
{
  if (SK_GetCurrentRenderBackend ().api == SK_RenderAPI::D3D12)
    return true;

  return
    SK_GetModuleHandleW (family.dx12_backend) != nullptr &&
    SK_GetModuleHandleW (family.vk_backend)   == nullptr;
}

static bool
SK_FFX_ResolveLayout (SK_FFX_LegacyContext* ctx, const uint8_t* desc)
{
  const auto& family = SK_FFX_Families [ctx->family];

  const size_t readable =
    SK_FFX_ReadableBytes (desc, 4096);

  const uint32_t max_width  = std::max (ctx->max_render_width,  ctx->display_width);
  const uint32_t max_height = std::max (ctx->max_render_height, ctx->display_height);

  //
  // Dispatch description prefix, common to FSR 2.0 - 3.0:
  //
  //   FfxCommandList   commandList;
  //   FfxResource      color, depth, motionVectors, exposure,
  //                    reactive, transparencyAndComposition, output;
  //   FfxFloatCoords2D jitterOffset, motionVectorScale;
  //   FfxDimensions2D  renderSize;
  //  [FfxDimensions2D  upscaleOutputSize;]  (some FSR 3.0 releases)
  //   bool             enableSharpening;
  //   float            sharpness, frameTimeDelta, preExposure;
  //   bool             reset;
  //
  for ( const uint32_t stride : family.strides )
  {
    const size_t jitter = 8 + 7 * static_cast <size_t> (stride);
    const size_t render = jitter + 16;

    // Largest read below is the reset flag with an upscale size present
    if (render + 8 + 8 + 17 > readable)
      continue;

    auto resource = [&](int idx) {
      return SK_FFX_Read <uintptr_t> (desc, 8 + idx * static_cast <size_t> (stride));
    };

    const uintptr_t color  = resource (0);
    const uintptr_t output = resource (6);

    if (color == 0 || output == 0 || color == output)
      continue;

    bool plausible_pointers = true;

    for ( int idx = 0 ; idx < 7 ; ++idx )
    {
      const uintptr_t ptr = resource (idx);

      if (ptr != 0 && (ptr < 0x10000 || (ptr & 0x7) != 0))
        plausible_pointers = false;
    }

    if (! plausible_pointers)
      continue;

    const float jitter_x = SK_FFX_Read <float> (desc, jitter);
    const float jitter_y = SK_FFX_Read <float> (desc, jitter + 4);

    // Also rejects NaN
    if (! (std::fabs (jitter_x) <= 1.0f && std::fabs (jitter_y) <= 1.0f))
      continue;

    const uint32_t render_width  = SK_FFX_Read <uint32_t> (desc, render);
    const uint32_t render_height = SK_FFX_Read <uint32_t> (desc, render + 4);

    if ( render_width  == 0 || render_width  > max_width ||
         render_height == 0 || render_height > max_height )
    {
      continue;
    }

    size_t tail             = render + 8;
    bool   has_upscale_size = false;

    if ( SK_FFX_Read <uint32_t> (desc, tail)     == ctx->display_width &&
         SK_FFX_Read <uint32_t> (desc, tail + 4) == ctx->display_height )
    {
      has_upscale_size = true;
      tail            += 8;
    }

    const float frame_time   = SK_FFX_Read <float> (desc, tail + 8);
    const float pre_exposure = SK_FFX_Read <float> (desc, tail + 12);

    if (! (std::isfinite (frame_time) && std::isfinite (pre_exposure) && pre_exposure >= 0.0f))
      continue;

    ctx->stride           = stride;
    ctx->tail             = tail;
    ctx->has_upscale_size = has_upscale_size;

    SK_LOGi0 ( L"%ws dispatch layout: %u byte FfxResource%ws",
                 family.name, stride, has_upscale_size ? L", with upscale size"
                                                       : L"" );

    return true;
  }

  SK_LOGi0 (L"%ws dispatch layout not recognized.", family.name);

  return false;
}

static FfxErrorCode
SK_FFX_LegacyFallback (SK_FFX_LegacyContext* ctx, void* context, const void* desc)
{
  const auto& family = SK_FFX_Families [ctx->family];

  SK_LOGi0 (L"Falling back to AMD's %ws upscaler.", family.name);

  ctx->dlss.release ();

  const FfxErrorCode ret =
    family.Create_Original (context, ctx->desc_copy.data ());

  if (ret != FFX_OK)
  {
    SK_LOGi0 (L"%ws context creation failed - %x", family.name, ret);
    return ret;
  }

  ctx->fallback = true;

  return
    family.Dispatch_Original (context, desc);
}

static FfxErrorCode
SK_FFX_LegacyDispatch (SK_FFX_LegacyContext* ctx, void* context, const uint8_t* desc)
{
  auto pCmdList =
    SK_FFX_Read <ID3D12GraphicsCommandList *> (desc, 0);

  if (! ctx->ngx_ready)
  {
    if ( pCmdList == nullptr || (! SK_FFX_ResolveLayout (ctx, desc))
                             || (! SK_FFX_InitNGX       (pCmdList)) )
    {
      return SK_FFX_LegacyFallback (ctx, context, desc);
    }

    ctx->ngx_ready = true;
  }

  const size_t stride = ctx->stride;
  const size_t jitter = 8 + 7 * stride;
  const size_t render = jitter + 16;
  const size_t tail   = ctx->tail;

  auto resource = [&](int idx) {
    return SK_FFX_Read <ID3D12Resource *> (desc, 8 + idx * stride);
  };

  SK_FFX_DLSSInputs in;

  // Legacy FfxResource state bits moved between releases; games overwhelmingly
  //   pass inputs as COMPUTE_READ and the output as UAV (ffxGetResourceDX12's
  //     defaults), which is what SK_FFX_DLSSInputs assumes.
  in.cmd_list         = pCmdList;
  in.color            = resource (0);
  in.depth            = resource (1);
  in.motion_vectors   = resource (2);
  in.exposure         = resource (3);
  in.output           = resource (6);
  in.jitter_x         = SK_FFX_Read <float>    (desc, jitter);
  in.jitter_y         = SK_FFX_Read <float>    (desc, jitter + 4);
  in.mv_scale_x       = SK_FFX_Read <float>    (desc, jitter + 8);
  in.mv_scale_y       = SK_FFX_Read <float>    (desc, jitter + 12);
  in.render_width     = SK_FFX_Read <uint32_t> (desc, render);
  in.render_height    = SK_FFX_Read <uint32_t> (desc, render + 4);
  in.upscale_width    = ctx->has_upscale_size ? SK_FFX_Read <uint32_t> (desc, render + 8)  : ctx->display_width;
  in.upscale_height   = ctx->has_upscale_size ? SK_FFX_Read <uint32_t> (desc, render + 12) : ctx->display_height;
  in.frame_time_delta = SK_FFX_Read <float>    (desc, tail + 8);
  in.pre_exposure     = SK_FFX_Read <float>    (desc, tail + 12);
  in.reset            = SK_FFX_Read <uint8_t>  (desc, tail + 16) != 0;

  if ( in.cmd_list     == nullptr || in.color         == nullptr || in.output == nullptr ||
       in.render_width == 0       || in.render_height == 0 )
  {
    return FFX_ERROR_INVALID_POINTER;
  }

  switch ( ctx->dlss.evaluate ( ctx->flags, ctx->max_render_width,
                                            ctx->max_render_height, in ) )
  {
    case SK_FFX_DLSSResult::Success:
      return FFX_OK;

    case SK_FFX_DLSSResult::CreateFailed:
      return SK_FFX_LegacyFallback (ctx, context, desc);

    default:
      return FFX_ERROR_BACKEND_API_ERROR;
  }
}

template <int F>
static FfxErrorCode
ContextCreate_Detour (void* context, const void* contextDescription)
{
  SK_LOG_FIRST_CALL

  auto& family = SK_FFX_Families [F];

  if ( context == nullptr || contextDescription == nullptr ||
       (! SK_FFX_IsCalledByGame (_ReturnAddress ()))       ||
       (! SK_FFX_IsD3D12Backend (family)) )
  {
    return family.Create_Original (context, contextDescription);
  }

  // Generous enough for every known description, clipped to readable memory
  const size_t desc_size =
    SK_FFX_ReadableBytes (contextDescription, 1024);

  if (desc_size < 20)
    return family.Create_Original (context, contextDescription);

  auto desc =
    static_cast <const uint8_t *> (contextDescription);

  auto ctx =
    std::make_unique <SK_FFX_LegacyContext> ();

  ctx->family            = F;
  ctx->flags             = SK_FFX_Read <uint32_t> (desc, 0);
  ctx->max_render_width  = SK_FFX_Read <uint32_t> (desc, 4);
  ctx->max_render_height = SK_FFX_Read <uint32_t> (desc, 8);
  ctx->display_width     = SK_FFX_Read <uint32_t> (desc, 12);
  ctx->display_height    = SK_FFX_Read <uint32_t> (desc, 16);
  ctx->desc_copy.assign (desc, desc + desc_size);

  SK_LOGi0 ( L"Intercepted %ws Upscale Context (Max Render: %ux%u, Display: %ux%u, Flags=%x)",
               family.name, ctx->max_render_width, ctx->max_render_height,
                            ctx->display_width,    ctx->display_height, ctx->flags );

  std::scoped_lock lock (SK_FFX_LegacyLock);

  // The game may reuse context memory without destroying it first
  if (auto it  = SK_FFX_LegacyContexts.find (context);
           it != SK_FFX_LegacyContexts.end  ())
  {
    it->second->dlss.release ();
  }

  SK_FFX_LegacyContexts [context] = std::move (ctx);

  return FFX_OK;
}

template <int F>
static FfxErrorCode
ContextDispatch_Detour (void* context, const void* dispatchDescription)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetLegacyContext (context);

  if (ctx == nullptr || ctx->fallback)
    return SK_FFX_Families [F].Dispatch_Original (context, dispatchDescription);

  if (dispatchDescription == nullptr)
    return FFX_ERROR_INVALID_POINTER;

  return
    SK_FFX_LegacyDispatch (ctx, context, static_cast <const uint8_t *> (dispatchDescription));
}

template <int F>
static FfxErrorCode
ContextGenerateReactiveMask_Detour (void* context, const void* params)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetLegacyContext (context);

  if (ctx == nullptr || ctx->fallback)
    return SK_FFX_Families [F].Reactive_Original (context, params);

  // DLSS does not consume a reactive mask, so there is nothing to generate.
  return FFX_OK;
}

template <int F>
static FfxErrorCode
ContextDestroy_Detour (void* context)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetLegacyContext (context);

  if (ctx == nullptr)
    return SK_FFX_Families [F].Destroy_Original (context);

  // The game is responsible for having waited on the GPU before this, as FSR requires.
  ctx->dlss.release ();

  const FfxErrorCode ret =
    ctx->fallback ? SK_FFX_Families [F].Destroy_Original (context)
                  : FFX_OK;

  std::scoped_lock lock (SK_FFX_LegacyLock);

  SK_FFX_LegacyContexts.erase (context);

  return ret;
}

template <int F>
static float
GetUpscaleRatio_Detour (uint32_t qualityMode)
{
  SK_LOG_FIRST_CALL

  if (! SK_FFX_ShouldOverrideStaticQueries ())
    return SK_FFX_Families [F].Ratio_Original (qualityMode);

  return
    SK_FFX_GetUpscaleRatio (qualityMode);
}

template <int F>
static FfxErrorCode
GetRenderResolution_Detour ( uint32_t* renderWidth,  uint32_t* renderHeight,
                             uint32_t  displayWidth, uint32_t  displayHeight, uint32_t qualityMode )
{
  SK_LOG_FIRST_CALL

  if (! SK_FFX_ShouldOverrideStaticQueries ())
  {
    return
      SK_FFX_Families [F].Resolution_Original ( renderWidth,  renderHeight,
                                                displayWidth, displayHeight, qualityMode );
  }

  if (renderWidth == nullptr || renderHeight == nullptr)
    return FFX_ERROR_INVALID_POINTER;

  const float ratio =
    SK_FFX_GetUpscaleRatio (qualityMode);

  *renderWidth  = static_cast <uint32_t> (static_cast <float> (displayWidth)  / ratio);
  *renderHeight = static_cast <uint32_t> (static_cast <float> (displayHeight) / ratio);

  return FFX_OK;
}

template <int F>
static bool
SK_FFX_HookFamily (void)
{
  auto& family = SK_FFX_Families [F];

  if (family.hooked)
    return true;

  for ( const wchar_t* wszModule : family.modules )
  {
    if (wszModule == nullptr)
      continue;

    const HMODULE hMod =
      SK_GetModuleHandleW (wszModule);

    // ffx_fsr3_x64.dll only exports the upscaler when built that way
    if ( hMod == nullptr || ! ( SK_GetProcAddress (hMod, family.create_fn)   &&
                                SK_GetProcAddress (hMod, family.dispatch_fn) &&
                                SK_GetProcAddress (hMod, family.destroy_fn) ) )
    {
      continue;
    }

    SK_LOGi0 (L"Hooking %ws for DLSS replacement of %ws upscaling.", wszModule, family.name);

    SK_CreateDLLHook2 (wszModule, family.create_fn,   ContextCreate_Detour   <F>, (void **)&family.Create_Original);
    SK_CreateDLLHook2 (wszModule, family.dispatch_fn, ContextDispatch_Detour <F>, (void **)&family.Dispatch_Original);
    SK_CreateDLLHook2 (wszModule, family.destroy_fn,  ContextDestroy_Detour  <F>, (void **)&family.Destroy_Original);

    if (SK_GetProcAddress (hMod, family.reactive_fn))
      SK_CreateDLLHook2 (wszModule, family.reactive_fn,   ContextGenerateReactiveMask_Detour <F>, (void **)&family.Reactive_Original);

    if (SK_GetProcAddress (hMod, family.ratio_fn))
      SK_CreateDLLHook2 (wszModule, family.ratio_fn,      GetUpscaleRatio_Detour             <F>, (void **)&family.Ratio_Original);

    if (SK_GetProcAddress (hMod, family.resolution_fn))
      SK_CreateDLLHook2 (wszModule, family.resolution_fn, GetRenderResolution_Detour         <F>, (void **)&family.Resolution_Original);

    SK_ApplyQueuedHooks ();

    family.hooked = true;

    return true;
  }

  return false;
}

bool
SK_FFX_HookLegacy (void)
{
  const bool fsr2 = SK_FFX_HookFamily <0> ();
  const bool fsr3 = SK_FFX_HookFamily <1> ();

  // Keep trying on later DLL loads until every family is hooked
  return fsr2 && fsr3;
}
