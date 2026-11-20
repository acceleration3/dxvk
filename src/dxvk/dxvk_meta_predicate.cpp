#include "dxvk_device.h"
#include "dxvk_meta_predicate.h"

#include <dxvk_predicate_resolve.h>

namespace dxvk {

  DxvkMetaPredicateObjects::DxvkMetaPredicateObjects(DxvkDevice* device)
  : m_device(device) {
    m_pipeline.layout = m_device->createBuiltInPipelineLayout(0u,
      VK_SHADER_STAGE_COMPUTE_BIT, sizeof(DxvkMetaPredicatePushConstants), 0u, nullptr);

    util::DxvkBuiltInShaderStage shader(dxvk_predicate_resolve, nullptr);
    m_pipeline.pipeline = m_device->createBuiltInComputePipeline(m_pipeline.layout, shader);
  }


  DxvkMetaPredicateObjects::~DxvkMetaPredicateObjects() {
    auto vk = m_device->vkd();
    vk->vkDestroyPipeline(vk->device(), m_pipeline.pipeline, nullptr);
  }

}
