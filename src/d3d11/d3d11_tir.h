#pragma once

#include <unordered_map>
#include <vector>

#include "../dxvk/dxvk_shader_ir.h"

#include "../util/thread.h"

namespace dxvk {

  class D3D11CommonShader;
  class D3D11Device;

  /**
   * \brief Where the coverage data lives in the pixel shader's inputs
   *
   * The locations right after the pixel shader's own inputs: the three
   * vertices in fixed point and the edges' top-left flags, then each clip
   * distance at each vertex, over its w, for clipping at every sample.
   */
  struct D3D11TirLayout {
    /// First input location the coverage data occupies
    uint32_t location = 0u;
    /// Clip distances evaluated per sample
    uint32_t clipPlanes = 0u;

    /// Number of locations the coverage data occupies
    uint32_t locationCount() const {
      return 2u + (3u * clipPlanes + 3u) / 4u;
    }

    bool eq(const D3D11TirLayout& other) const {
      return location == other.location
          && clipPlanes == other.clipPlanes;
    }
  };


  /**
   * \brief Output of the stage feeding the rasterizer
   *
   * One output declaration of the vertex or domain shader,
   * which the geometry shader reads and passes on.
   */
  struct D3D11TirVarying {
    dxbc_spv::ir::Type    type;
    dxbc_spv::ir::OpFlags flags;
    dxbc_spv::ir::BuiltIn builtIn = dxbc_spv::ir::BuiltIn::ePosition;
    bool                  isBuiltIn = false;
    uint32_t              location = 0u;
    uint32_t              component = 0u;
  };


  /**
   * \brief Geometry shader for one previous stage and layout
   */
  struct D3D11TirGeometryShader {
    /// The geometry shader, or null if the previous
    /// stage's outputs leave TIR unable to be emulated
    Rc<DxvkShader>  shader;
    /// Layout of the data handed to the pixel shader
    D3D11TirLayout  layout;
  };


  /**
   * \brief Shaders that emulate target-independent rasterization
   *
   * Target-independent rasterization (TIR, D3D11.1) rasterizes at a forced
   * sample count, up to 16, whatever the render targets are, which must be
   * single-sampled: the pixel shader runs once per pixel with any sample of
   * D3D's standard pattern covered, and sees that pattern's coverage as
   * SV_Coverage. Vulkan rasterizes at the attachments' sample count, and
   * only NVIDIA's mixed-samples extension decouples the two. Nor do all
   * GPUs rasterize 16 samples at all: RADV offers 8 at most.
   *
   * So it is emulated, the same on every GPU. The draw is rasterized at
   * one sample, conservatively where the device can, so that every pixel
   * the triangle touches gets a fragment. A geometry shader hands each
   * fragment its triangle in the rasterizer's own fixed point (16.8,
   * snapped round-to-nearest-even), and the pixel shader computes the
   * forced pattern's coverage from it with the top-left rule, exactly as a
   * D3D rasterizer does, clipping at every sample as D3D requires. A pixel
   * with no sample covered is demoted. Draws with no render target bound
   * (UAV-only rendering) are emulated as well, even where Vulkan could
   * rasterize the count without attachments: hardware clips at clip
   * distances geometrically, which moved an edge past a sample by a
   * fraction of a subpixel, where D3D clips each sample.
   *
   * Creates and caches the geometry shader for each stage feeding the
   * rasterizer and the coverage-computing variant of each pixel shader.
   * Thread-safe.
   */
  class D3D11TirShaders {

  public:

    D3D11TirShaders(D3D11Device* pDevice);

    ~D3D11TirShaders();

    /**
     * \brief Checks whether the device can emulate TIR
     *
     * Requires geometry shaders and 64-bit integers, which
     * the edge functions of the 16.8 fixed point need.
     */
    bool IsSupported() const {
      return m_supported;
    }

    /**
     * \brief Geometry shader for the stage feeding the rasterizer
     *
     * \param [in] pPrevStage Vertex or domain shader
     * \param [in] Location First location free of the pixel shader's inputs
     * \param [in] ForwardClip Whether the pixel shader reads clip or cull distances
     * \returns Geometry shader and layout. The shader is null if
     *    the previous stage writes the layer or viewport index, or if
     *    the data does not fit into the remaining locations.
     */
    D3D11TirGeometryShader GetGeometryShader(
      const Rc<DxvkShader>&         pPrevStage,
            uint32_t                Location,
            bool                    ForwardClip);

    /**
     * \brief Coverage-computing variant of a pixel shader
     *
     * \param [in] pShader Pixel shader
     * \param [in] SampleCount Forced sample count
     * \param [in] Layout Layout of the coverage data
     * \returns Pixel shader variant
     */
    Rc<DxvkShader> GetPixelShader(
      const D3D11CommonShader*      pShader,
            UINT                    SampleCount,
      const D3D11TirLayout&         Layout);

  private:

    struct GsKey {
      DxvkShader* prevStage;
      uint32_t    location;
      bool        forwardClip;

      bool eq(const GsKey& other) const {
        return prevStage == other.prevStage
            && location == other.location
            && forwardClip == other.forwardClip;
      }

      size_t hash() const {
        DxvkHashState hash;
        hash.add(reinterpret_cast<uintptr_t>(prevStage));
        hash.add(location);
        hash.add(uint32_t(forwardClip));
        return hash;
      }
    };

    struct GsEntry {
      Rc<DxvkShader>          prevStage;
      D3D11TirGeometryShader  shader;
    };

    struct PsKey {
      DxvkShader*     shader;
      UINT            sampleCount;
      D3D11TirLayout  layout;

      bool eq(const PsKey& other) const {
        return shader == other.shader
            && sampleCount == other.sampleCount
            && layout.eq(other.layout);
      }

      size_t hash() const {
        DxvkHashState hash;
        hash.add(reinterpret_cast<uintptr_t>(shader));
        hash.add(sampleCount);
        hash.add(layout.location);
        hash.add(layout.clipPlanes);
        return hash;
      }
    };

    struct PsEntry {
      Rc<DxvkShader>  base;
      Rc<DxvkShader>  shader;
    };

    D3D11Device*      m_device;
    bool              m_supported = false;
    uint32_t          m_maxLocations = 0u;
    uint32_t          m_maxGsComponents = 0u;
    uint32_t          m_maxGsTotalComponents = 0u;

    dxvk::mutex       m_mutex;

    std::unordered_map<GsKey, GsEntry, DxvkHash, DxvkEq> m_geometryShaders;
    std::unordered_map<PsKey, PsEntry, DxvkHash, DxvkEq> m_pixelShaders;

    D3D11TirGeometryShader CreateGeometryShader(
      const Rc<DxvkShader>&         pPrevStage,
            uint32_t                Location,
            bool                    ForwardClip);

  };

}
