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
// FidelityFX API (FSR 3.1+) front end.
//
//   Games that load amd_fidelityfx_dx12.dll (or the SDK 2.x loader DLL) have
//     their upscale contexts redirected to DLSS. Everything else (frame gen,
//       swapchain proxies, etc.) passes straight through to AMD's runtime.
//

#include <SpecialK/stdafx.h>

#include <SpecialK/render/ngx/ngx.h>
#include <SpecialK/render/ngx/ngx_ffx.h>

#ifdef  __SK_SUBSYSTEM__
#undef  __SK_SUBSYSTEM__
#endif
#define __SK_SUBSYSTEM__ L"FFX->DLSS"

//
// FidelityFX API ABI (ffx_api.h, ffx_api_types.h, ffx_upscale.h, ffx_api_dx12.h)
//
using ffxStructType_t = uint64_t;
using ffxReturnCode_t = uint32_t;
using ffxContext      = void*;

struct ffxApiHeader {
  ffxStructType_t      type;
  struct ffxApiHeader* pNext;
};

using ffxApiMessage = void (*)(uint32_t type, const wchar_t* message);
using ffxAlloc      = void*(*)(void* pUserData, uint64_t size);
using ffxDealloc    = void (*)(void* pUserData, void* pMem);

struct ffxAllocationCallbacks {
  void*      pUserData;
  ffxAlloc   alloc;
  ffxDealloc dealloc;
};

enum : ffxReturnCode_t {
  FFX_API_RETURN_OK                     = 0,
  FFX_API_RETURN_ERROR                  = 1,
  FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
  FFX_API_RETURN_ERROR_RUNTIME_ERROR    = 3,
  FFX_API_RETURN_NO_PROVIDER            = 4,
  FFX_API_RETURN_ERROR_MEMORY           = 5,
  FFX_API_RETURN_ERROR_PARAMETER        = 6,
};

constexpr ffxStructType_t FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12                      = 0x00000002u;
constexpr ffxStructType_t FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE                           = 0x00010000u;
constexpr ffxStructType_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE                                 = 0x00010001u;
constexpr ffxStructType_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE     = 0x00010002u;
constexpr ffxStructType_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE = 0x00010003u;
constexpr ffxStructType_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT                = 0x00010004u;
constexpr ffxStructType_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET                    = 0x00010005u;
constexpr ffxStructType_t FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE                   = 0x00010008u;

enum FfxApiResourceState : uint32_t {
  FFX_API_RESOURCE_STATE_COMMON            = (1 << 0),
  FFX_API_RESOURCE_STATE_UNORDERED_ACCESS  = (1 << 1),
  FFX_API_RESOURCE_STATE_COMPUTE_READ      = (1 << 2),
  FFX_API_RESOURCE_STATE_PIXEL_READ        = (1 << 3),
  FFX_API_RESOURCE_STATE_COPY_SRC          = (1 << 4),
  FFX_API_RESOURCE_STATE_COPY_DEST         = (1 << 5),
  FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT = (1 << 6),
  FFX_API_RESOURCE_STATE_PRESENT           = (1 << 7),
  FFX_API_RESOURCE_STATE_RENDER_TARGET     = (1 << 8),
};

struct FfxApiDimensions2D  { uint32_t width, height; };
struct FfxApiFloatCoords2D { float    x,     y;      };

struct FfxApiResourceDescription {
  uint32_t type;
  uint32_t format;
  union { uint32_t width;  uint32_t size;      };
  union { uint32_t height; uint32_t stride;    };
  union { uint32_t depth;  uint32_t alignment; };
  uint32_t mipCount;
  uint32_t flags;
  uint32_t usage;
};

struct FfxApiResource {
  void*                     resource;
  FfxApiResourceDescription description;
  uint32_t                  state;
};

struct ffxCreateBackendDX12Desc {
  ffxApiHeader  header;
  ID3D12Device* device;
};

struct ffxCreateContextDescUpscale {
  ffxApiHeader       header;
  uint32_t           flags;
  FfxApiDimensions2D maxRenderSize;
  FfxApiDimensions2D maxUpscaleSize;
  ffxApiMessage      fpMessage;
};

