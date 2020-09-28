#include <ATen/native/vulkan/api/Command.h>
#include <ATen/native/vulkan/api/Adapter.h>

namespace at {
namespace native {
namespace vulkan {
namespace api {
namespace {

VkCommandPool create_command_pool(
    const VkDevice device,
    const uint32_t queue_family_index) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device,
      "Invalid Vulkan device!");

  const VkCommandPoolCreateInfo command_pool_create_info{
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    nullptr,
    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    queue_family_index,
  };

  VkCommandPool command_pool{};
  VK_CHECK(vkCreateCommandPool(
      device,
      &command_pool_create_info,
      nullptr,
      &command_pool));

  TORCH_CHECK(
      command_pool,
      "Invalid Vulkan command pool!");

  return command_pool;
}

VkCommandBuffer allocate_command_buffer(
    const VkDevice device,
    const VkCommandPool command_pool) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device,
      "Invalid Vulkan device!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      command_pool,
      "Invalid Vulkan command pool!");

  const VkCommandBufferAllocateInfo command_buffer_allocate_info{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    nullptr,
    command_pool,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    1u,
  };

  VkCommandBuffer command_buffer{};
  VK_CHECK(vkAllocateCommandBuffers(
      device,
      &command_buffer_allocate_info,
      &command_buffer));

  TORCH_CHECK(
      command_buffer,
      "Invalid Vulkan command buffer!");

  return command_buffer;
}

} // namespace

Command::Buffer::Buffer(
    const VkDevice device,
    const VkCommandPool command_pool)
  : command_buffer_(allocate_command_buffer(device, command_pool)) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      command_buffer_,
      "Invalid Vulkan command buffer!");
}

void Command::Buffer::Buffer::begin() {
  const VkCommandBufferBeginInfo command_buffer_begin_info{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    nullptr,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    nullptr,
  };

  VK_CHECK(vkBeginCommandBuffer(
      command_buffer_,
      &command_buffer_begin_info));
}

void Command::Buffer::Buffer::end() {
  VK_CHECK(vkEndCommandBuffer(command_buffer_));
}

void Command::Buffer::barrier(
    const Pipeline::Barrier& barrier) {
  c10::SmallVector<VkMemoryBarrier, 1u> global_memory_barriers;
  c10::SmallVector<VkImageMemoryBarrier, 1u> image_memory_barriers;

  switch(barrier.type) {
    case Pipeline::Barrier::Type::Execution:
      break;

    case Pipeline::Barrier::Type::Buffer:
      // Using global memory barriers for buffers.  The consensus seems to be
      // that there is no advantage in using the latter.
      global_memory_barriers.push_back({
          VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          nullptr,
          barrier.as.buffer.memory.src,
          barrier.as.buffer.memory.dst,
        });
      break;

    case Pipeline::Barrier::Type::Image:
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          barrier.as.image.handle,
          "Invalid Vulkan image!");


      image_memory_barriers.push_back({
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          nullptr,
          barrier.as.image.memory.src,
          barrier.as.image.memory.dst,
          barrier.as.image.layout.src,
          barrier.as.image.layout.dst,
          VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED,
          barrier.as.image.handle,
          VkImageSubresourceRange{
            VK_IMAGE_ASPECT_COLOR_BIT,
            0u,
            VK_REMAINING_MIP_LEVELS,
            0u,
            VK_REMAINING_ARRAY_LAYERS,
          },
        });
      break;

    default:
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          false,
          "Invalid Vulkan barrier type!");
  };

  vkCmdPipelineBarrier(
      command_buffer_,
      barrier.stage.src,
      barrier.stage.dst,
      0u,
      global_memory_barriers.size(),
      global_memory_barriers.data(),
      0u,
      nullptr,
      image_memory_barriers.size(),
      image_memory_barriers.data());
}

void Command::Buffer::bind(
    const Pipeline::Object pipeline) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      pipeline,
      "Invalid Vulkan pipeline!");

  if (pipeline.handle != bound_.pipeline.handle) {
    vkCmdBindPipeline(
        command_buffer_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline.handle);

    bound_.pipeline = pipeline;
  }
}

void Command::Buffer::bind(
    const Descriptor::Set& set) {
  const VkDescriptorSet descriptor_set = set.handle();

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      descriptor_set,
      "Invalid Vulkan descriptor set!");

  if (descriptor_set != bound_.descriptor_set) {
    vkCmdBindDescriptorSets(
        command_buffer_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        bound_.pipeline.layout,
        0u,
        1u,
        &descriptor_set,
        0u,
        nullptr);

    bound_.descriptor_set = descriptor_set;
  }
}

void Command::Buffer::copy(
    const VkBuffer source,
    const VkBuffer destination,
    const size_t size) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      source,
      "Invalid Vulkan source buffer!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      destination,
      "Invalid Vulkan destination buffer!");

  const VkBufferCopy buffer_copy{
    0u,
    0u,
    size,
  };

  vkCmdCopyBuffer(
      command_buffer_,
      source,
      destination,
      1u,
      &buffer_copy);
}

void Command::Buffer::dispatch(
    const Shader::WorkGroup& work_group) {
  vkCmdDispatch(
      command_buffer_,
      work_group.x,
      work_group.y,
      work_group.z);
}

void Command::Buffer::submit(
    const VkQueue queue,
    const VkFence fence) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      queue,
      "Invalid Vulkan queue!");

  const VkSubmitInfo submit_info{
    VK_STRUCTURE_TYPE_SUBMIT_INFO,
    nullptr,
    0u,
    nullptr,
    nullptr,
    1u,
    &command_buffer_,
    0u,
    nullptr,
  };

  VK_CHECK(vkQueueSubmit(queue, 1u, &submit_info, fence));
}

Command::Pool::Pool(const GPU& gpu)
  : device_(gpu.device),
    command_pool_(
        create_command_pool(gpu.device, gpu.adapter->compute_queue_family_index),
        VK_DELETER(CommandPool)(device_)) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device_,
      "Invalid Vulkan device!");

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      command_pool_,
      "Invalid Vulkan command pool!");
}

Command::Buffer Command::Pool::allocate() {
  return Buffer(device_, command_pool_.get());
}

void Command::Pool::purge() {
  VK_CHECK(vkResetCommandPool(device_, command_pool_.get(), 0u));
}

} // namespace api
} // namespace vulkan
} // namespace native
} // namespace at
