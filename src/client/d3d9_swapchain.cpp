/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "pch.h"
#include "d3d9_lss.h"
#include "d3d9_swapchain.h"
#include "d3d9_surface.h"
#include "d3d9_surfacebuffer_helper.h"

/*
 * Direct3DSwapChain9_LSS Interface Implementation
 */

HRESULT Direct3DSwapChain9_LSS::QueryInterface(REFIID riid, LPVOID* ppvObj) {
  LogFunctionCall();
  if (ppvObj == nullptr) {
    return E_POINTER;
  }

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown)
    || riid == __uuidof(IDirect3DSwapChain9)) {
    *ppvObj = bridge_cast<IDirect3DSwapChain9*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG Direct3DSwapChain9_LSS::AddRef() {
  LogFunctionCall();
  return Direct3DContainer9_LSS::AddRef();
}

ULONG Direct3DSwapChain9_LSS::Release() {
  LogFunctionCall();
  return Direct3DContainer9_LSS::Release();
}

void Direct3DSwapChain9_LSS::onDestroy() {
  BRIDGE_PARENT_DEVICE_LOCKGUARD();
  ClientMessage c(Commands::IDirect3DSwapChain9_Destroy, getId());
}

HRESULT Direct3DSwapChain9_LSS::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                        HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion,
                                        DWORD dwFlags) {
  ZoneScoped;
  LogFunctionCall();
#ifdef ENABLE_PRESENT_SEMAPHORE_TRACE
  Logger::trace(format_string("Present(): ClientMessage counter is at %d.", ClientMessage::get_counter()));
#endif
  ClientMessage::reset_counter();
  gSceneState = WaitBeginScene;

  // If the bridge was disabled in the meantime for some reason we want to bail
  // out here so we don't spend time waiting on the Present semaphore or trying
  // to send keyboard state to the server.
  if (!gbBridgeRunning) {
    return D3D_OK;
  }

  // Send present first
  {
    BRIDGE_PARENT_DEVICE_LOCKGUARD();
    ClientMessage c(Commands::IDirect3DSwapChain9_Present, getId());
    c.send_data(sizeof(RECT), (void*) pSourceRect);
    c.send_data(sizeof(RECT), (void*) pDestRect);
    c.send_data((uint32_t) hDestWindowOverride);
    c.send_data(sizeof(RGNDATA), (void*) pDirtyRegion);
    c.send_data(dwFlags);
  }

  // Seeing this in the log could indicate the game is sending inputs to a different window
  extern std::unordered_map<HWND, WNDPROC> ogWndProc;
  if (hDestWindowOverride != NULL && ogWndProc.count(hDestWindowOverride) == 0)
    ONCE(Logger::info("Detected unhooked winproc on Direct3DSwapChain9::Present"));

  extern HRESULT syncOnPresent();
  const auto syncResult = syncOnPresent();
  if (syncResult == ERROR_SEM_TIMEOUT) {
    return ERROR_SEM_TIMEOUT;
  }

  FrameMark;

  return D3D_OK;
}

HRESULT Direct3DSwapChain9_LSS::GetFrontBufferData(IDirect3DSurface9* pDestSurface) {
  LogFunctionCall();

  const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);
  const auto pIDestinationSurface = pLssDestinationSurface->D3D<IDirect3DSurface9>();

  BRIDGE_PARENT_DEVICE_LOCKGUARD();

  {
    ClientMessage c(Commands::IDirect3DSwapChain9_GetFrontBufferData, getId());
    c.send_data((uint32_t) pIDestinationSurface);
  }

  return copyServerSurfaceRawData(pLssDestinationSurface);
}

HRESULT Direct3DSwapChain9_LSS::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                              IDirect3DSurface9** ppBackBuffer) {
  ZoneScoped;
  LogFunctionCall();

  if (ppBackBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_PARENT_DEVICE_LOCKGUARD();
    if (auto surface = getChild(iBackBuffer)) {
      surface->AddRef();
      *ppBackBuffer = surface;
      return D3D_OK;
    }

    // Insert our own IDirect3DSurface9 interface implementation
    D3DSURFACE_DESC desc;
    desc.Width = m_presParam.BackBufferWidth;
    desc.Height = m_presParam.BackBufferHeight;
    desc.MultiSampleQuality = m_presParam.MultiSampleQuality;
    desc.MultiSampleType = m_presParam.MultiSampleType;
    desc.Format = m_presParam.BackBufferFormat;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    auto* const pLssSurface = trackWrapper(new Direct3DSurface9_LSS(m_pDevice, this, desc));
    setChild(iBackBuffer, pLssSurface);

    (*ppBackBuffer) = pLssSurface;

    // Add handles for backbuffer
    {
      ClientMessage c(Commands::IDirect3DSwapChain9_GetBackBuffer, getId());
      c.send_data(iBackBuffer);
      c.send_data(Type);
      c.send_data(pLssSurface->getId());
    }
  }

  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetBackBuffer()", D3DERR_INVALIDCALL);
}

HRESULT Direct3DSwapChain9_LSS::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
  LogMissingFunctionCall();
  return D3D_OK;
}

HRESULT Direct3DSwapChain9_LSS::GetDisplayMode(D3DDISPLAYMODE* pMode) {
  LogFunctionCall();
  if (pMode == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  return m_pDevice->GetDisplayMode(0, pMode);
}

HRESULT Direct3DSwapChain9_LSS::GetDevice(IDirect3DDevice9** ppDevice) {
  LogFunctionCall();
  if (ppDevice == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  m_pDevice->AddRef();
  (*ppDevice) = m_pDevice;
  return D3D_OK;
}

HRESULT Direct3DSwapChain9_LSS::GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) {
  LogFunctionCall();
  if (pPresentationParameters == nullptr)
    return D3DERR_INVALIDCALL;
  *pPresentationParameters = m_presParam;
  return D3D_OK;
}