struct ffxDispatchDescUpscale {
  ffxApiHeader        header;
  void*               commandList;
  FfxApiResource      color;
  FfxApiResource      depth;
  FfxApiResource      motionVectors;
  FfxApiResource      exposure;
  FfxApiResource      reactive;
  FfxApiResource      transparencyAndComposition;
  FfxApiResource      output;
  FfxApiFloatCoords2D jitterOffset;
  FfxApiFloatCoords2D motionVectorScale;
  FfxApiDimensions2D  renderSize;
  FfxApiDimensions2D  upscaleSize;
  bool                enableSharpening;
  float               sharpness;
  float               frameTimeDelta;
  float               preExposure;
  bool                reset;
  float               cameraNear;
  float               cameraFar;
  float               cameraFovAngleVertical;
  float               viewSpaceToMetersFactor;
  uint32_t            flags;
};

struct ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode {
  ffxApiHeader header;
  uint32_t     qualityMode;
  float*       pOutUpscaleRatio;
};

struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode {
  ffxApiHeader header;
  uint32_t     displayWidth;
  uint32_t     displayHeight;
  uint32_t     qualityMode;
  uint32_t*    pOutRenderWidth;
  uint32_t*    pOutRenderHeight;
};

struct ffxQueryDescUpscaleGetJitterPhaseCount {
  ffxApiHeader header;
  uint32_t     renderWidth;
  uint32_t     displayWidth;
  int32_t*     pOutPhaseCount;
};

struct ffxQueryDescUpscaleGetJitterOffset {
  ffxApiHeader header;
  int32_t      index;
  int32_t      phaseCount;
  float*       pOutX;
  float*       pOutY;
};

struct ffxQueryDescUpscaleGetGPUMemoryUsage {
  ffxApiHeader header;
  struct {
    uint64_t totalUsageInBytes;
    uint64_t aliasableUsageInBytes;
  }           *gpuMemoryUsageUpscaler;
};

using ffxCreateContext_pfn  = ffxReturnCode_t (*)(ffxContext* context, ffxApiHeader* desc, const ffxAllocationCallbacks* memCb);
using ffxDestroyContext_pfn = ffxReturnCode_t (*)(ffxContext* context,                     const ffxAllocationCallbacks* memCb);
using ffxConfigure_pfn      = ffxReturnCode_t (*)(ffxContext* context, const ffxApiHeader* desc);
using ffxQuery_pfn          = ffxReturnCode_t (*)(ffxContext* context,       ffxApiHeader* desc);
using ffxDispatch_pfn       = ffxReturnCode_t (*)(ffxContext* context, const ffxApiHeader* desc);

static ffxCreateContext_pfn  ffxCreateContext_Original  = nullptr;
static ffxDestroyContext_pfn ffxDestroyContext_Original = nullptr;
static ffxConfigure_pfn      ffxConfigure_Original      = nullptr;
static ffxQuery_pfn          ffxQuery_Original          = nullptr;
static ffxDispatch_pfn       ffxDispatch_Original       = nullptr;

struct SK_FFX_UpscaleContext
{
  // Copy of the game's creation parameters, used to fall back to AMD's
  //   upscaler if DLSS fails.
  ffxCreateContextDescUpscale desc        = { };
  ffxCreateBackendDX12Desc    backend     = { };
  ffxAllocationCallbacks      mem_cb      = { };
  bool                        has_mem_cb  = false;

  SK_FFX_DLSSFeature          dlss;

  // Non-null once DLSS has failed and AMD's upscaler took over
  ffxContext                  fallback    = nullptr;
};

static SK_Thread_HybridSpinlock                         SK_FFX_Lock;
static std::unordered_map <ffxContext, std::unique_ptr <SK_FFX_UpscaleContext>>
                                                        SK_FFX_Contexts;

static SK_FFX_UpscaleContext*
SK_FFX_GetContext (ffxContext* context)
{
  if (context == nullptr || *context == nullptr)
    return nullptr;

  std::scoped_lock lock (SK_FFX_Lock);

  const auto it = SK_FFX_Contexts.find (*context);

  return
    it != SK_FFX_Contexts.cend () ? it->second.get ()
                                  : nullptr;
}

