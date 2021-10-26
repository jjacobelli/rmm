/*
 * Copyright (c) 2020-2021, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <rmm/detail/error.hpp>
#include <rmm/mr/device/device_memory_resource.hpp>

#include <cstddef>
#include <functional>

namespace rmm::mr {

/**
 * @brief Callback function type used by failure_callback_resource_adaptor
 *
 * The resource adaptor calls this function when a memory allocation throws a
 * `std::bad_alloc` exception. The function decides whether the resource adaptor
 * should try to allocate the memory again or re-throw the `std::bad_alloc`
 * exception.
 *
 * The callback function signature is:
 *     `bool failure_callback_t(std::size_t bytes, void* callback_arg)`
 *
 * The callback function will be passed two parameters: `bytes` is the size of the
 * failed memory allocation, and `arg` is the extra argument passed to the constructor
 * of the `failure_callback_resource_adaptor`. The callback function returns a Boolean
 * where true means to retry the memory allocation and false means to throw a
 * `rmm::bad_alloc` exception.
 */
using failure_callback_t = std::function<bool(std::size_t, void*)>;

/**
 * @brief A device memory resource that calls a callback function when allocations
 * throws `std::bad_alloc`.
 *
 * An instance of this resource must be constructed with an existing, upstream
 * resource in order to satisfy allocation requests.
 *
 * The callback function takes an allocation size and a callback argument and returns
 * a bool representing whether to retry the allocation (true) or throw `std::bad_alloc`
 * (false).
 *
 * When implementing a callback function for allocation retry, care must be taken to
 * avoid an infinite loop. In the following example, we make sure to only retry the allocation
 * once:
 *
 * @code{c++}
 * using failure_callback_adaptor =
 *   rmm::mr::failure_callback_resource_adaptor<rmm::mr::device_memory_resource>;
 *
 * bool failure_handler(std::size_t bytes, void* arg)
 * {
 *   bool& retried = *reinterpret_cast<bool*>(arg);
 *   if (!retried) {
 *     retried = true;
 *     return true;  // First time we request an allocation retry
 *   } else {
 *     return false;  // Second time we let the adaptor throw std::bad_alloc
 *   }
 * }
 *
 * int main()
 * {
 *   bool retried{false};
 *   failure_callback_adaptor mr{
 *     rmm::mr::get_current_device_resource(), failure_handler, &retried
 *   };
 *   rmm::mr::set_current_device_resource(&mr);
 * }
 * @endcode
 *
 * @tparam Upstream The type of the upstream resource used for allocation/deallocation.
 */
template <typename Upstream>
class failure_callback_resource_adaptor final : public device_memory_resource {
 public:
  /**
   * @brief Construct a new `failure_callback_resource_adaptor` using `upstream` to satisfy
   * allocation requests.
   *
   * @throws `rmm::logic_error` if `upstream == nullptr`
   *
   * @param upstream The resource used for allocating/deallocating device memory
   * @param callback Callback function @see failure_callback_t
   * @param callback_arg Extra argument passed to `callback`
   */
  failure_callback_resource_adaptor(Upstream* upstream,
                                    failure_callback_t callback,
                                    void* callback_arg)
    : upstream_{upstream}, callback_{callback}, callback_arg_{callback_arg}
  {
    RMM_EXPECTS(nullptr != upstream, "Unexpected null upstream resource pointer.");
  }

  failure_callback_resource_adaptor()                                         = delete;
  ~failure_callback_resource_adaptor() override                               = default;
  failure_callback_resource_adaptor(failure_callback_resource_adaptor const&) = delete;
  failure_callback_resource_adaptor& operator=(failure_callback_resource_adaptor const&) = delete;
  failure_callback_resource_adaptor(failure_callback_resource_adaptor&&) noexcept        = default;
  failure_callback_resource_adaptor& operator=(failure_callback_resource_adaptor&&) noexcept =
    default;

  /**
   * @brief Return pointer to the upstream resource.
   *
   * @return Upstream* Pointer to the upstream resource.
   */
  Upstream* get_upstream() const noexcept { return upstream_; }

  /**
   * @brief Checks whether the upstream resource supports streams.
   *
   * @return true The upstream resource supports streams
   * @return false The upstream resource does not support streams.
   */
  bool supports_streams() const noexcept override { return upstream_->supports_streams(); }

  /**
   * @brief Query whether the resource supports the get_mem_info API.
   *
   * @return bool true if the upstream resource supports get_mem_info, false otherwise.
   */
  bool supports_get_mem_info() const noexcept override
  {
    return upstream_->supports_get_mem_info();
  }

 private:
  /**
   * @brief Allocates memory of size at least `bytes` using the upstream
   * resource.
   *
   * @throws `rmm::bad_alloc` if the requested allocation could not be fulfilled
   * by the upstream resource.
   *
   * @param bytes The size, in bytes, of the allocation
   * @param stream Stream on which to perform the allocation
   * @return void* Pointer to the newly allocated memory
   */
  void* do_allocate(std::size_t bytes, cuda_stream_view stream) override
  {
    void* ret;

    while (true) {
      try {
        ret = upstream_->allocate(bytes, stream);
        break;
      } catch (std::bad_alloc const& e) {
        if (!callback_(bytes, callback_arg_)) { throw; }
      }
    }
    return ret;
  }

  /**
   * @brief Free allocation of size `bytes` pointed to by `ptr`
   *
   * @throws Nothing.
   *
   * @param ptr Pointer to be deallocated
   * @param bytes Size of the allocation
   * @param stream Stream on which to perform the deallocation
   */
  void do_deallocate(void* ptr, std::size_t bytes, cuda_stream_view stream) override
  {
    upstream_->deallocate(ptr, bytes, stream);
  }

  /**
   * @brief Compare the upstream resource to another.
   *
   * @throws Nothing.
   *
   * @param other The other resource to compare to
   * @return true If the two resources are equivalent
   * @return false If the two resources are not equal
   */
  bool do_is_equal(device_memory_resource const& other) const noexcept override
  {
    if (this == &other) { return true; }
    auto cast = dynamic_cast<failure_callback_resource_adaptor<Upstream> const*>(&other);
    return cast != nullptr ? upstream_->is_equal(*cast->get_upstream())
                           : upstream_->is_equal(other);
  }

  /**
   * @brief Get free and available memory from upstream resource.
   *
   * @throws `rmm::cuda_error` if unable to retrieve memory info.
   *
   * @param stream Stream on which to get the mem info.
   * @return std::pair contaiing free_size and total_size of memory
   */
  std::pair<std::size_t, std::size_t> do_get_mem_info(cuda_stream_view stream) const override
  {
    return upstream_->get_mem_info(stream);
  }

  Upstream* upstream_;  // the upstream resource used for satisfying allocation requests
  failure_callback_t callback_;
  void* callback_arg_;
};

}  // namespace rmm::mr