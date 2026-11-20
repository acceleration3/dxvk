#pragma once

#include "dxvk_include.h"
#include "dxvk_pipelayout.h"

namespace dxvk {

  class DxvkDevice;

  struct DxvkMetaPredicatePushConstants {
    VkDeviceAddress srcVa = 0u;
    VkDeviceAddress dstVa = 0u;
    uint32_t        count = 0u;
    uint32_t        mode  = 0u;
  };


  struct DxvkMetaPredicatePipeline {
    const DxvkPipelineLayout* layout   = nullptr;
    VkPipeline                pipeline = VK_NULL_HANDLE;
  };


  class DxvkMetaPredicateObjects {

  public:

    constexpr static uint32_t ModeAnySamples = 0u;
    constexpr static uint32_t ModeOverflow   = 1u;

    DxvkMetaPredicateObjects(DxvkDevice* device);
    ~DxvkMetaPredicateObjects();

    DxvkMetaPredicatePipeline getPipeline() const {
      return m_pipeline;
    }

  private:

    DxvkDevice* m_device = nullptr;

    DxvkMetaPredicatePipeline m_pipeline;

  };

}