static const ffxApiHeader*
SK_FFX_FindDesc (const ffxApiHeader* pHeader, ffxStructType_t type)
{
  for ( ; pHeader != nullptr ; pHeader = pHeader->pNext )
  {
    if (pHeader->type == type)
      return pHeader;
  }

  return nullptr;
}

// Answers the upscaler queries that do not need a provider; returns false if unhandled.
static bool
SK_FFX_HandleUpscaleQuery (ffxApiHeader* desc, ffxReturnCode_t& ret)
{
  ret = FFX_API_RETURN_OK;

  switch (desc->type)
  {
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE:
    {
      auto query =
        reinterpret_cast <ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode *> (desc);

      if (query->pOutUpscaleRatio != nullptr)
         *query->pOutUpscaleRatio = SK_FFX_GetUpscaleRatio (query->qualityMode);
    } return true;

    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE:
    {
      auto query =
        reinterpret_cast <ffxQueryDescUpscaleGetRenderResolutionFromQualityMode *> (desc);

      const float ratio =
        SK_FFX_GetUpscaleRatio (query->qualityMode);

      if (query->pOutRenderWidth  != nullptr) *query->pOutRenderWidth  = static_cast <uint32_t> (static_cast <float> (query->displayWidth)  / ratio);
      if (query->pOutRenderHeight != nullptr) *query->pOutRenderHeight = static_cast <uint32_t> (static_cast <float> (query->displayHeight) / ratio);
    } return true;

    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT:
    {
      auto query =
        reinterpret_cast <ffxQueryDescUpscaleGetJitterPhaseCount *> (desc);

      const float ratio =
        static_cast <float> (query->displayWidth) /
        static_cast <float> (std::max (1u, query->renderWidth));

      if (query->pOutPhaseCount != nullptr)
         *query->pOutPhaseCount = static_cast <int32_t> (8.0f * ratio * ratio);
    } return true;

    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET:
    {
      auto query =
        reinterpret_cast <ffxQueryDescUpscaleGetJitterOffset *> (desc);

      const int32_t phase =
        ( query->index % std::max (1, query->phaseCount) ) + 1;

      if (query->pOutX != nullptr) *query->pOutX = SK_FFX_Halton (phase, 2) - 0.5f;
      if (query->pOutY != nullptr) *query->pOutY = SK_FFX_Halton (phase, 3) - 0.5f;
    } return true;

    default:
      return false;
  }
}

static D3D12_RESOURCE_STATES
SK_FFX_ToD3D12State (uint32_t state)
{
  int d3d12_state = D3D12_RESOURCE_STATE_COMMON;

  if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)  d3d12_state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (state & FFX_API_RESOURCE_STATE_COMPUTE_READ)      d3d12_state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_PIXEL_READ)        d3d12_state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_SRC)          d3d12_state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_DEST)         d3d12_state |= D3D12_RESOURCE_STATE_COPY_DEST;
  if (state & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) d3d12_state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET)     d3d12_state |= D3D12_RESOURCE_STATE_RENDER_TARGET;

  return static_cast <D3D12_RESOURCE_STATES> (d3d12_state);
}

static ffxReturnCode_t
SK_FFX_ActivateFallback (SK_FFX_UpscaleContext* ctx)
{
  SK_LOGi0 (L"Falling back to AMD's FSR upscaler.");

  ctx->desc.header.pNext    = &ctx->backend.header;
  ctx->backend.header.pNext = nullptr;

  return
    ffxCreateContext_Original ( &ctx->fallback, &ctx->desc.header,
                                  ctx->has_mem_cb ? &ctx->mem_cb : nullptr );
}

