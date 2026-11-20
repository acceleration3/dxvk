#pragma once

#include "../dxvk/dxvk_device.h"

#include "../d3d10/d3d10_rasterizer.h"

#include "d3d11_device_child.h"

namespace dxvk {
  
  class D3D11Device;
  
  class D3D11RasterizerState : public D3D11StateObject<ID3D11RasterizerState2, D3D11RasterizerState> {
    using Container = D3D11StateObjectSet<D3D11RasterizerState>;
  public:
    
    using DescType = D3D11_RASTERIZER_DESC2;
    
    D3D11RasterizerState(
            D3D11Device*                    device,
      const D3D11_RASTERIZER_DESC2&         desc,
            Container*                      container);
    ~D3D11RasterizerState();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) final;
    
    void STDMETHODCALLTYPE GetDesc(
            D3D11_RASTERIZER_DESC* pDesc) final;
    
    void STDMETHODCALLTYPE GetDesc1(
            D3D11_RASTERIZER_DESC1* pDesc) final;
    
    void STDMETHODCALLTYPE GetDesc2(
            D3D11_RASTERIZER_DESC2* pDesc) final;
    
    const D3D11_RASTERIZER_DESC2& Desc() const {
      return m_desc;
    }
    
    DxvkRasterizerState GetState() const {
      return m_state;
    }

    /**
     * \brief State for emulated target-independent rasterization
     *
     * Rasterizes at one sample, conservatively where supported.
     * \returns Rasterizer state for emulated TIR draws
     */
    DxvkRasterizerState GetTirState() const {
      return m_tirState;
    }

    bool EmulatesLineCoverage() const {
      return m_emulatesLineCoverage;
    }

    DxvkRasterizerState GetLineCoverageState() const {
      return m_lineCoverageState;
    }

    DxvkDepthBias GetDepthBias() const {
      return m_depthBias;
    }
    
    D3D10RasterizerState* GetD3D10Iface() {
      return &m_d3d10;
    }
    
    static D3D11_RASTERIZER_DESC2 PromoteDesc(
      const D3D11_RASTERIZER_DESC*  pDesc);
    
    static D3D11_RASTERIZER_DESC2 PromoteDesc(
      const D3D11_RASTERIZER_DESC1* pDesc);
    
    static HRESULT NormalizeDesc(
            D3D11_RASTERIZER_DESC2* pDesc);
    
  private:

    D3D11_RASTERIZER_DESC2 m_desc;
    DxvkRasterizerState    m_state      = { };
    DxvkRasterizerState    m_tirState   = { };
    DxvkRasterizerState    m_lineCoverageState = { };
    bool                   m_emulatesLineCoverage = false;
    DxvkDepthBias          m_depthBias  = { };
    D3D10RasterizerState   m_d3d10;

    D3DDestructionNotifier m_destructionNotifier;
    
  };
  
}
