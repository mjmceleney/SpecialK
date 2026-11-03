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
// FidelityFX -> DLSS Super Resolution: shared NGX backend.
//
//   The front ends (ngx_ffx_api.cpp, ngx_ffx_legacy.cpp) translate each FSR
//     generation's structures into SK_FFX_DLSSInputs; everything NGX lives here.
//

#include <SpecialK/stdafx.h>

#include <SpecialK/render/ngx/ngx.h>
#include <SpecialK/render/ngx/ngx_dlss.h>
#include <SpecialK/render/ngx/ngx_ffx.h>

#ifdef  __SK_SUBSYSTEM__
#undef  __SK_SUBSYSTEM__
#endif
#define __SK_SUBSYSTEM__ L"FFX->DLSS"

//
// NGX entry points, resolved from _nvngx.dll. Calls go through Special K's
//   NGX hooks, so preset / DLAA / scale overrides apply to these features too.
//
using NVSDK_NGX_D3D12_Init_ProjectID_pfn          = NVSDK_NGX_Result (NVSDK_CONV *)(const char *InProjectId, NVSDK_NGX_EngineType InEngineType, const char *InEngineVersion, const wchar_t *InApplicationDataPath, ID3D12Device *InDevice, const NVSDK_NGX_FeatureCommonInfo *InFeatureInfo, NVSDK_NGX_Version InSDKVersion);
using NVSDK_NGX_D3D12_GetCapabilityParameters_pfn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter** OutParameters);
using NVSDK_NGX_D3D12_AllocateParameters_pfn      = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter** OutParameters);
using NVSDK_NGX_D3D12_DestroyParameters_pfn       = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter*  InParameters);
using NVSDK_NGX_D3D12_CreateFeature_pfn           = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *InCmdList, NVSDK_NGX_Feature InFeatureID, NVSDK_NGX_Parameter *InParameters, NVSDK_NGX_Handle **OutHandle);
using NVSDK_NGX_D3D12_ReleaseFeature_pfn          = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *InHandle);
using NVSDK_NGX_D3D12_EvaluateFeature_pfn         = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *InCmdList, const NVSDK_NGX_Handle *InFeatureHandle, const NVSDK_NGX_Parameter *InParameters, void *InCallback);

static struct {
  NVSDK_NGX_D3D12_Init_ProjectID_pfn          Init_ProjectID          = nullptr;
  NVSDK_NGX_D3D12_GetCapabilityParameters_pfn GetCapabilityParameters = nullptr;
  NVSDK_NGX_D3D12_AllocateParameters_pfn      AllocateParameters      = nullptr;
  NVSDK_NGX_D3D12_DestroyParameters_pfn       DestroyParameters       = nullptr;
  NVSDK_NGX_D3D12_CreateFeature_pfn           CreateFeature           = nullptr;
  NVSDK_NGX_D3D12_ReleaseFeature_pfn          ReleaseFeature          = nullptr;
  NVSDK_NGX_D3D12_EvaluateFeature_pfn         EvaluateFeature         = nullptr;

  // Canonical IUnknown of the device NGX was last initialized on (not ref-counted)
  IUnknown*                                   pDevice                 = nullptr;
  bool                                        bSuperSampling          = false;
} SK_FFX_NGX;

static SK_Thread_HybridSpinlock SK_FFX_NGXLock;

// Number of DLSS features ever created in place of FSR (for the control panel)
static volatile LONG            SK_FFX_FeaturesCreated = 0;

