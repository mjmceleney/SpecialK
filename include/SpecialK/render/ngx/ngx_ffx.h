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

#pragma once

//
// Shared DLSS backend for the FidelityFX -> DLSS translation layers:
//
//   ngx_ffx_api.cpp    - FidelityFX API (FSR 3.1+, amd_fidelityfx_dx12.dll)
//   ngx_ffx_legacy.cpp - FSR 2.x (ffx_fsr2_api_x64.dll) and FSR 3.0 upscaler
//

#include <render/ngx/ngx_defs.h>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

// Every FSR generation shares these creation flag bits
enum SK_FFX_UpscaleFlags : uint32_t {
  SK_FFX_ENABLE_HIGH_DYNAMIC_RANGE                 = (1 << 0),
  SK_FFX_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS  = (1 << 1),
  SK_FFX_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),
  SK_FFX_ENABLE_DEPTH_INVERTED                     = (1 << 3),
  SK_FFX_ENABLE_DEPTH_INFINITE                     = (1 << 4),
  SK_FFX_ENABLE_AUTO_EXPOSURE                      = (1 << 5),
  SK_FFX_ENABLE_DYNAMIC_RESOLUTION                 = (1 << 6),
};

struct SK_FFX_DLSSInputs
{
  ID3D12GraphicsCommandList* cmd_list       = nullptr;

  ID3D12Resource*            color          = nullptr;
  ID3D12Resource*            depth          = nullptr;
  ID3D12Resource*            motion_vectors = nullptr;
  ID3D12Resource*            exposure       = nullptr;
  ID3D12Resource*            output         = nullptr;

  // Current state of each resource; DLSS needs inputs readable by compute
  //   and the output as a UAV, transitions are added around the evaluation.
  D3D12_RESOURCE_STATES      color_state    = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES      depth_state    = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES      mv_state       = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES      exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES      output_state   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  float                      jitter_x         = 0.0f, jitter_y   = 0.0f;
  float                      mv_scale_x       = 1.0f, mv_scale_y = 1.0f;
  uint32_t                   render_width     = 0,    render_height  = 0;
  uint32_t                   upscale_width    = 0,    upscale_height = 0;
  float                      frame_time_delta = 0.0f;
  float                      pre_exposure     = 1.0f;
  bool                       reset            = false;
};

enum class SK_FFX_DLSSResult {
  Success,
  CreateFailed,  // Caller should fall back to FSR
  EvaluateFailed
};

struct SK_FFX_DLSSFeature
{
  NVSDK_NGX_Parameter* params     = nullptr;
  NVSDK_NGX_Handle*    handle     = nullptr;
  uint32_t             in_width   = 0, in_height  = 0;
  uint32_t             out_width  = 0, out_height = 0;

  // Creates (or re-creates) the feature on the game's command list as needed.
  //   NGX must already be initialized on the command list's device.
  SK_FFX_DLSSResult evaluate ( uint32_t                 ffx_flags,
                               uint32_t                 max_render_width,
                               uint32_t                 max_render_height,
                               const SK_FFX_DLSSInputs& in );
  void              release  (void);
};

// Loads/initializes NGX for a device, returns true if DLSS-SR is usable
bool  SK_FFX_InitNGX                    (ID3D12Device* pDevice);
bool  SK_FFX_InitNGX                    (ID3D12GraphicsCommandList* pCmdList);

// False once NGX was tried and DLSS turned out unavailable
bool  SK_FFX_ShouldOverrideStaticQueries (void);

// FSR's quality-mode ratios, adjusted by Special K's DLSS scale / DLAA settings.
//   Quality mode values (NativeAA=0 .. UltraPerformance=4) match in every FSR version.
float SK_FFX_GetUpscaleRatio            (uint32_t qualityMode);
float SK_FFX_Halton                     (int index, int base);

// Status for the control panel
bool  SK_FFX_HasPlugInDLSS              (void); // PlugIns\ThirdParty\NVIDIA\nvngx_dlss.dll exists
bool  SK_FFX_IsFSRLoaded                (void); // Game loaded a FidelityFX upscaler DLL we can hook
bool  SK_FFX_IsReplacingFSR             (void); // DLSS has run in place of FSR this session

// Front ends, return true once hooked
bool  SK_FFX_HookAPI                    (void);
bool  SK_FFX_HookLegacy                 (void);