static ffxReturnCode_t
SK_FFX_Dispatch (SK_FFX_UpscaleContext* ctx, const ffxDispatchDescUpscale* dispatch)
{
  if ( dispatch->commandList     == nullptr ||
       dispatch->color.resource  == nullptr ||
       dispatch->output.resource == nullptr )
  {
    return FFX_API_RETURN_ERROR_PARAMETER;
  }

  SK_FFX_DLSSInputs in;

  in.cmd_list         = static_cast <ID3D12GraphicsCommandList *> (dispatch->commandList);
  in.color            = static_cast <ID3D12Resource *>            (dispatch->color.resource);
  in.depth            = static_cast <ID3D12Resource *>            (dispatch->depth.resource);
  in.motion_vectors   = static_cast <ID3D12Resource *>            (dispatch->motionVectors.resource);
  in.exposure         = static_cast <ID3D12Resource *>            (dispatch->exposure.resource);
  in.output           = static_cast <ID3D12Resource *>            (dispatch->output.resource);
  in.color_state      = SK_FFX_ToD3D12State (dispatch->color.state);
  in.depth_state      = SK_FFX_ToD3D12State (dispatch->depth.state);
  in.mv_state         = SK_FFX_ToD3D12State (dispatch->motionVectors.state);
  in.exposure_state   = SK_FFX_ToD3D12State (dispatch->exposure.state);
  in.output_state     = SK_FFX_ToD3D12State (dispatch->output.state);
  in.jitter_x         = dispatch->jitterOffset.x;
  in.jitter_y         = dispatch->jitterOffset.y;
  in.mv_scale_x       = dispatch->motionVectorScale.x;
  in.mv_scale_y       = dispatch->motionVectorScale.y;
  in.render_width     = dispatch->renderSize.width;
  in.render_height    = dispatch->renderSize.height;
  in.upscale_width    = dispatch->upscaleSize.width;
  in.upscale_height   = dispatch->upscaleSize.height;
  in.frame_time_delta = dispatch->frameTimeDelta;
  in.pre_exposure     = dispatch->preExposure;
  in.reset            = dispatch->reset;

  switch ( ctx->dlss.evaluate ( ctx->desc.flags, ctx->desc.maxRenderSize.width,
                                                 ctx->desc.maxRenderSize.height, in ) )
  {
    case SK_FFX_DLSSResult::Success:
      return FFX_API_RETURN_OK;

    case SK_FFX_DLSSResult::CreateFailed:
      if (SK_FFX_ActivateFallback (ctx) != FFX_API_RETURN_OK)
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;

      return
        ffxDispatch_Original (&ctx->fallback, &dispatch->header);

    default:
      return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  }
}

static ffxReturnCode_t
ffxCreateContext_Detour (ffxContext* context, ffxApiHeader* desc, const ffxAllocationCallbacks* memCb)
{
  SK_LOG_FIRST_CALL

  if (context == nullptr || desc == nullptr || desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
    return ffxCreateContext_Original (context, desc, memCb);

  auto backend =
    reinterpret_cast <const ffxCreateBackendDX12Desc *> (
      SK_FFX_FindDesc (desc->pNext, FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
    );

  if (backend == nullptr || (! SK_FFX_InitNGX (backend->device)))
    return ffxCreateContext_Original (context, desc, memCb);

  auto ctx =
    std::make_unique <SK_FFX_UpscaleContext> ();

  ctx->desc    = *reinterpret_cast <const ffxCreateContextDescUpscale *> (desc);
  ctx->backend = *backend;

  if (memCb != nullptr)
  {
    ctx->mem_cb     = *memCb;
    ctx->has_mem_cb = true;
  }

  *context = ctx.get ();

  SK_LOGi0 ( L"Intercepted FSR 3.1 Upscale Context (Max Render: %ux%u, Max Upscale: %ux%u, Flags=%x)",
               ctx->desc.maxRenderSize.width,  ctx->desc.maxRenderSize.height,
               ctx->desc.maxUpscaleSize.width, ctx->desc.maxUpscaleSize.height, ctx->desc.flags );

  std::scoped_lock lock (SK_FFX_Lock);

  SK_FFX_Contexts.emplace (*context, std::move (ctx));

  return FFX_API_RETURN_OK;
}

static ffxReturnCode_t
ffxDestroyContext_Detour (ffxContext* context, const ffxAllocationCallbacks* memCb)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetContext (context);

  if (ctx == nullptr)
    return ffxDestroyContext_Original (context, memCb);

  // The game is responsible for having waited on the GPU before this, as FSR requires.
  ctx->dlss.release ();

  if (ctx->fallback != nullptr)
    ffxDestroyContext_Original (&ctx->fallback, memCb);

  std::scoped_lock lock (SK_FFX_Lock);

  SK_FFX_Contexts.erase (*context);

  *context = nullptr;

  return FFX_API_RETURN_OK;
}

static ffxReturnCode_t
ffxConfigure_Detour (ffxContext* context, const ffxApiHeader* desc)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetContext (context);

  if (ctx == nullptr)
    return ffxConfigure_Original (context, desc);

  if (ctx->fallback != nullptr)
    return ffxConfigure_Original (&ctx->fallback, desc);

  // FSR-specific tuning knobs (key/value) have no DLSS equivalent.
  return FFX_API_RETURN_OK;
}