bool
SK_FFX_InitNGX (ID3D12Device* pDevice)
{
  if (pDevice == nullptr)
    return false;

  // Different interface pointers can name the same device, compare identities
  SK_ComPtr <IUnknown>                                                pUnkDevice;
  if (FAILED (pDevice->QueryInterface (IID_PPV_ARGS (&pUnkDevice.p))))
    return false;

  std::scoped_lock lock (SK_FFX_NGXLock);

  if (SK_FFX_NGX.pDevice == pUnkDevice.p)
    return SK_FFX_NGX.bSuperSampling;

  // Any early-out below leaves DLSS marked unavailable for this device
  SK_FFX_NGX.pDevice        = pUnkDevice.p;
  SK_FFX_NGX.bSuperSampling = false;

  HMODULE hModNGX =
    SK_GetModuleHandleW (L"_nvngx.dll");

  // Game does not use NGX itself, so we have to load it from the driver store.
  if (hModNGX == nullptr)
  {
    wchar_t wszNGXPath [MAX_PATH + 2] = { };
    DWORD   dwLen                     = sizeof (wszNGXPath) - sizeof (wchar_t) * 2;

    if ( ERROR_SUCCESS ==
           RegGetValueW ( HKEY_LOCAL_MACHINE,
                            LR"(SOFTWARE\NVIDIA Corporation\Global\NGXCore)",
                                     L"FullPath", RRF_RT_REG_SZ, nullptr,
                                                    wszNGXPath, &dwLen ) )
    {
      PathAppendW (wszNGXPath, L"_nvngx.dll");

      hModNGX =
        SK_LoadLibraryW (wszNGXPath);
    }

    if (hModNGX == nullptr)
    {
      SK_LOGi0 (L"Unable to locate _nvngx.dll, DLSS is unavailable.");
      return false;
    }
  }

  // Installs Special K's NGX hooks (no-op if the game already loaded NGX)
  SK_NGX_Init ();

  SK_FFX_NGX.Init_ProjectID          = (NVSDK_NGX_D3D12_Init_ProjectID_pfn)          SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_Init_ProjectID");
  SK_FFX_NGX.GetCapabilityParameters = (NVSDK_NGX_D3D12_GetCapabilityParameters_pfn) SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_GetCapabilityParameters");
  SK_FFX_NGX.AllocateParameters      = (NVSDK_NGX_D3D12_AllocateParameters_pfn)      SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_AllocateParameters");
  SK_FFX_NGX.DestroyParameters       = (NVSDK_NGX_D3D12_DestroyParameters_pfn)       SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_DestroyParameters");
  SK_FFX_NGX.CreateFeature           = (NVSDK_NGX_D3D12_CreateFeature_pfn)           SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_CreateFeature");
  SK_FFX_NGX.ReleaseFeature          = (NVSDK_NGX_D3D12_ReleaseFeature_pfn)          SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_ReleaseFeature");
  SK_FFX_NGX.EvaluateFeature         = (NVSDK_NGX_D3D12_EvaluateFeature_pfn)         SK_GetProcAddress (hModNGX, "NVSDK_NGX_D3D12_EvaluateFeature");

  if (! ( SK_FFX_NGX.Init_ProjectID          && SK_FFX_NGX.GetCapabilityParameters &&
          SK_FFX_NGX.AllocateParameters      && SK_FFX_NGX.DestroyParameters       &&
          SK_FFX_NGX.CreateFeature           && SK_FFX_NGX.ReleaseFeature          &&
          SK_FFX_NGX.EvaluateFeature ))
  {
    SK_LOGi0 (L"_nvngx.dll is missing required exports, DLSS is unavailable.");
    return false;
  }

  // Search Special K's DLSS plug-in directory (the same one AutoRedirectDLL
  //   uses) for nvngx_dlss.dll, in addition to the game's own directory.
  static std::wstring plugin_dir =
    (std::filesystem::path (SK_GetPlugInDirectory (SK_PlugIn_Type::ThirdParty)) / L"NVIDIA").wstring ();

  static const wchar_t* paths [] = { plugin_dir.c_str () };

  NVSDK_NGX_FeatureCommonInfo
    feature_info                     = { };
    feature_info.PathListInfo.Path   = paths;
    feature_info.PathListInfo.Length = 1;

  NVSDK_NGX_Result ret =
    SK_FFX_NGX.Init_ProjectID ( "24480451-f00d-face-1304-0308dabad187",
                                  NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                    SK_GetConfigPath (), pDevice,
                                      &feature_info, NVSDK_NGX_Version_API );

  if (NVSDK_NGX_FAILED (ret))
  {
    SK_LOGi0 (L"NVSDK_NGX_D3D12_Init_ProjectID Failed - %x", ret);
    return false;
  }

  NVSDK_NGX_Parameter* caps = nullptr;

  if (NVSDK_NGX_SUCCEED (SK_FFX_NGX.GetCapabilityParameters (&caps)) && caps != nullptr)
  {
    int available = 0;

    caps->Get (NVSDK_NGX_Parameter_SuperSampling_Available, &available);

    SK_FFX_NGX.bSuperSampling = (available != 0);
  }

  SK_LOGi0 (L"DLSS Super Resolution is %ws", SK_FFX_NGX.bSuperSampling ? L"available"
                                                                       : L"unavailable");

  return SK_FFX_NGX.bSuperSampling;
}

