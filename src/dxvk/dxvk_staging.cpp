#include "dxvk_device.h"
#include "dxvk_staging.h"

namespace dxvk {
  
  DxvkStagingBuffer::DxvkStagingBuffer(
    const Rc<DxvkDevice>&     device,
          VkDeviceSize        size)
  : m_device(device), m_offset(0), m_size(size) {

  }


  DxvkStagingBuffer::~DxvkStagingBuffer() {

  }


  DxvkBufferSlice DxvkStagingBuffer::alloc(VkDeviceSize size) {
    DxvkBufferCreateInfo info;
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
                | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT
                | VK_ACCESS_SHADER_READ_BIT;
    info.debugName = "Staging buffer";

    VkDeviceSize alignedSize = dxvk::align(size, 256u);
    m_allocationCounter += alignedSize;

    if (2 * alignedSize > m_size) {
      Rc<DxvkBuffer> large = tryCreateBuffer(info);
      return large != nullptr ? DxvkBufferSlice(std::move(large)) : DxvkBufferSlice();
    }

    if (m_offset + alignedSize > m_size || m_buffer == nullptr) {
      info.size = m_size;

      // Free resources first if possible, in some rare
      // situations this may help avoid a memory allocation.
      m_buffer = nullptr;
      m_buffer = tryCreateBuffer(info);
      m_offset = 0;

      if (m_buffer == nullptr)
        return DxvkBufferSlice();
    }

    DxvkBufferSlice slice(m_buffer, m_offset, size);
    m_offset += alignedSize;
    return slice;
  }


  Rc<DxvkBuffer> DxvkStagingBuffer::tryCreateBuffer(
    const DxvkBufferCreateInfo& info) {
    try {
      return m_device->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } catch (const DxvkOutOfMemoryError& e) {
      Logger::err(e.message());
      return nullptr;
    }
  }


  void DxvkStagingBuffer::reset() {
    m_buffer = nullptr;
    m_offset = 0;

    m_allocationCounterValueOnReset = m_allocationCounter;
  }
  
}
