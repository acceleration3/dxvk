#include "../util/av_line_coverage.h"

#include "../dxvk/dxvk_shader_spirv.h"

#include "d3d11_device.h"
#include "d3d11_line_coverage.h"

namespace dxvk {

  namespace {

    uint32_t NextLocation(const DxvkShaderIo& io) {
      uint32_t next = 0u;

      for (uint32_t i = 0u; i < io.getVarCount(); i++) {
        auto var = io.getVar(i);

        if (var.builtIn == spv::BuiltInMax && !var.isPatchConstant)
          next = std::max(next, uint32_t(var.location) + 1u);
      }

      return next;
    }

  }


  D3D11LineCoveragePixelShader::D3D11LineCoveragePixelShader(
          Rc<DxvkShader>          pBase,
          uint32_t                Location)
  : m_base(std::move(pBase)), m_location(Location) {

  }


  D3D11LineCoveragePixelShader::~D3D11LineCoveragePixelShader() {

  }


  DxvkPipelineLayoutBuilder D3D11LineCoveragePixelShader::getLayout() {
    return m_base->getLayout();
  }


  DxvkShaderMetadata D3D11LineCoveragePixelShader::getShaderMetadata() {
    DxvkShaderMetadata metadata = m_base->metadata();

    DxvkShaderIoVar coverage = { };
    coverage.location = uint8_t(m_location);
    coverage.componentCount = 1u;
    metadata.inputs.add(coverage);
    return metadata;
  }


  void D3D11LineCoveragePixelShader::compile() {
    m_base->compile();
  }


  SpirvCodeBuffer D3D11LineCoveragePixelShader::getCode(
    const DxvkShaderBindingMap*   bindings,
    const DxvkShaderLinkage*      linkage) {
    SpirvCodeBuffer code = m_base->getCode(bindings, linkage);

    uint32_t* words = nullptr;
    size_t count = 0u;

    int status = av_lc_patch_fragment_shader(code.data(), code.dwords(), m_location, &words, &count);

    if (status == AV_LC_UNCHANGED)
      return code;

    if (status != AV_LC_OK)
      throw DxvkError(str::format("D3D11: ", m_base->debugName(), ": line coverage not applied to the pixel shader: ", status));

    SpirvCodeBuffer patched(uint32_t(count), words);
    free(words);
    return patched;
  }


  void D3D11LineCoveragePixelShader::dump(std::ostream& outputStream) {
    auto code = getCode(nullptr, nullptr);
    outputStream.write(reinterpret_cast<const char*>(code.data()), code.size());
  }


  std::string D3D11LineCoveragePixelShader::debugName() {
    return str::format(m_base->debugName(), "_LC", m_location);
  }


  D3D11LineCoverage::D3D11LineCoverage(D3D11Device* pDevice)
  : m_device(pDevice) {
    auto dxvkDevice = pDevice->GetDXVKDevice();

    const auto& features = dxvkDevice->features();
    const auto& limits = dxvkDevice->properties().core.properties.limits;

    m_supported = features.core.features.geometryShader
               && !features.extLineRasterization.smoothLines;

    m_maxLocations = std::min(limits.maxFragmentInputComponents,
                              limits.maxGeometryOutputComponents) / 4u;
    m_maxGsComponents = limits.maxGeometryOutputComponents;
    m_maxGsTotalComponents = limits.maxGeometryTotalOutputComponents;
  }


  D3D11LineCoverage::~D3D11LineCoverage() {

  }


  D3D11LineCoverageShaders D3D11LineCoverage::GetShaders(
    const Rc<DxvkShader>&         pPrevStage,
    const Rc<DxvkShader>&         pPixelShader,
          bool                    Adjacency) {
    std::lock_guard lock(m_mutex);

    Key key = { pPrevStage.ptr(), pPixelShader.ptr(), Adjacency };
    auto entry = m_shaders.find(key);

    if (entry != m_shaders.end())
      return entry->second.shaders;

    Entry value = { pPrevStage, pPixelShader, CreateShaders(pPrevStage, pPixelShader, Adjacency) };
    m_shaders.insert({ key, value });
    return value.shaders;
  }


  D3D11LineCoverageShaders D3D11LineCoverage::CreateShaders(
    const Rc<DxvkShader>&         pPrevStage,
    const Rc<DxvkShader>&         pPixelShader,
          bool                    Adjacency) {
    D3D11LineCoverageShaders result = { };

    const auto& prevMeta = pPrevStage->metadata();

    if (prevMeta.flags.any(DxvkShaderFlag::ExportsLayer, DxvkShaderFlag::ExportsViewport,
        DxvkShaderFlag::HasTransformFeedback, DxvkShaderFlag::SemanticIo)) {
      Logger::warn(str::format("D3D11: ", pPrevStage->debugName(),
        " writes the layer, the viewport index or stream output: antialiased lines drawn aliased"));
      return result;
    }

    uint32_t location = std::max(NextLocation(prevMeta.outputs),
                                 NextLocation(pPixelShader->metadata().inputs));

    std::array<av_lc_varying, 32u> varyings = { };
    uint32_t found = 0u;

    SpirvCodeBuffer prevCode = pPrevStage->getCode(nullptr, nullptr);

    int status = av_lc_reflect(prevCode.data(), prevCode.dwords(), AV_LC_SC_OUTPUT,
      varyings.data(), varyings.size(), &found);

    uint32_t components = av_lc_output_components(varyings.data(), found);

    if (status == AV_LC_OK && (location >= m_maxLocations
     || components > m_maxGsComponents || 4u * components > m_maxGsTotalComponents))
      status = AV_LC_TOO_MANY;

    uint32_t* words = nullptr;
    size_t count = 0u;

    if (status == AV_LC_OK)
      status = av_lc_build_geometry_shader(varyings.data(), found, Adjacency, location, 8u, &words, &count);

    if (status != AV_LC_OK) {
      Logger::warn(str::format("D3D11: ", pPrevStage->debugName(),
        ": no line coverage geometry stage (", status, "): antialiased lines drawn aliased"));
      return result;
    }

    DxvkSpirvShaderCreateInfo info;
    info.localPushData = DxvkPushDataBlock(0u, 16u, 4u, 0u);
    info.debugName = str::format(pPrevStage->debugName(), "_LCGS", Adjacency ? "_ADJ" : "", "_L", location);

    result.gs = new DxvkSpirvShader(info, SpirvCodeBuffer(uint32_t(count), words));
    free(words);

    result.ps = new D3D11LineCoveragePixelShader(pPixelShader, location);

    m_device->GetDXVKDevice()->registerShader(result.gs);
    m_device->GetDXVKDevice()->registerShader(result.ps);
    return result;
  }

}