bool
SK_FFX_InitNGX (ID3D12GraphicsCommandList* pCmdList)
{
  if (pCmdList == nullptr)
    return false;

  SK_ComPtr <ID3D12Device>                                 pDevice;
  if (FAILED (pCmdList->GetDevice (IID_PPV_ARGS (&pDevice.p))))
    return false;

  return
    SK_FFX_InitNGX (pDevice.p);
}

bool
SK_FFX_ShouldOverrideStaticQueries (void)
{
  return
    SK_FFX_NGX.pDevice == nullptr || SK_FFX_NGX.bSuperSampling;
}

float
SK_FFX_GetUpscaleRatio (uint32_t qualityMode)
{
  if (config.nvidia.dlss.force_dlaa)
    return 1.0f;

  float custom_scale = 0.0f,
        ratio        = 1.0f;

  switch (qualityMode)
  {
    case 1: ratio = 1.5f; custom_scale = config.nvidia.dlss.scale.quality;           break;
    case 2: ratio = 1.7f; custom_scale = config.nvidia.dlss.scale.balanced;          break;
    case 3: ratio = 2.0f; custom_scale = config.nvidia.dlss.scale.performance;       break;
    case 4: ratio = 3.0f; custom_scale = config.nvidia.dlss.scale.ultra_performance; break;
    case 0: // Native AA
    default:
      break;
  }

  return
    custom_scale > 0.0f ? 1.0f / custom_scale
                        : ratio;
}

float
SK_FFX_Halton (int index, int base)
{
  float f      = 1.0f,
        result = 0.0f;

  for ( int current = index ; current > 0 ; current /= base )
  {
    f      /= static_cast <float> (base);
    result += f * static_cast <float> (current % base);
  }

  return result;
}

