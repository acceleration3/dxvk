#pragma once

#include <cstdint>

#include "dxvk_include.h"

namespace dxvk {

  /**
   * \brief Embedder's cap on device-local memory
   *
   * Mirrors the embedder's AvVramGate: a table of C callbacks that
   * hands out one account per device. Every device-local allocation
   * takes its size from the account before the Vulkan call and gives it
   * back when the memory is freed, so the sum over all APIs on a GPU
   * stays under the cap the embedder reports to its guest.
   */
  struct DxvkVramGate {
    uint32_t size;
    void*    self;
    void*    (*open)(void* self, const uint8_t* gpuUuid);
    int      (*take)(void* account, uint64_t bytes);
    void     (*give)(void* account, uint64_t bytes);
    void     (*close)(void* self, void* account);
    uint64_t (*freeBytes)(void* account);
    uint64_t (*cap)(void* account);
    int      (*takeHost)(void* account, uint64_t bytes);
    void     (*giveHost)(void* account, uint64_t bytes);
    void     (*relieveBy)(void* self, void* account, void (*relieve)(void* holder), void* holder);
  };

  inline const DxvkVramGate* g_vramGate = nullptr;

  class DxvkVramAccount {

  public:

    DxvkVramAccount() = default;

    explicit DxvkVramAccount(const uint8_t* gpuUuid)
    : m_gate(g_vramGate) {
      if (m_gate && m_gate->size >= sizeof(DxvkVramGate))
        m_account = m_gate->open(m_gate->self, gpuUuid);
    }

    DxvkVramAccount(const DxvkVramAccount&) = delete;
    DxvkVramAccount& operator = (const DxvkVramAccount&) = delete;

    DxvkVramAccount(DxvkVramAccount&& other) noexcept
    : m_gate(other.m_gate), m_account(other.m_account) {
      other.m_account = nullptr;
    }

    DxvkVramAccount& operator = (DxvkVramAccount&& other) noexcept {
      release();
      m_gate = other.m_gate;
      m_account = other.m_account;
      other.m_account = nullptr;
      return *this;
    }

    ~DxvkVramAccount() {
      release();
    }

    bool active() const {
      return m_account != nullptr;
    }

    bool take(VkDeviceSize bytes) const {
      return !m_account || m_gate->take(m_account, bytes);
    }

    void give(VkDeviceSize bytes) const {
      if (m_account)
        m_gate->give(m_account, bytes);
    }

    bool takeHost(VkDeviceSize bytes) const {
      return !m_account || m_gate->takeHost(m_account, bytes);
    }

    void giveHost(VkDeviceSize bytes) const {
      if (m_account)
        m_gate->giveHost(m_account, bytes);
    }

    void relieveBy(void (*relieve)(void* holder), void* holder) const {
      if (m_account)
        m_gate->relieveBy(m_gate->self, m_account, relieve, holder);
    }

  private:

    const DxvkVramGate* m_gate    = nullptr;
    void*               m_account = nullptr;

    void release() {
      if (m_account)
        m_gate->close(m_gate->self, m_account);

      m_account = nullptr;
    }

  };

}
