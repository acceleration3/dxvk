#pragma once

#include <unordered_map>

#include "../dxvk/dxvk_shader.h"

#include "../util/thread.h"

namespace dxvk {

  class D3D11Device;

  class D3D11LineCoveragePixelShader : public DxvkShader {

  public:

    D3D11LineCoveragePixelShader(
            Rc<DxvkShader>          pBase,
            uint32_t                Location);

    ~D3D11LineCoveragePixelShader();

    DxvkPipelineLayoutBuilder getLayout();

    DxvkShaderMetadata getShaderMetadata();

    void compile();

    SpirvCodeBuffer getCode(
      const DxvkShaderBindingMap*   bindings,
      const DxvkShaderLinkage*      linkage);

    void dump(std::ostream& outputStream);

    std::string debugName();

  private:

    Rc<DxvkShader>  m_base;
    uint32_t        m_location;

  };


  struct D3D11LineCoverageShaders {
    Rc<DxvkShader>  gs;
    Rc<DxvkShader>  ps;
  };


  class D3D11LineCoverage {

  public:

    D3D11LineCoverage(D3D11Device* pDevice);

    ~D3D11LineCoverage();

    bool IsSupported() const {
      return m_supported;
    }

    D3D11LineCoverageShaders GetShaders(
      const Rc<DxvkShader>&         pPrevStage,
      const Rc<DxvkShader>&         pPixelShader,
            bool                    Adjacency);

  private:

    struct Key {
      DxvkShader* prevStage;
      DxvkShader* pixelShader;
      bool        adjacency;

      bool eq(const Key& other) const {
        return prevStage == other.prevStage
            && pixelShader == other.pixelShader
            && adjacency == other.adjacency;
      }

      size_t hash() const {
        DxvkHashState hash;
        hash.add(reinterpret_cast<uintptr_t>(prevStage));
        hash.add(reinterpret_cast<uintptr_t>(pixelShader));
        hash.add(uint32_t(adjacency));
        return hash;
      }
    };

    struct Entry {
      Rc<DxvkShader>            prevStage;
      Rc<DxvkShader>            pixelShader;
      D3D11LineCoverageShaders  shaders;
    };

    D3D11Device*      m_device;
    bool              m_supported = false;
    uint32_t          m_maxLocations = 0u;
    uint32_t          m_maxGsComponents = 0u;
    uint32_t          m_maxGsTotalComponents = 0u;

    dxvk::mutex       m_mutex;

    std::unordered_map<Key, Entry, DxvkHash, DxvkEq> m_shaders;

    D3D11LineCoverageShaders CreateShaders(
      const Rc<DxvkShader>&         pPrevStage,
      const Rc<DxvkShader>&         pPixelShader,
            bool                    Adjacency);

  };

}