static ffxReturnCode_t
ffxQuery_Detour (ffxContext* context, ffxApiHeader* desc)
{
  SK_LOG_FIRST_CALL

  if (desc == nullptr)
    return ffxQuery_Original (context, desc);

  auto ctx =
    SK_FFX_GetContext (context);

  if (ctx != nullptr && ctx->fallback != nullptr)
    return ffxQuery_Original (&ctx->fallback, desc);

  // Static upscaler queries are answered here as well, so render resolution
  //   honors Special K's DLSS scale overrides even before a context exists.
  const bool is_static_query =
    (context == nullptr || *context == nullptr) && SK_FFX_ShouldOverrideStaticQueries ();

  if (ctx != nullptr || is_static_query)
  {
    ffxReturnCode_t ret;

    if (SK_FFX_HandleUpscaleQuery (desc, ret))
      return ret;
  }

  if (ctx == nullptr)
    return ffxQuery_Original (context, desc);

  if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE)
  {
    auto query =
      reinterpret_cast <ffxQueryDescUpscaleGetGPUMemoryUsage *> (desc);

    if (query->gpuMemoryUsageUpscaler != nullptr)
       *query->gpuMemoryUsageUpscaler = { };

    return FFX_API_RETURN_OK;
  }

  return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

static ffxReturnCode_t
ffxDispatch_Detour (ffxContext* context, const ffxApiHeader* desc)
{
  SK_LOG_FIRST_CALL

  auto ctx =
    SK_FFX_GetContext (context);

  if (ctx == nullptr)
    return ffxDispatch_Original (context, desc);

  if (ctx->fallback != nullptr)
    return ffxDispatch_Original (&ctx->fallback, desc);

  if (desc == nullptr || desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;

  return
    SK_FFX_Dispatch (ctx, reinterpret_cast <const ffxDispatchDescUpscale *> (desc));
}

static bool
SK_FFX_HookAPIModule (const wchar_t* wszModule)
{
  if (! SK_GetModuleHandleW (wszModule))
    return false;

  SK_LOGi0 (L"Hooking %ws for DLSS replacement of FSR 3.1+ upscaling.", wszModule);

  SK_CreateDLLHook2 (wszModule, "ffxCreateContext",  ffxCreateContext_Detour,  (void **)&ffxCreateContext_Original);
  SK_CreateDLLHook2 (wszModule, "ffxDestroyContext", ffxDestroyContext_Detour, (void **)&ffxDestroyContext_Original);
  SK_CreateDLLHook2 (wszModule, "ffxConfigure",      ffxConfigure_Detour,      (void **)&ffxConfigure_Original);
  SK_CreateDLLHook2 (wszModule, "ffxQuery",          ffxQuery_Detour,          (void **)&ffxQuery_Original);
  SK_CreateDLLHook2 (wszModule, "ffxDispatch",       ffxDispatch_Detour,       (void **)&ffxDispatch_Original);

  SK_ApplyQueuedHooks ();

  return true;
}

bool
SK_FFX_HookAPI (void)
{
  // FidelityFX SDK 1.1 ships a single DLL, SDK 2.x splits the API into a loader
  //   plus per-effect providers; both export the same five functions.
  return
    SK_FFX_HookAPIModule (L"amd_fidelityfx_loader_dx12.dll") ||
    SK_FFX_HookAPIModule (L"amd_fidelityfx_dx12.dll");
}