static NVSDK_NGX_PerfQuality_Value
SK_FFX_PerfQualityFromRatio (float ratio)
{
  if (ratio <= 1.01f)
  {
    return
      SK_DLSS_Context::dlss_s::hasDLAAQualityLevel () ? NVSDK_NGX_PerfQuality_Value_DLAA
                                                      : NVSDK_NGX_PerfQuality_Value_MaxQuality;
  }

  if (ratio <= 1.55f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  if (ratio <= 1.80f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  if (ratio <= 2.20f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;

  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

void
SK_FFX_DLSSFeature::release (void)
{
  if (handle != nullptr)
  {
    SK_FFX_NGX.ReleaseFeature (handle);
    handle = nullptr;
  }

  if (params != nullptr)
  {
    SK_FFX_NGX.DestroyParameters (params);
    params = nullptr;
  }
}

static void
SK_FFX_AddTransition ( std::vector <D3D12_RESOURCE_BARRIER>& barriers,
                       ID3D12Resource*                       pResource,
                       D3D12_RESOURCE_STATES                 before,
                       bool                                  is_output )
{
  if (pResource == nullptr)
    return;

  if (is_output ? (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                : (before &  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0)
  {
    return;
  }

  for ( const auto& barrier : barriers )
  {
    if (barrier.Transition.pResource == pResource)
      return;
  }

  D3D12_RESOURCE_BARRIER
    barrier                        = { };
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = pResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = is_output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  barriers.push_back (barrier);
}

SK_FFX_DLSSResult
SK_FFX_DLSSFeature::evaluate ( uint32_t                 ffx_flags,
                               uint32_t                 max_render_width,
                               uint32_t                 max_render_height,
                               const SK_FFX_DLSSInputs& in )
{
  const bool needs_feature =
    handle    == nullptr           ||
    out_width  != in.upscale_width ||
    out_height != in.upscale_height||
    in_width   <  in.render_width  ||
    in_height  <  in.render_height;

  if (needs_feature)
  {
    release ();

    if (NVSDK_NGX_FAILED (SK_FFX_NGX.AllocateParameters (&params)) || params == nullptr)
    {
      params = nullptr;
      return SK_FFX_DLSSResult::CreateFailed;
    }

    int dlss_flags = NVSDK_NGX_DLSS_Feature_Flags_None;

    if (  ffx_flags & SK_FFX_ENABLE_HIGH_DYNAMIC_RANGE)                  dlss_flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (!(ffx_flags & SK_FFX_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS))  dlss_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (  ffx_flags & SK_FFX_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)  dlss_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (  ffx_flags & SK_FFX_ENABLE_DEPTH_INVERTED)                      dlss_flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (  ffx_flags & SK_FFX_ENABLE_AUTO_EXPOSURE)                       dlss_flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    // Input resolution is the largest the game said it would render at, so
    //   dynamic resolution only needs a subrect change, not a new feature.
    in_width   = std::max (max_render_width,  in.render_width);
    in_height  = std::max (max_render_height, in.render_height);
    out_width  = in.upscale_width;
    out_height = in.upscale_height;

    const float ratio =
      static_cast <float> (in.upscale_width) /
      static_cast <float> (std::max (1u, in.render_width));

    params->Set (NVSDK_NGX_Parameter_CreationNodeMask,            1u);
    params->Set (NVSDK_NGX_Parameter_VisibilityNodeMask,          1u);
    params->Set (NVSDK_NGX_Parameter_Width,                       in_width);
    params->Set (NVSDK_NGX_Parameter_Height,                      in_height);
    params->Set (NVSDK_NGX_Parameter_OutWidth,                    out_width);
    params->Set (NVSDK_NGX_Parameter_OutHeight,                   out_height);
    params->Set (NVSDK_NGX_Parameter_PerfQualityValue,            static_cast <int> (SK_FFX_PerfQualityFromRatio (ratio)));
    params->Set (NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,   dlss_flags);
    params->Set (NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);

    const NVSDK_NGX_Result ret =
      SK_FFX_NGX.CreateFeature (in.cmd_list, NVSDK_NGX_Feature_SuperSampling, params, &handle);

    if (NVSDK_NGX_FAILED (ret))
    {
      SK_LOGi0 (L"DLSS Feature Creation Failed - %x", ret);

      handle = nullptr;
      release ();

      return SK_FFX_DLSSResult::CreateFailed;
    }

    SK_LOGi0 ( L"Replaced FSR Upscaler With DLSS (%ux%u -> %ux%u, Flags=%x)",
                 in_width, in_height, out_width, out_height, dlss_flags );

    InterlockedIncrement (&SK_FFX_FeaturesCreated);
  }

  std::vector <D3D12_RESOURCE_BARRIER> barriers;

  SK_FFX_AddTransition (barriers, in.color,          in.color_state,    false);
  SK_FFX_AddTransition (barriers, in.depth,          in.depth_state,    false);
  SK_FFX_AddTransition (barriers, in.motion_vectors, in.mv_state,       false);
  SK_FFX_AddTransition (barriers, in.exposure,       in.exposure_state, false);
  SK_FFX_AddTransition (barriers, in.output,         in.output_state,   true);

  if (! barriers.empty ())
    in.cmd_list->ResourceBarrier (static_cast <UINT> (barriers.size ()), barriers.data ());

  params->Set (NVSDK_NGX_Parameter_Color,           in.color);
  params->Set (NVSDK_NGX_Parameter_Output,          in.output);
  params->Set (NVSDK_NGX_Parameter_Depth,           in.depth);
  params->Set (NVSDK_NGX_Parameter_MotionVectors,   in.motion_vectors);
  params->Set (NVSDK_NGX_Parameter_ExposureTexture, in.exposure);

  // FSR's reactive and transparency/composition masks have no reliable DLSS
  //   equivalent (DLSS's bias mask behaves differently), so they are dropped.

  params->Set (NVSDK_NGX_Parameter_Jitter_Offset_X,      in.jitter_x);
  params->Set (NVSDK_NGX_Parameter_Jitter_Offset_Y,      in.jitter_y);
  params->Set (NVSDK_NGX_Parameter_MV_Scale_X,           in.mv_scale_x);
  params->Set (NVSDK_NGX_Parameter_MV_Scale_Y,           in.mv_scale_y);
  params->Set (NVSDK_NGX_Parameter_Reset,                in.reset ? 1 : 0);
  params->Set (NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, in.frame_time_delta);
  params->Set (NVSDK_NGX_Parameter_DLSS_Pre_Exposure,    in.pre_exposure > 0.0f ? in.pre_exposure : 1.0f);
  params->Set (NVSDK_NGX_Parameter_DLSS_Exposure_Scale,  1.0f);

  params->Set (NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  in.render_width);
  params->Set (NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, in.render_height);

  const NVSDK_NGX_Result ret =
    SK_FFX_NGX.EvaluateFeature (in.cmd_list, handle, params, nullptr);

  if (! barriers.empty ())
  {
    for ( auto& barrier : barriers )
      std::swap (barrier.Transition.StateBefore, barrier.Transition.StateAfter);

    in.cmd_list->ResourceBarrier (static_cast <UINT> (barriers.size ()), barriers.data ());
  }

  if (NVSDK_NGX_FAILED (ret))
  {
    SK_LOGi0 (L"DLSS Evaluate Failed - %x", ret);
    return SK_FFX_DLSSResult::EvaluateFailed;
  }

  return SK_FFX_DLSSResult::Success;
}

bool
SK_FFX_HasPlugInDLSS (void)
{
  std::error_code ec;

  return
    std::filesystem::exists (
      std::filesystem::path (SK_GetPlugInDirectory (SK_PlugIn_Type::ThirdParty)) /
                               L"NVIDIA" / L"nvngx_dlss.dll", ec );
}

bool
SK_FFX_IsFSRLoaded (void)
{
  static bool loaded = false;

  if (! loaded)
  {
    for ( const wchar_t* wszModule : { L"amd_fidelityfx_dx12.dll",  L"amd_fidelityfx_loader_dx12.dll",
                                       L"ffx_fsr2_api_x64.dll",     L"ffx_fsr3upscaler_x64.dll",
                                       L"ffx_fsr3_x64.dll" } )
    {
      loaded |= (SK_GetModuleHandleW (wszModule) != nullptr);
    }
  }

  return loaded;
}

bool
SK_FFX_IsReplacingFSR (void)
{
  return
    ReadAcquire (&SK_FFX_FeaturesCreated) > 0;
}

void
SK_FFX_Init (void)
{
  // "Use Special K's DLSS" covers FSR games too, using the same plug-in DLL
  if (! config.nvidia.dlss.auto_redirect_dlss)
    return;

  static const bool has_plugin_dlss =
    SK_FFX_HasPlugInDLSS ();

  if (! has_plugin_dlss)
  {
    SK_RunOnce (
      SK_LOGi0 (L"No plug-in nvngx_dlss.dll, FSR will not be replaced.")
    );

    return;
  }

  static SK_Thread_HybridSpinlock init_lock;
  static bool                     api_hooked    = false,
                                  legacy_hooked = false;

  std::scoped_lock lock (init_lock);

  if (! api_hooked)    api_hooked    = SK_FFX_HookAPI    ();
  if (! legacy_hooked) legacy_hooked = SK_FFX_HookLegacy ();
}
