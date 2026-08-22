// GpuImageUploader.cpp
#include "render/GpuImageUploader.hpp"
#include "render/upload/DrmFormat.hpp"
#include "render/upload/YuvConstants.hpp"
#include "codec_motion_expand.spv.h"
#include "bicubic_prefilter.spv.h"
#include "drm_yuv_to_fsr_input.spv.h"
#include "easu.spv.h"
#include "util/Log.hpp"
#include "yuv_to_fsr_input.spv.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libdrm/drm_fourcc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace temporal_forge {

bool GpuImageUploader::init(VkPhysicalDevice physical, VkDevice device,
                            VkQueue queue, uint32_t queueFamily,
                            uint32_t presentationQueueFamily) {
  physical_ = physical;
  device_ = device;
  queue_ = queue;
  queueFamily_ = queueFamily;
  presentationQueueFamily_ = presentationQueueFamily;
  if (device_ == VK_NULL_HANDLE)
    return false;
  VkFormatProperties easuFormatProperties{};
  vkGetPhysicalDeviceFormatProperties(
      physical_, VK_FORMAT_R8G8B8A8_UNORM, &easuFormatProperties);
  logInfo("GpuImageUploader: EASU RGBA8 format features optimal=0x{:x}",
          easuFormatProperties.optimalTilingFeatures);

  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queueFamily_;
  if (vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_) != VK_SUCCESS) {
    logError("GpuImageUploader: createCommandPool failed");
    return false;
  }
  // Command buffer is allocated lazily in beginUploadCmd().
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) {
    return false;
  }

  std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dci.bindingCount = static_cast<uint32_t>(bindings.size());
  dci.pBindings = bindings.data();
  if (vkCreateDescriptorSetLayout(device_, &dci, nullptr, &yuvDescLayout_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create yuv descriptor layout failed");
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 4> drmBindings{};
  drmBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  drmBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  drmBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  drmBindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo drmdci{};
  drmdci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  drmdci.bindingCount = static_cast<uint32_t>(drmBindings.size());
  drmdci.pBindings = drmBindings.data();
  if (vkCreateDescriptorSetLayout(device_, &drmdci, nullptr, &drmDescLayout_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create drm descriptor layout failed");
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 3> motionBindings{};
  motionBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  motionBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  motionBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo motionDci{};
  motionDci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  motionDci.bindingCount = static_cast<uint32_t>(motionBindings.size());
  motionDci.pBindings = motionBindings.data();
  if (vkCreateDescriptorSetLayout(device_, &motionDci, nullptr,
                                  &motionDescLayout_) != VK_SUCCESS) {
    logError("GpuImageUploader: create motion descriptor layout failed");
    return false;
  }
  std::array<VkDescriptorPoolSize, 1> poolSizes{};
  poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16};
  std::array<VkDescriptorPoolSize, 2> drmPoolSizes{};
  drmPoolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
  drmPoolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8};
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 16;
  std::array<VkDescriptorPoolSize, 3> allPools{poolSizes[0], drmPoolSizes[0],
                                               drmPoolSizes[1]};
  dpci.poolSizeCount = static_cast<uint32_t>(allPools.size());
  dpci.pPoolSizes = allPools.data();
  if (vkCreateDescriptorPool(device_, &dpci, nullptr, &yuvDescPool_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create yuv descriptor pool failed");
    return false;
  }
  if (vkCreateDescriptorPool(device_, &dpci, nullptr, &drmDescPool_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create drm descriptor pool failed");
    return false;
  }
  std::array<VkDescriptorPoolSize, 2> motionPoolSizes{{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
  }};
  VkDescriptorPoolCreateInfo motionDpci{};
  motionDpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  motionDpci.maxSets = 4;
  motionDpci.poolSizeCount = static_cast<uint32_t>(motionPoolSizes.size());
  motionDpci.pPoolSizes = motionPoolSizes.data();
  if (vkCreateDescriptorPool(device_, &motionDpci, nullptr, &motionDescPool_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create motion descriptor pool failed");
    return false;
  }
  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(YuvPushConstants);
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &yuvDescLayout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &pl, nullptr, &yuvPipelineLayout_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create yuv pipeline layout failed");
    return false;
  }
  VkPipelineLayoutCreateInfo drmpl{};
  drmpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  drmpl.setLayoutCount = 1;
  drmpl.pSetLayouts = &drmDescLayout_;
  drmpl.pushConstantRangeCount = 1;
  drmpl.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &drmpl, nullptr, &drmPipelineLayout_) !=
      VK_SUCCESS) {
    logError("GpuImageUploader: create drm pipeline layout failed");
    return false;
  }
  VkPushConstantRange motionPcr{};
  motionPcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  motionPcr.offset = 0;
  motionPcr.size = sizeof(uint32_t) * 5;
  VkPipelineLayoutCreateInfo motionPl{};
  motionPl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  motionPl.setLayoutCount = 1;
  motionPl.pSetLayouts = &motionDescLayout_;
  motionPl.pushConstantRangeCount = 1;
  motionPl.pPushConstantRanges = &motionPcr;
  if (vkCreatePipelineLayout(device_, &motionPl, nullptr,
                             &motionPipelineLayout_) != VK_SUCCESS) {
    logError("GpuImageUploader: create motion pipeline layout failed");
    return false;
  }
  VkShaderModuleCreateInfo sm{};
  sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sm.codeSize = kyuv_to_fsr_input_spv_words * sizeof(uint32_t);
  sm.pCode = kyuv_to_fsr_input_spv;
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &sm, nullptr, &mod) != VK_SUCCESS) {
    logError("GpuImageUploader: create yuv shader module failed");
    return false;
  }
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = mod;
  cpci.stage.pName = "main";
  cpci.layout = yuvPipelineLayout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                               &yuvPipeline_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, mod, nullptr);
    logError("GpuImageUploader: create yuv pipeline failed");
    return false;
  }
  VkShaderModuleCreateInfo drmSm{};
  drmSm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  drmSm.codeSize = kdrm_yuv_to_fsr_input_spv_words * sizeof(uint32_t);
  drmSm.pCode = kdrm_yuv_to_fsr_input_spv;
  VkShaderModule drmMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &drmSm, nullptr, &drmMod) != VK_SUCCESS) {
    logError("GpuImageUploader: create drm shader module failed");
    return false;
  }
  VkComputePipelineCreateInfo drmCpci{};
  drmCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  drmCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drmCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  drmCpci.stage.module = drmMod;
  drmCpci.stage.pName = "main";
  drmCpci.layout = drmPipelineLayout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &drmCpci, nullptr,
                               &drmPipeline_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, drmMod, nullptr);
    logError("GpuImageUploader: create drm pipeline failed");
    return false;
  }
  VkShaderModuleCreateInfo motionSm{};
  motionSm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  motionSm.codeSize = kcodec_motion_expand_spv_words * sizeof(uint32_t);
  motionSm.pCode = kcodec_motion_expand_spv;
  VkShaderModule motionMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &motionSm, nullptr, &motionMod) !=
      VK_SUCCESS) {
    vkDestroyShaderModule(device_, drmMod, nullptr);
    vkDestroyShaderModule(device_, mod, nullptr);
    logError("GpuImageUploader: create motion shader module failed");
    return false;
  }
  VkComputePipelineCreateInfo motionCpci{};
  motionCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  motionCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  motionCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  motionCpci.stage.module = motionMod;
  motionCpci.stage.pName = "main";
  motionCpci.layout = motionPipelineLayout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &motionCpci, nullptr,
                               &motionPipeline_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, motionMod, nullptr);
    vkDestroyShaderModule(device_, drmMod, nullptr);
    vkDestroyShaderModule(device_, mod, nullptr);
    logError("GpuImageUploader: create motion pipeline failed");
    return false;
  }
  vkDestroyShaderModule(device_, motionMod, nullptr);
  vkDestroyShaderModule(device_, drmMod, nullptr);
  vkDestroyShaderModule(device_, mod, nullptr);

  // --- GPU bicubic prefilter ---
  std::array<VkDescriptorSetLayoutBinding, 2> prefilterBindings{};
  prefilterBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                          VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  prefilterBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                          VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo prefilterDci{};
  prefilterDci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  prefilterDci.bindingCount = static_cast<uint32_t>(prefilterBindings.size());
  prefilterDci.pBindings = prefilterBindings.data();
  if (vkCreateDescriptorSetLayout(device_, &prefilterDci, nullptr,
                                  &prefilterDescLayout_) != VK_SUCCESS)
    return false;
  VkPushConstantRange prefilterPcr{};
  prefilterPcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  prefilterPcr.size = sizeof(uint32_t) * 5;
  VkPipelineLayoutCreateInfo prefilterPl{};
  prefilterPl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  prefilterPl.setLayoutCount = 1;
  prefilterPl.pSetLayouts = &prefilterDescLayout_;
  prefilterPl.pushConstantRangeCount = 1;
  prefilterPl.pPushConstantRanges = &prefilterPcr;
  if (vkCreatePipelineLayout(device_, &prefilterPl, nullptr,
                             &prefilterPipelineLayout_) != VK_SUCCESS)
    return false;
  VkShaderModuleCreateInfo prefilterSm{};
  prefilterSm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  prefilterSm.codeSize = kbicubic_prefilter_spv_words * sizeof(uint32_t);
  prefilterSm.pCode = kbicubic_prefilter_spv;
  VkShaderModule prefilterMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &prefilterSm, nullptr, &prefilterMod) !=
      VK_SUCCESS)
    return false;
  VkComputePipelineCreateInfo prefilterCpci{};
  prefilterCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  prefilterCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  prefilterCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  prefilterCpci.stage.module = prefilterMod;
  prefilterCpci.stage.pName = "main";
  prefilterCpci.layout = prefilterPipelineLayout_;
  const VkResult prefilterResult = vkCreateComputePipelines(
      device_, VK_NULL_HANDLE, 1, &prefilterCpci, nullptr,
      &prefilterPipeline_);
  vkDestroyShaderModule(device_, prefilterMod, nullptr);
  if (prefilterResult != VK_SUCCESS)
    return false;
  std::array<VkDescriptorPoolSize, 1> prefilterPoolSizes{{
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}}};
  VkDescriptorPoolCreateInfo prefilterDpci{};
  prefilterDpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  prefilterDpci.maxSets = 1;
  prefilterDpci.poolSizeCount = 1;
  prefilterDpci.pPoolSizes = prefilterPoolSizes.data();
  if (vkCreateDescriptorPool(device_, &prefilterDpci, nullptr,
                             &prefilterDescPool_) != VK_SUCCESS)
    return false;
  VkDescriptorSetAllocateInfo prefilterSai{};
  prefilterSai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  prefilterSai.descriptorPool = prefilterDescPool_;
  prefilterSai.descriptorSetCount = 1;
  prefilterSai.pSetLayouts = &prefilterDescLayout_;
  if (vkAllocateDescriptorSets(device_, &prefilterSai, &prefilterSet_) !=
      VK_SUCCESS)
    return false;

  // --- EASU pipeline (FSR1 edge-adaptive spatial upscale) ---
  // Descriptor layout: binding 0 = src image (rgba8 readonly), binding 1 = dst
  // image (rgba8 writeonly). Push constant = uvec4(srcW, srcH, dstW, dstH).
  std::array<VkDescriptorSetLayoutBinding, 2> easuBindings{};
  easuBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  easuBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo easuDci{};
  easuDci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  easuDci.bindingCount = static_cast<uint32_t>(easuBindings.size());
  easuDci.pBindings = easuBindings.data();
  if (vkCreateDescriptorSetLayout(device_, &easuDci, nullptr,
                                  &easuDescLayout_) != VK_SUCCESS) {
    logError("GpuImageUploader: create easu descriptor layout failed");
    return false;
  }
  VkPushConstantRange easuPcr{};
  easuPcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  easuPcr.offset = 0;
  easuPcr.size = sizeof(uint32_t) * 5;
  VkPipelineLayoutCreateInfo easuPl{};
  easuPl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  easuPl.setLayoutCount = 1;
  easuPl.pSetLayouts = &easuDescLayout_;
  easuPl.pushConstantRangeCount = 1;
  easuPl.pPushConstantRanges = &easuPcr;
  if (vkCreatePipelineLayout(device_, &easuPl, nullptr,
                             &easuPipelineLayout_) != VK_SUCCESS) {
    logError("GpuImageUploader: create easu pipeline layout failed");
    return false;
  }
  VkShaderModuleCreateInfo easuSm{};
  easuSm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  easuSm.codeSize = keasu_spv_words * sizeof(uint32_t);
  easuSm.pCode = keasu_spv;
  VkShaderModule easuMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &easuSm, nullptr, &easuMod) != VK_SUCCESS) {
    logError("GpuImageUploader: create easu shader module failed");
    return false;
  }
  VkComputePipelineCreateInfo easuCpci{};
  easuCpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  easuCpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  easuCpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  easuCpci.stage.module = easuMod;
  easuCpci.stage.pName = "main";
  easuCpci.layout = easuPipelineLayout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &easuCpci, nullptr,
                               &easuPipeline_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, easuMod, nullptr);
    logError("GpuImageUploader: create easu pipeline failed");
    return false;
  }
  vkDestroyShaderModule(device_, easuMod, nullptr);
  // EASU descriptor pool + set (one persistent set; re-bound per allocate).
  std::array<VkDescriptorPoolSize, 1> easuPoolSizes{};
  easuPoolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
  VkDescriptorPoolCreateInfo easuDpci{};
  easuDpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  easuDpci.maxSets = 2;
  easuDpci.poolSizeCount = static_cast<uint32_t>(easuPoolSizes.size());
  easuDpci.pPoolSizes = easuPoolSizes.data();
  if (vkCreateDescriptorPool(device_, &easuDpci, nullptr,
                             &easuDescPool_) != VK_SUCCESS) {
    logError("GpuImageUploader: create easu descriptor pool failed");
    return false;
  }
  VkDescriptorSetAllocateInfo easuSai{};
  easuSai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  easuSai.descriptorPool = easuDescPool_;
  easuSai.descriptorSetCount = 1;
  easuSai.pSetLayouts = &easuDescLayout_;
  const VkResult easuSetResult =
      vkAllocateDescriptorSets(device_, &easuSai, &easuSet_);
  if (easuSetResult != VK_SUCCESS) {
    logError("GpuImageUploader: allocate easu descriptor set failed ({})",
             static_cast<int>(easuSetResult));
    easuSet_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void GpuImageUploader::destroy() {
  if (device_ == VK_NULL_HANDLE)
    return;
  if (yuvPipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, yuvPipeline_, nullptr);
    yuvPipeline_ = VK_NULL_HANDLE;
  }
  if (drmPipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, drmPipeline_, nullptr);
    drmPipeline_ = VK_NULL_HANDLE;
  }
  if (motionPipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, motionPipeline_, nullptr);
    motionPipeline_ = VK_NULL_HANDLE;
  }
  if (prefilterPipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, prefilterPipeline_, nullptr);
    prefilterPipeline_ = VK_NULL_HANDLE;
  }
  if (easuPipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, easuPipeline_, nullptr);
    easuPipeline_ = VK_NULL_HANDLE;
  }
  if (yuvPipelineLayout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, yuvPipelineLayout_, nullptr);
    yuvPipelineLayout_ = VK_NULL_HANDLE;
  }
  if (drmPipelineLayout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, drmPipelineLayout_, nullptr);
    drmPipelineLayout_ = VK_NULL_HANDLE;
  }
  if (motionPipelineLayout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, motionPipelineLayout_, nullptr);
    motionPipelineLayout_ = VK_NULL_HANDLE;
  }
  if (prefilterPipelineLayout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, prefilterPipelineLayout_, nullptr);
    prefilterPipelineLayout_ = VK_NULL_HANDLE;
  }
  if (easuPipelineLayout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, easuPipelineLayout_, nullptr);
    easuPipelineLayout_ = VK_NULL_HANDLE;
  }
  if (yuvDescPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, yuvDescPool_, nullptr);
    yuvDescPool_ = VK_NULL_HANDLE;
  }
  if (drmDescPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, drmDescPool_, nullptr);
    drmDescPool_ = VK_NULL_HANDLE;
  }
  if (motionDescPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, motionDescPool_, nullptr);
    motionDescPool_ = VK_NULL_HANDLE;
  }
  if (prefilterDescPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, prefilterDescPool_, nullptr);
    prefilterDescPool_ = VK_NULL_HANDLE;
    prefilterSet_ = VK_NULL_HANDLE;
  }
  if (easuDescPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, easuDescPool_, nullptr);
    easuDescPool_ = VK_NULL_HANDLE;
    easuSet_ = VK_NULL_HANDLE;
  }
  if (yuvDescLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, yuvDescLayout_, nullptr);
    yuvDescLayout_ = VK_NULL_HANDLE;
  }
  if (drmDescLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, drmDescLayout_, nullptr);
    drmDescLayout_ = VK_NULL_HANDLE;
  }
  if (motionDescLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, motionDescLayout_, nullptr);
    motionDescLayout_ = VK_NULL_HANDLE;
  }
  if (prefilterDescLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, prefilterDescLayout_, nullptr);
    prefilterDescLayout_ = VK_NULL_HANDLE;
  }
  if (easuDescLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, easuDescLayout_, nullptr);
    easuDescLayout_ = VK_NULL_HANDLE;
  }
  destroyGpuImage(device_, easuImage_);
  destroyImportedDrmRuntime(drmRt_);
  destroyGpuImage(device_, sourceModel_);
  destroyGpuImage(device_, color_);
  destroyGpuImage(device_, rawPresentation_);
  destroyGpuImage(device_, yPlane_);
  destroyGpuImage(device_, uPlane_);
  destroyGpuImage(device_, vPlane_);
  destroyGpuImage(device_, motion_);
  destroyGpuImage(device_, depth_);
  destroyGpuImage(device_, reactive_);
  destroyGpuImage(device_, tcMask_);
  destroyGpuImage(device_, exposure_);
  destroyGpuImage(device_, output_);
  destroyGpuImage(device_, presentation_);
  destroyGpuImage(device_, presentationRetained_);
  destroyGpuImage(device_, history_[0]);
  destroyGpuImage(device_, history_[1]);
  destroyGpuImage(device_, recurrent_[0]);
  destroyGpuImage(device_, recurrent_[1]);
  if (stagingMapped_) {
    vkUnmapMemory(device_, staging_.memory);
    stagingMapped_ = nullptr;
  }
  destroyGpuBufferObj(device_, staging_);
  if (swsColor_) {
    sws_freeContext(swsColor_);
    swsColor_ = nullptr;
  }
  swsW_ = swsH_ = 0;
  swsFormat_ = -1;
  yuv420Scratch_.clear();
  if (motionVectorsMapped_) {
    vkUnmapMemory(device_, motionVectors_.memory);
    motionVectorsMapped_ = nullptr;
  }
  destroyGpuBufferObj(device_, motionVectors_);
  destroyGpuBufferObj(device_, motionOwners_);
  if (fence_ != VK_NULL_HANDLE) {
    vkDestroyFence(device_, fence_, nullptr);
    fence_ = VK_NULL_HANDLE;
  }
  if (cmdPool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device_, cmdPool_, nullptr);
    cmdPool_ = VK_NULL_HANDLE;
  }
  cmd_ = VK_NULL_HANDLE;
  srcW_ = srcH_ = modelW_ = modelH_ = outW_ = outH_ = 0;
  rgbaScratch_.clear();
  rgbaSharpScratch_.clear();
  lumaScratch_.clear();
}

bool GpuImageUploader::allocate(uint32_t sourceW, uint32_t sourceH,
                                uint32_t outputW, uint32_t outputH,
                                uint32_t modelW, uint32_t modelH) {
  if (modelW == 0) modelW = sourceW;
  if (modelH == 0) modelH = sourceH;
  if (sourceW == srcW_ && sourceH == srcH_ && outputW == outW_ &&
      outputH == outH_ && modelW == modelW_ && modelH == modelH_)
    return true; // no change
  if (sourceW == 0 || sourceH == 0 || outputW == 0 || outputH == 0)
    return false;

  destroyGpuImage(device_, sourceModel_);
  destroyGpuImage(device_, color_);
  destroyGpuImage(device_, rawPresentation_);
  destroyGpuImage(device_, yPlane_);
  destroyGpuImage(device_, uPlane_);
  destroyGpuImage(device_, vPlane_);
  destroyGpuImage(device_, motion_);
  destroyGpuImage(device_, depth_);
  destroyGpuImage(device_, reactive_);
  destroyGpuImage(device_, tcMask_);
  destroyGpuImage(device_, exposure_);
  destroyGpuImage(device_, output_);
  destroyGpuImage(device_, presentation_);
  destroyGpuImage(device_, presentationRetained_);
  destroyGpuImage(device_, history_[0]);
  destroyGpuImage(device_, history_[1]);
  destroyGpuImage(device_, recurrent_[0]);
  destroyGpuImage(device_, recurrent_[1]);
  destroyGpuImage(device_, easuImage_);
  destroyGpuBufferObj(device_, motionOwners_);

  const VkImageUsageFlags inputUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags outputUsage = VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;

  bool ok = true;
  ok &= createGpuImage(device_, physical_, sourceW, sourceH, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, yPlane_, "fsr4_y_plane");
  const uint32_t chromaW = std::max(1u, (sourceW + 1u) / 2u);
  const uint32_t chromaH = std::max(1u, (sourceH + 1u) / 2u);
  ok &= createGpuImage(device_, physical_, chromaW, chromaH, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, uPlane_, "fsr4_u_plane");
  ok &= createGpuImage(device_, physical_, chromaW, chromaH, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, vPlane_, "fsr4_v_plane");
  ok &= createGpuImage(device_, physical_, sourceW, sourceH,
                       VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, sourceModel_,
                       "fsr4_source_model");
  ok &= createGpuImage(device_, physical_, modelW, modelH,
                       VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, color_, "fsr4_color");
  ok &= createGpuImage(
      device_, physical_, sourceW, sourceH, VK_FORMAT_R8G8B8A8_UNORM,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, rawPresentation_, "fsr4_raw_present",
      queueFamily_, presentationQueueFamily_);
  // EASU 2x intermediate: same format as color_ (rgb10_a2) so the FSR4
  // prepass can consume it directly. Allocated at 2x native dimensions.
  const uint32_t easuW = sourceW * 2u;
  const uint32_t easuH = sourceH * 2u;
  ok &= createGpuImage(device_, physical_, easuW, easuH,
                       VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, easuImage_, "fsr4_easu",
                       queueFamily_, presentationQueueFamily_);
  ok &= createGpuImage(device_, physical_, modelW, modelH,
                       VK_FORMAT_R16G16_SFLOAT, inputUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, motion_, "fsr4_motion");
  ok &= createGpuImage(device_, physical_, modelW, modelH,
                       VK_FORMAT_R32_SFLOAT, inputUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, depth_, "fsr4_depth");
  ok &= createGpuImage(device_, physical_, modelW, modelH, VK_FORMAT_R8_UNORM,
                       inputUsage, VK_IMAGE_ASPECT_COLOR_BIT, reactive_,
                       "fsr4_reactive");
  ok &= createGpuImage(device_, physical_, modelW, modelH, VK_FORMAT_R8_UNORM,
                       inputUsage, VK_IMAGE_ASPECT_COLOR_BIT, tcMask_,
                       "fsr4_tcmask");
  // Exposure is 1x1.
  ok &=
      createGpuImage(device_, physical_, 1, 1, VK_FORMAT_R16_SFLOAT, inputUsage,
                     VK_IMAGE_ASPECT_COLOR_BIT, exposure_, "fsr4_exposure");
  // Output + history use the requested target dimensions. The postpass
  // performs the source-to-target RGB resolve.
  ok &= createGpuImage(device_, physical_, outputW, outputH,
                       VK_FORMAT_R8G8B8A8_UNORM, outputUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, output_, "fsr4_output",
                       queueFamily_, presentationQueueFamily_);
  ok &= createGpuImage(
      device_, physical_, outputW, outputH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      outputUsage, VK_IMAGE_ASPECT_COLOR_BIT, history_[0], "fsr4_history_a");
  ok &= createGpuImage(
      device_, physical_, outputW, outputH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      outputUsage, VK_IMAGE_ASPECT_COLOR_BIT, history_[1], "fsr4_history_b");
  ok &= createGpuImage(device_, physical_, outputW, outputH,
                       VK_FORMAT_R16G16B16A16_SFLOAT, outputUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, recurrent_[0],
                       "fsr4_recurrent_a");
  ok &= createGpuImage(device_, physical_, outputW, outputH,
                       VK_FORMAT_R16G16B16A16_SFLOAT, outputUsage,
                       VK_IMAGE_ASPECT_COLOR_BIT, recurrent_[1],
                       "fsr4_recurrent_b");
  ok &= createGpuBufferObj(
      device_, physical_,
      static_cast<VkDeviceSize>(sourceW) * sourceH * sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, motionOwners_, "fsr4_motion_owners");
  historyIndex_.store(0, std::memory_order_release);
  if (!ok) {
    logError("GpuImageUploader: failed to allocate images for {}x{} -> {}x{}",
             sourceW, sourceH, outputW, outputH);
    return false;
  }
  srcW_ = sourceW;
  srcH_ = sourceH;
  modelW_ = modelW;
  modelH_ = modelH;
  outW_ = outputW;
  outH_ = outputH;
  const size_t rgba8Size = (size_t)srcW_ * srcH_ * 4;
  rgbaScratch_.reserve(rgba8Size);
  rgbaSharpScratch_.reserve(rgba8Size);
  lumaScratch_.reserve((size_t)modelW_ * modelH_);
  logInfo("GpuImageUploader: allocated source {}x{} -> model {}x{} -> output {}x{}",
          sourceW, sourceH, modelW, modelH, outputW, outputH);
  return true;
}

bool GpuImageUploader::ensureStaging(VkDeviceSize size) {
  if (staging_.buffer != VK_NULL_HANDLE && staging_.size >= size)
    return true;
  if (stagingMapped_) {
    vkUnmapMemory(device_, staging_.memory);
    stagingMapped_ = nullptr;
  }
  destroyGpuBufferObj(device_, staging_);
  if (!createGpuBufferObj(device_, physical_, size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          staging_, "uploader_staging")) {
    return false;
  }
  return vkMapMemory(device_, staging_.memory, 0, staging_.size, 0,
                     &stagingMapped_) == VK_SUCCESS;
}

bool GpuImageUploader::ensureMotionVectorBuffer(size_t vectorCount) {
  constexpr VkDeviceSize stride = sizeof(uint32_t) * 8;
  const VkDeviceSize required =
      std::max<VkDeviceSize>(stride, vectorCount * stride);
  if (motionVectors_.buffer != VK_NULL_HANDLE &&
      motionVectors_.size >= required) {
    return true;
  }
  if (motionVectorsMapped_) {
    vkUnmapMemory(device_, motionVectors_.memory);
    motionVectorsMapped_ = nullptr;
  }
  destroyGpuBufferObj(device_, motionVectors_);
  if (!createGpuBufferObj(device_, physical_, required,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          motionVectors_, "fsr4_motion_vectors")) {
    return false;
  }
  return vkMapMemory(device_, motionVectors_.memory, 0, motionVectors_.size, 0,
                     &motionVectorsMapped_) == VK_SUCCESS;
}

bool GpuImageUploader::beginUploadCmd() {
  if (frameUploadBatch_)
    return true;
  if (cmd_ == VK_NULL_HANDLE) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cai, &cmd_) != VK_SUCCESS)
      return false;
  }
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(cmd_, &bi) == VK_SUCCESS;
}

bool GpuImageUploader::endUploadCmd() {
  if (frameUploadBatch_)
    return true;
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS)
    return false;
  return submitAndWait(device_, queue_, cmd_, fence_);
}

bool GpuImageUploader::beginFrameUploads(bool allowBatch) {
  static const bool forceCpuMotion =
      std::getenv("TFORGE_FSR4_CPU_MOTION") != nullptr;
  if (frameUploadBatch_ || deferredFrameUploads_)
    return false;
  if (!allowBatch || forceCpuMotion)
    return true;
  if (!beginUploadCmd())
    return false;
  frameUploadBatch_ = true;
  return true;
}

bool GpuImageUploader::endFrameUploads(VkCommandBuffer *deferredCmd) {
  if (deferredCmd)
    *deferredCmd = VK_NULL_HANDLE;
  if (frameUploadBatch_) {
    frameUploadBatch_ = false;
    if (deferredCmd) {
      // This command buffer is submitted immediately before the FSR dispatch
      // buffer.  Make the final conversion/prefilter writes visible to the
      // dispatch command buffer; command-buffer order is not itself a shader
      // memory dependency.
      const VkImage producedImage = colorImage();
      if (producedImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier producedBarrier{};
        producedBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        producedBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        producedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        producedBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        producedBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        producedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        producedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        producedBarrier.image = producedImage;
        producedBarrier.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(
            cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
            1, &producedBarrier);
      }
      if (vkEndCommandBuffer(cmd_) != VK_SUCCESS)
        return false;
      deferredFrameUploads_ = true;
      *deferredCmd = cmd_;
      return true;
    }
    const bool ok = endUploadCmd();
    if (resetYuvDescriptorsAfterBatch_) {
      vkResetDescriptorPool(device_, yuvDescPool_, 0);
      resetYuvDescriptorsAfterBatch_ = false;
    }
    if (resetDrmDescriptorsAfterBatch_) {
      vkResetDescriptorPool(device_, drmDescPool_, 0);
      resetDrmDescriptorsAfterBatch_ = false;
    }
    if (resetMotionDescriptorsAfterBatch_) {
      vkResetDescriptorPool(device_, motionDescPool_, 0);
      resetMotionDescriptorsAfterBatch_ = false;
    }
    return ok;
  }
  return true;
}

void GpuImageUploader::completeDeferredFrameUploads() {
  if (!deferredFrameUploads_)
    return;
  deferredFrameUploads_ = false;
  if (resetYuvDescriptorsAfterBatch_) {
    vkResetDescriptorPool(device_, yuvDescPool_, 0);
    resetYuvDescriptorsAfterBatch_ = false;
  }
  if (resetDrmDescriptorsAfterBatch_) {
    vkResetDescriptorPool(device_, drmDescPool_, 0);
    resetDrmDescriptorsAfterBatch_ = false;
  }
  if (resetMotionDescriptorsAfterBatch_) {
    vkResetDescriptorPool(device_, motionDescPool_, 0);
    resetMotionDescriptorsAfterBatch_ = false;
  }
}

bool GpuImageUploader::copyBufferToImage(GpuImage &image, VkBuffer staging,
                                         VkDeviceSize offset, uint32_t width,
                                         uint32_t height,
                                         VkImageAspectFlags aspect) {
  // Inputs are reused every frame and return to GENERAL after compute.
  VkImageMemoryBarrier b1{};
  b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b1.srcAccessMask =
      image.layout == VK_IMAGE_LAYOUT_UNDEFINED
          ? 0
          : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b1.oldLayout = image.layout;
  b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.image = image.image;
  b1.subresourceRange = {aspect, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_,
                       image.layout == VK_IMAGE_LAYOUT_UNDEFINED
                           ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                           : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b1);

  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {aspect, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd_, staging, image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // TRANSFER_DST_OPTIMAL → GENERAL (so shaders can read/write)
  VkImageMemoryBarrier b2{};
  b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.image = image.image;
  b2.subresourceRange = {aspect, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b2);
  image.layout = VK_IMAGE_LAYOUT_GENERAL;
  return true;
}

bool GpuImageUploader::uploadColor(const DecodedVideoFrame &frame) {
  return uploadColorTo(frame, color_);
}

bool GpuImageUploader::dispatchBicubicPrefilter(const DecodedVideoFrame &frame) {
  if (prefilterPipeline_ == VK_NULL_HANDLE || prefilterSet_ == VK_NULL_HANDLE ||
      rawPresentation_.image == VK_NULL_HANDLE || color_.image == VK_NULL_HANDLE)
    return false;
  if (!beginUploadCmd())
    return false;

  if (sourceModel_.layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier sourceBarrier{};
    sourceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceBarrier.srcAccessMask = sourceModel_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                                      ? 0
                                      : VK_ACCESS_SHADER_WRITE_BIT;
    sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceBarrier.oldLayout = sourceModel_.layout;
    sourceBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.image = sourceModel_.image;
    sourceBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        cmd_, sourceModel_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                  ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &sourceBarrier);
    sourceModel_.layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  if (color_.layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = color_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = color_.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_,
                         color_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                             : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    color_.layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  VkDescriptorImageInfo sourceInfo{VK_NULL_HANDLE, sourceModel_.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo destinationInfo{VK_NULL_HANDLE, color_.view,
                                        VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = prefilterSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  writes[0].pImageInfo = &sourceInfo;
  writes[1].pImageInfo = &destinationInfo;
  vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
  const uint32_t transfer =
      frame.colorTransfer == AVCOL_TRC_SMPTE2084
          ? 1u
          : frame.colorTransfer == AVCOL_TRC_ARIB_STD_B67 ? 2u : 0u;
  const uint32_t push[5] = {srcW_, srcH_, modelW_, modelH_, transfer};
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          prefilterPipelineLayout_, 0, 1, &prefilterSet_, 0,
                          nullptr);
  vkCmdPushConstants(cmd_, prefilterPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(push), push);
  vkCmdDispatch(cmd_, (modelW_ + 15u) / 16u, (modelH_ + 15u) / 16u, 1);
  VkMemoryBarrier memory{};
  memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0,
                       nullptr, 0, nullptr);
  return true;
}

bool GpuImageUploader::dispatchYuvConvert(const DecodedVideoFrame &frame,
                                          bool compareEnabled,
                                          float sharpness) {
  if (yuvPipeline_ == VK_NULL_HANDLE)
    return false;

  for (GpuImage *image : {&sourceModel_, &rawPresentation_, &color_}) {
    if (image->layout == VK_IMAGE_LAYOUT_GENERAL)
      continue;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = image->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = image->layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_,
                         image->layout == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                             : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo asi{};
  asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  asi.descriptorPool = yuvDescPool_;
  asi.descriptorSetCount = 1;
  asi.pSetLayouts = &yuvDescLayout_;
  if (vkAllocateDescriptorSets(device_, &asi, &set) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorImageInfo yInfo{VK_NULL_HANDLE, yPlane_.view,
                              VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo uInfo{VK_NULL_HANDLE, uPlane_.view,
                              VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo vInfo{VK_NULL_HANDLE, vPlane_.view,
                              VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo colorInfo{
      VK_NULL_HANDLE,
      (modelW_ == srcW_ && modelH_ == srcH_) ? color_.view : sourceModel_.view,
      VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, rawPresentation_.view,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[5]{};
  w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = set;
  w[0].dstBinding = 0;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[0].descriptorCount = 1;
  w[0].pImageInfo = &yInfo;
  w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[1].dstSet = set;
  w[1].dstBinding = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[1].descriptorCount = 1;
  w[1].pImageInfo = &uInfo;
  w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[2].dstSet = set;
  w[2].dstBinding = 2;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[2].descriptorCount = 1;
  w[2].pImageInfo = &vInfo;
  w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[3].dstSet = set;
  w[3].dstBinding = 3;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[3].descriptorCount = 1;
  w[3].pImageInfo = &colorInfo;
  w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[4].dstSet = set;
  w[4].dstBinding = 4;
  w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[4].descriptorCount = 1;
  w[4].pImageInfo = &rawInfo;
  vkUpdateDescriptorSets(device_, 5, w, 0, nullptr);

  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, yuvPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          yuvPipelineLayout_, 0, 1, &set, 0, nullptr);
  const YuvPushConstants push =
      yuvPushConstants(frame, compareEnabled, sharpness);
  vkCmdPushConstants(cmd_, yuvPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), &push);
  const uint32_t gx = (static_cast<uint32_t>(frame.width) + 15u) / 16u;
  const uint32_t gy = (static_cast<uint32_t>(frame.height) + 15u) / 16u;
  vkCmdDispatch(cmd_, gx, gy, 1);

  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                       nullptr, 0, nullptr);
  return true;
}

bool GpuImageUploader::importDrmPrimeFrame(const DecodedVideoFrame &frame,
                                           ImportedDrmFrame &imported) {
  imported = {};
  if (!frame.hwFrame || frame.hwFrameFormat != AV_PIX_FMT_DRM_PRIME ||
      frame.drmObjects <= 0) {
    return false;
  }
  imported.valid = true;
  const bool splitNv12 =
      frame.drmLayers >= 2 && frame.drmLayerPlaneCount[0] == 1 &&
      frame.drmLayerPlaneCount[1] == 1 &&
      frame.drmLayerFourcc[0] == DRM_FORMAT_R8 &&
      (frame.drmLayerFourcc[1] == DRM_FORMAT_GR88 ||
       frame.drmLayerFourcc[1] == DRM_FORMAT_RG88);
  imported.fourcc = splitNv12 ? DRM_FORMAT_NV12 : frame.drmFourcc;
  imported.layers = frame.drmLayers;
  imported.planes = splitNv12 ? 2 : frame.drmPlanes;
  imported.objects = frame.drmObjects;
  imported.width = frame.width;
  imported.height = frame.height;
  imported.colorRange = frame.colorRange;
  imported.colorSpace = frame.colorSpace;
  imported.modifier = frame.drmObject[0].formatModifier;
  imported.fd = frame.drmObject[0].fd;
  for (int i = 0; i < imported.planes && i < 4; ++i) {
    const int layer = splitNv12 ? i : 0;
    const int plane = splitNv12 ? 0 : i;
    imported.planeLayouts[i].offset = frame.drmLayerPlane[layer][plane].offset;
    imported.planeLayouts[i].rowPitch = frame.drmLayerPlane[layer][plane].pitch;
  }
  return true;
}

bool GpuImageUploader::createImportedDrmRuntime(const ImportedDrmFrame &frame,
                                                ImportedDrmRuntime &rt) {
  destroyImportedDrmRuntime(rt);
  static bool loggedFailure = false;
  auto logFailure = [&](const char *operation, VkResult result,
                        uint32_t memoryTypeBits = 0) {
    if (loggedFailure)
      return;
    loggedFailure = true;
    TFORGE_LOG_ERROR(
        "DRM PRIME import failed at {}: VkResult={} size={}x{} fourcc={} "
        "modifier={} planes={} layout0=({}, {}) layout1=({}, {}) "
        "memoryTypeBits={}",
        operation, static_cast<int>(result), frame.width, frame.height,
        frame.fourcc, frame.modifier, frame.planes,
        frame.planeLayouts[0].offset, frame.planeLayouts[0].rowPitch,
        frame.planeLayouts[1].offset, frame.planeLayouts[1].rowPitch,
        memoryTypeBits);
  };
  if (frame.fd < 0 || frame.width <= 0 || frame.height <= 0) {
    logFailure("input validation", VK_ERROR_INITIALIZATION_FAILED);
    return false;
  }
  const VkFormat vkFmt = drmFourccToVkFormat(
      frame.fourcc, frame.fourcc == DRM_FORMAT_P010 ||
                        frame.fourcc == DRM_FORMAT_YUV420_10BIT);
  if (vkFmt == VK_FORMAT_UNDEFINED) {
    logFailure("DRM fourcc conversion", VK_ERROR_FORMAT_NOT_SUPPORTED);
    return false;
  }

  int importedFd = -1;
  auto cleanup = [&]() {
    if (importedFd >= 0) {
      close(importedFd);
      importedFd = -1;
    }
    destroyImportedDrmRuntime(rt);
    return false;
  };

  VkExternalMemoryImageCreateInfo emici{};
  emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageDrmFormatModifierExplicitCreateInfoEXT drmInfo{};
  drmInfo.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  drmInfo.drmFormatModifier = frame.modifier;
  drmInfo.drmFormatModifierPlaneCount = static_cast<uint32_t>(frame.planes);
  drmInfo.pPlaneLayouts = frame.planeLayouts;
  drmInfo.pNext = &emici;

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.pNext = &drmInfo;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = vkFmt;
  ici.extent = {static_cast<uint32_t>(frame.width),
                static_cast<uint32_t>(frame.height), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  const VkResult imageResult = vkCreateImage(device_, &ici, nullptr, &rt.image);
  if (imageResult != VK_SUCCESS) {
    logFailure("vkCreateImage", imageResult);
    return cleanup();
  }

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, rt.image, &req);
  VkMemoryDedicatedAllocateInfo dedi{};
  dedi.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedi.image = rt.image;

  VkImportMemoryFdInfoKHR importFd{};
  importFd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  importFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  importedFd = dup(frame.fd);
  if (importedFd < 0) {
    logFailure("dup", VK_ERROR_INVALID_EXTERNAL_HANDLE, req.memoryTypeBits);
    return cleanup();
  }
  importFd.fd = importedFd;
  importFd.pNext = &dedi;

  uint32_t memType = findMemoryType(physical_, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memType == ~0u) {
    logFailure("findMemoryType", VK_ERROR_FEATURE_NOT_PRESENT,
               req.memoryTypeBits);
    return cleanup();
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &importFd;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memType;
  const VkResult allocateResult =
      vkAllocateMemory(device_, &mai, nullptr, &rt.memory);
  if (allocateResult != VK_SUCCESS) {
    logFailure("vkAllocateMemory", allocateResult, req.memoryTypeBits);
    return cleanup();
  }
  importedFd = -1;
  const VkResult bindResult = vkBindImageMemory(device_, rt.image, rt.memory, 0);
  if (bindResult != VK_SUCCESS) {
    logFailure("vkBindImageMemory", bindResult, req.memoryTypeBits);
    return cleanup();
  }

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = VK_FILTER_NEAREST;
  sci.minFilter = VK_FILTER_NEAREST;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.anisotropyEnable = VK_FALSE;
  sci.compareEnable = VK_FALSE;
  sci.unnormalizedCoordinates = VK_FALSE;
  const VkResult samplerResult =
      vkCreateSampler(device_, &sci, nullptr, &rt.sampler);
  if (samplerResult != VK_SUCCESS) {
    logFailure("vkCreateSampler", samplerResult, req.memoryTypeBits);
    return cleanup();
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = rt.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  const bool tenBit = vkFmt == VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
  vci.format = tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
  vci.subresourceRange.levelCount = 1;
  vci.subresourceRange.layerCount = 1;
  const VkResult yViewResult =
      vkCreateImageView(device_, &vci, nullptr, &rt.yView);
  if (yViewResult != VK_SUCCESS) {
    logFailure("vkCreateImageView(y)", yViewResult, req.memoryTypeBits);
    return cleanup();
  }
  vci.format = tenBit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
  const VkResult uvViewResult =
      vkCreateImageView(device_, &vci, nullptr, &rt.uvView);
  if (uvViewResult != VK_SUCCESS) {
    logFailure("vkCreateImageView(uv)", uvViewResult, req.memoryTypeBits);
    return cleanup();
  }
  return true;
}

void GpuImageUploader::destroyImportedDrmRuntime(ImportedDrmRuntime &rt) {
  if (device_ == VK_NULL_HANDLE)
    return;
  if (rt.yView != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rt.yView, nullptr);
    rt.yView = VK_NULL_HANDLE;
  }
  if (rt.uvView != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rt.uvView, nullptr);
    rt.uvView = VK_NULL_HANDLE;
  }
  if (rt.sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, rt.sampler, nullptr);
    rt.sampler = VK_NULL_HANDLE;
  }
  if (rt.image != VK_NULL_HANDLE) {
    vkDestroyImage(device_, rt.image, nullptr);
    rt.image = VK_NULL_HANDLE;
  }
  if (rt.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device_, rt.memory, nullptr);
    rt.memory = VK_NULL_HANDLE;
  }
}

bool GpuImageUploader::importAndConvertDrmFrame(
    const DecodedVideoFrame &frame) {
  static bool loggedImportMetadata = false;
  if (!loggedImportMetadata) {
    loggedImportMetadata = true;
    TFORGE_LOG_INFO(
        "GpuImageUploader: DRM frame hw={} format={} objects={} layers={} "
        "planes={} layerPlanes=[{},{}] fourcc=[{},{}]",
        frame.hwFrame, frame.hwFrameFormat, frame.drmObjects, frame.drmLayers,
        frame.drmPlanes, frame.drmLayerPlaneCount[0],
        frame.drmLayerPlaneCount[1], frame.drmLayerFourcc[0],
        frame.drmLayerFourcc[1]);
  }
  ImportedDrmFrame imported{};
  if (!importDrmPrimeFrame(frame, imported)) {
    TFORGE_LOG_ERROR("GpuImageUploader: DRM descriptor extraction failed");
    return false;
  }
  if (!drmFrameMatchesLinearNv12(frame)) {
    TFORGE_LOG_ERROR("GpuImageUploader: unsupported DRM layer layout");
    return false;
  }
  if (!createImportedDrmRuntime(imported, drmRt_))
    return false;
  if (!beginUploadCmd())
    return false;

  // The DRM conversion writes all three destinations.  The old path only
  // transitioned color_, leaving rawPresentation_ (the EASU source) and the
  // optional sourceModel_ prefilter target in UNDEFINED layout.  That made
  // the neural color path appear valid while the spatial presentation path
  // produced black output on VAAPI/DRM frames.
  for (GpuImage *image : {&color_, &rawPresentation_, &sourceModel_}) {
    if (image->layout == VK_IMAGE_LAYOUT_GENERAL)
      continue;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = image->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = image->layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        cmd_, image->layout == VK_IMAGE_LAYOUT_UNDEFINED
                  ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  auto fail = [&]() {
    vkEndCommandBuffer(cmd_);
    return false;
  };

  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo asi{};
  asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  asi.descriptorPool = drmDescPool_;
  asi.descriptorSetCount = 1;
  asi.pSetLayouts = &drmDescLayout_;
  if (vkAllocateDescriptorSets(device_, &asi, &set) != VK_SUCCESS)
    return fail();

  VkDescriptorImageInfo yInfo{drmRt_.sampler, drmRt_.yView,
                              VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo uvInfo{drmRt_.sampler, drmRt_.uvView,
                               VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo colorInfo{
      VK_NULL_HANDLE,
      (modelW_ == srcW_ && modelH_ == srcH_) ? color_.view : sourceModel_.view,
      VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, rawPresentation_.view,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[4]{};
  w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = set;
  w[0].dstBinding = 0;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[0].descriptorCount = 1;
  w[0].pImageInfo = &yInfo;
  w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[1].dstSet = set;
  w[1].dstBinding = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[1].descriptorCount = 1;
  w[1].pImageInfo = &uvInfo;
  w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[2].dstSet = set;
  w[2].dstBinding = 2;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[2].descriptorCount = 1;
  w[2].pImageInfo = &colorInfo;
  w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[3].dstSet = set;
  w[3].dstBinding = 3;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[3].descriptorCount = 1;
  w[3].pImageInfo = &rawInfo;
  vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);

  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, drmPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          drmPipelineLayout_, 0, 1, &set, 0, nullptr);
  const YuvPushConstants push = yuvPushConstants(
      frame, compareEnabled_.load(std::memory_order_acquire),
      std::clamp(sharpness_.load(std::memory_order_acquire), 0.0f, 1.0f));
  vkCmdPushConstants(cmd_, drmPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), &push);
  const uint32_t gx = (frame.width + 15u) / 16u;
  const uint32_t gy = (frame.height + 15u) / 16u;
  vkCmdDispatch(cmd_, gx, gy, 1);
  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                       nullptr, 0, nullptr);
  return true;
}

bool GpuImageUploader::uploadColorTo(const DecodedVideoFrame &frame,
                                     GpuImage &image) {
  if (srcW_ == 0 || frame.width != (int)srcW_ || frame.height != (int)srcH_)
    return false;

  const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame.avFormat);
  const bool planar420 =
      fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P ||
      fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_YUV420P12LE;
  const float sharpness =
      std::clamp(sharpness_.load(std::memory_order_acquire), 0.0f, 1.0f);

  if (frame.hwFrame) {
    if (importAndConvertDrmFrame(frame)) {
      if ((modelW_ != srcW_ || modelH_ != srcH_) &&
          !dispatchBicubicPrefilter(frame))
        return false;
      if (!endUploadCmd())
        return false;
      if (frameUploadBatch_) {
        resetDrmDescriptorsAfterBatch_ = true;
      } else if (drmDescPool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device_, drmDescPool_, 0);
      }
      return true;
    }
    destroyImportedDrmRuntime(drmRt_);
  }

  // The GPU conversion shader consumes planar 4:2:0. Normalize other
  // software-decoded formats here instead of dropping their frames upstream.
  // This covers RGB, 4:2:2/4:4:4, and software 10/12-bit formats supported by
  // the installed libswscale build.
  if (!planar420) {
    if (frame.planes <= 0 || !frame.plane[0].data() ||
        !av_pix_fmt_desc_get(fmt)) {
      logWarn("GpuImageUploader: cannot normalize pixel format {}",
              av_get_pix_fmt_name(fmt));
      return false;
    }

    swsColor_ = sws_getCachedContext(
        swsColor_, frame.width, frame.height, fmt, frame.width, frame.height,
        AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsColor_) {
      logWarn("GpuImageUploader: libswscale cannot convert {} to yuv420p",
              av_get_pix_fmt_name(fmt));
      return false;
    }
    swsW_ = frame.width;
    swsH_ = frame.height;
    swsFormat_ = fmt;

    const int uvW = std::max(1, (frame.width + 1) / 2);
    const int uvH = std::max(1, (frame.height + 1) / 2);
    const size_t ySize = static_cast<size_t>(frame.width) * frame.height;
    const size_t uvSize = static_cast<size_t>(uvW) * uvH;
    yuv420Scratch_.resize(ySize + uvSize * 2);
    uint8_t *dstData[4] = {
        yuv420Scratch_.data(), yuv420Scratch_.data() + ySize,
        yuv420Scratch_.data() + ySize + uvSize, nullptr};
    int dstLinesize[4] = {frame.width, uvW, uvW, 0};
    const uint8_t *srcData[4] = {nullptr, nullptr, nullptr, nullptr};
    int srcLinesize[4] = {0, 0, 0, 0};
    for (int i = 0; i < std::min(frame.planes, 4); ++i) {
      srcData[i] = frame.plane[i].data();
      srcLinesize[i] = frame.linesize[i];
    }
    if (sws_scale(swsColor_, srcData, srcLinesize, 0, frame.height,
                  dstData, dstLinesize) <= 0) {
      logWarn("GpuImageUploader: libswscale returned no output for {}",
              av_get_pix_fmt_name(fmt));
      return false;
    }

    DecodedVideoFrame normalized;
    normalized.width = frame.width;
    normalized.height = frame.height;
    normalized.avFormat = AV_PIX_FMT_YUV420P;
    normalized.colorRange = frame.colorRange;
    normalized.colorSpace = frame.colorSpace;
    normalized.planes = 3;
    normalized.linesize[0] = frame.width;
    normalized.linesize[1] = uvW;
    normalized.linesize[2] = uvW;
    normalized.plane[0].assign(dstData[0], dstData[0] + ySize);
    normalized.plane[1].assign(dstData[1], dstData[1] + uvSize);
    normalized.plane[2].assign(dstData[2], dstData[2] + uvSize);
    return uploadColorTo(normalized, image);
  }

  if (planar420 && yuvPipeline_ != VK_NULL_HANDLE && frame.planes >= 3 &&
      frame.plane[0].data() && frame.plane[1].data() && frame.plane[2].data()) {
    const uint32_t yW = (uint32_t)frame.width;
    const uint32_t yH = (uint32_t)frame.height;
    const uint32_t uvW = std::max(1u, (yW + 1u) / 2u);
    const uint32_t uvH = std::max(1u, (yH + 1u) / 2u);
    const size_t ySize = (size_t)frame.linesize[0] * yH;
    const size_t uSize = (size_t)frame.linesize[1] * uvH;
    const size_t vSize = (size_t)frame.linesize[2] * uvH;
    // The planes share one staging allocation at cumulative offsets.
    // Reserving only the largest plane overruns the buffer on the V plane.
    if (!ensureStaging(std::max(ySize + uSize + vSize, (size_t)4096u)))
      return false;

    if (!beginUploadCmd())
      return false;
    auto copyPlane = [&](const uint8_t *src, int linesize, VkDeviceSize offset,
                         GpuImage &img, uint32_t w, uint32_t h) -> bool {
      if (!img.image)
        return false;
      auto *dst = static_cast<uint8_t *>(stagingMapped_);
      for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(dst + offset + (size_t)row * w,
                    src + (size_t)row * linesize, w);
      }
      if (!copyBufferToImage(img, staging_.buffer, offset, w, h,
                             VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
      return true;
    };
    const VkDeviceSize yOffset = 0;
    const VkDeviceSize uOffset = ySize;
    const VkDeviceSize vOffset = ySize + uSize;
    if (!copyPlane(frame.plane[0].data(), frame.linesize[0], yOffset, yPlane_,
                   yW, yH))
      return false;
    if (!copyPlane(frame.plane[1].data(), frame.linesize[1], uOffset, uPlane_,
                   uvW, uvH))
      return false;
    if (!copyPlane(frame.plane[2].data(), frame.linesize[2], vOffset, vPlane_,
                   uvW, uvH))
      return false;
    if (!dispatchYuvConvert(
            frame, compareEnabled_.load(std::memory_order_acquire), sharpness))
      return false;
    if ((modelW_ != srcW_ || modelH_ != srcH_) &&
        !dispatchBicubicPrefilter(frame))
      return false;
    if (!endUploadCmd())
      return false;
    if (frameUploadBatch_) {
      resetYuvDescriptorsAfterBatch_ = true;
    } else {
      vkResetDescriptorPool(device_, yuvDescPool_, 0);
    }
    return true;
  }

  logWarn("GpuImageUploader: unsupported pixel format {} for GPU-only path",
          frame.avFormat);
  return false;
}

bool GpuImageUploader::uploadMotion(const std::vector<MvEntry> &mvs) {
  static const bool forceCpu = std::getenv("TFORGE_FSR4_CPU_MOTION") != nullptr;
  if (forceCpu || motionPipeline_ == VK_NULL_HANDLE) {
    return uploadMotionCpu(mvs);
  }
  if (modelW_ == 0 || motionOwners_.buffer == VK_NULL_HANDLE ||
      !ensureMotionVectorBuffer(mvs.size())) {
    return false;
  }

  struct alignas(16) GpuMotionVector {
    int32_t dstX;
    int32_t dstY;
    float mvX;
    float mvY;
    uint32_t width;
    uint32_t height;
    uint32_t padding[2];
  };
  static_assert(sizeof(GpuMotionVector) == 32);
  auto *vectors = static_cast<GpuMotionVector *>(motionVectorsMapped_);
  for (size_t i = 0; i < mvs.size(); ++i) {
    const auto &mv = mvs[i];
    vectors[i] = {mv.dstX, mv.dstY, mv.mvX, mv.mvY, mv.w, mv.h, {0, 0}};
  }

  if (!beginUploadCmd())
    return false;
  vkCmdFillBuffer(cmd_, motionOwners_.buffer, 0, VK_WHOLE_SIZE, 0);

  VkBufferMemoryBarrier ownerClear{};
  ownerClear.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  ownerClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  ownerClear.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  ownerClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ownerClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ownerClear.buffer = motionOwners_.buffer;
  ownerClear.offset = 0;
  ownerClear.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                       &ownerClear, 0, nullptr);

  VkImageMemoryBarrier motionReady{};
  motionReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  motionReady.srcAccessMask =
      motion_.layout == VK_IMAGE_LAYOUT_UNDEFINED
          ? 0
          : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  motionReady.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  motionReady.oldLayout = motion_.layout;
  motionReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  motionReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  motionReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  motionReady.image = motion_.image;
  motionReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_,
                       motion_.layout == VK_IMAGE_LAYOUT_UNDEFINED
                           ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                           : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &motionReady);
  motion_.layout = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo asi{};
  asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  asi.descriptorPool = motionDescPool_;
  asi.descriptorSetCount = 1;
  asi.pSetLayouts = &motionDescLayout_;
  if (vkAllocateDescriptorSets(device_, &asi, &set) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorBufferInfo vectorInfo{motionVectors_.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo ownerInfo{motionOwners_.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo motionInfo{VK_NULL_HANDLE, motion_.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  std::array<VkWriteDescriptorSet, 3> writes{};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[0].dstSet = set;
  writes[0].dstBinding = 0;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &vectorInfo;
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[1].dstSet = set;
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].descriptorCount = 1;
  writes[1].pBufferInfo = &ownerInfo;
  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[2].dstSet = set;
  writes[2].dstBinding = 2;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &motionInfo;
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, motionPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          motionPipelineLayout_, 0, 1, &set, 0, nullptr);
  constexpr uint32_t maxDispatchX = 65535;
  uint32_t push[5] = {modelW_, modelH_, static_cast<uint32_t>(mvs.size()), 0, 0};
  while (push[3] < push[2]) {
    const uint32_t count = std::min(maxDispatchX, push[2] - push[3]);
    vkCmdPushConstants(cmd_, motionPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), push);
    vkCmdDispatch(cmd_, count, 1, 1);
    push[3] += count;
  }

  VkBufferMemoryBarrier ownerResolve = ownerClear;
  ownerResolve.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  ownerResolve.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                       &ownerResolve, 0, nullptr);

  push[3] = 0;
  push[4] = 1;
  vkCmdPushConstants(cmd_, motionPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(push), push);
  vkCmdDispatch(cmd_, (modelW_ + 15u) / 16u, (modelH_ + 15u) / 16u, 1);

  VkImageMemoryBarrier motionWritten = motionReady;
  motionWritten.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  motionWritten.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  motionWritten.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &motionWritten);

  if (!endUploadCmd())
    return false;
  if (frameUploadBatch_) {
    resetMotionDescriptorsAfterBatch_ = true;
  } else {
    vkResetDescriptorPool(device_, motionDescPool_, 0);
  }
  return true;
}

bool GpuImageUploader::uploadMotionCpu(const std::vector<MvEntry> &mvs) {
  if (modelW_ == 0)
    return false;
  // Each source pixel gets its block's MV via nearest block lookup. Build a
  // lookup table of (dstX, dstY) → mv per block, then splat.
  // Layout: rg16f = 2 × half per pixel = 4 bytes/pixel.
  const size_t motionSize = (size_t)modelW_ * modelH_ * 4;
  if (!ensureStaging(std::max(motionSize, (VkDeviceSize)4096)))
    return false;

  // Fill a block map: index by (blockTopLeftY * srcW + blockTopLeftX).
  // For each pixel, find the block whose (dstX,dstY) <= pixel and whose
  // (dstX+w, dstY+h) > pixel. Naive O(pixels*blocks) is too slow; instead
  // build a 2D coverage array by stamping each block's MV over its region.
  auto *out = static_cast<uint8_t *>(stagingMapped_);
  std::memset(out, 0, motionSize); // zero = no motion (default)

  auto writeHalf = [&](size_t pixelIdx, float mvX, float mvY) {
    // fp16 encode (cheap: clamp + reuse bit pattern via fp16-from-fp32).
    // We pack two fp16 into 4 bytes.
    auto f32to16 = [](float f) -> uint16_t {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      uint32_t sign = (u >> 16) & 0x8000;
      int32_t exp = ((u >> 23) & 0xff) - 127 + 15;
      uint32_t mant = (u >> 13) & 0x3ff;
      if (exp <= 0)
        return sign; // denormal/zero → zero (motion is small)
      if (exp >= 31)
        return sign | 0x7c00; // inf/nan → inf
      return static_cast<uint16_t>(sign | (exp << 10) | mant);
    };
    uint16_t hx = f32to16(mvX);
    uint16_t hy = f32to16(mvY);
    out[pixelIdx * 4 + 0] = hx & 0xff;
    out[pixelIdx * 4 + 1] = (hx >> 8) & 0xff;
    out[pixelIdx * 4 + 2] = hy & 0xff;
    out[pixelIdx * 4 + 3] = (hy >> 8) & 0xff;
  };

  // Stamp each MV block over its destination region.
  for (const auto &m : mvs) {
    int x0 = std::max(0, (int)m.dstX);
    int y0 = std::max(0, (int)m.dstY);
    int x1 = std::min((int)modelW_, (int)m.dstX + m.w);
    int y1 = std::min((int)modelH_, (int)m.dstY + m.h);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        size_t idx = (size_t)y * modelW_ + x;
        writeHalf(idx, m.mvX, m.mvY);
      }
    }
  }

  if (!beginUploadCmd())
    return false;
  if (!copyBufferToImage(motion_, staging_.buffer, 0, modelW_, modelH_,
                         VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  return endUploadCmd();
}

bool GpuImageUploader::uploadDepthFlat() {
  if (modelW_ == 0)
    return false;
  const size_t depthSize = (size_t)modelW_ * modelH_ * 4; // r32f = 4 bytes
  if (!ensureStaging(std::max(depthSize, (VkDeviceSize)4096)))
    return false;
  // Flat depth = 1.0 everywhere (spec EdgeLite "Flat" mode).
  auto *f = reinterpret_cast<float *>(stagingMapped_);
  for (size_t i = 0; i < (size_t)modelW_ * modelH_; ++i)
    f[i] = 1.0f;

  if (!beginUploadCmd())
    return false;
  return copyBufferToImage(depth_, staging_.buffer, 0, modelW_, modelH_,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
         endUploadCmd();
}

bool GpuImageUploader::uploadDepthEdgeLite(const SideBufferSource &s) {
  if (srcW_ == 0 || !s.luma)
    return false;
  const int W = s.lumaWidth;
  const int H = s.lumaHeight;
  if (W <= 0 || H <= 0)
    return false;
  const size_t depthSize = (size_t)modelW_ * modelH_ * 4;
  if (!ensureStaging(std::max(depthSize, (VkDeviceSize)4096)))
    return false;

  auto *out = reinterpret_cast<float *>(stagingMapped_);
  // Sobel edge detect on luma. Edges → low depth (0.0), flat → high (1.0).
  // This matches the spec's EdgeLite semantic: edges get reconstructed harder.
  const auto at = [&](int x, int y) -> float {
    x = std::clamp(x, 0, W - 1);
    y = std::clamp(y, 0, H - 1);
    return s.luma[(size_t)y * s.lumaLinesize + x] * (1.0f / 255.0f);
  };
  for (int y = 0; y < (int)modelH_; ++y) {
    for (int x = 0; x < (int)modelW_; ++x) {
      // Map to luma coords (source may equal luma for yuv420p luma plane).
      int lx = W == (int)modelW_ ? x : (x * W) / (int)modelW_;
      int ly = H == (int)modelH_ ? y : (y * H) / (int)modelH_;
      float gx = -at(lx - 1, ly - 1) - 2 * at(lx - 1, ly) - at(lx - 1, ly + 1) +
                 at(lx + 1, ly - 1) + 2 * at(lx + 1, ly) + at(lx + 1, ly + 1);
      float gy = -at(lx - 1, ly - 1) - 2 * at(lx, ly - 1) - at(lx + 1, ly - 1) +
                 at(lx - 1, ly + 1) + 2 * at(lx, ly + 1) + at(lx + 1, ly + 1);
      float mag = std::sqrt(gx * gx + gy * gy);
      // Edge magnitude ~ [0, ~4]. Map: high edge → low depth.
      float depth = std::clamp(1.0f - mag * 0.5f, 0.0f, 1.0f);
      out[(size_t)y * modelW_ + x] = depth;
    }
  }

  if (!beginUploadCmd())
    return false;
  return copyBufferToImage(depth_, staging_.buffer, 0, modelW_, modelH_,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
         endUploadCmd();
}

bool GpuImageUploader::uploadReactive(const SideBufferSource &s,
                                      bool aggressive) {
  if (modelW_ == 0)
    return false;
  const size_t reacSize = (size_t)modelW_ * modelH_; // r8 = 1 byte
  if (!ensureStaging(std::max(reacSize, (VkDeviceSize)4096)))
    return false;

  auto *out = static_cast<uint8_t *>(stagingMapped_);
  if (s.reactiveAverage <= 0.0f) {
    std::memset(out, 0, reacSize);
    if (!beginUploadCmd())
      return false;
    return copyBufferToImage(reactive_, staging_.buffer, 0, modelW_, modelH_,
                             VK_IMAGE_ASPECT_COLOR_BIT) &&
           endUploadCmd();
  }
  if (!s.luma || s.lumaWidth <= 0 || s.lumaHeight <= 0)
    return false;
  const int W = s.lumaWidth;
  const int H = s.lumaHeight;
  // Local luma variance in a 3x3 window → reactive mask. High-variance
  // regions (detail/edges) get higher reactive gain so the network knows to
  // preserve detail there. Multiplied by the global reactiveAverage.
  const float gain = aggressive ? 2.0f : 1.0f;
  const auto at = [&](int x, int y) -> float {
    x = std::clamp(x, 0, W - 1);
    y = std::clamp(y, 0, H - 1);
    return s.luma[(size_t)y * s.lumaLinesize + x] * (1.0f / 255.0f);
  };
  for (int y = 0; y < (int)modelH_; ++y) {
    for (int x = 0; x < (int)modelW_; ++x) {
      int lx = W == (int)modelW_ ? x : (x * W) / (int)modelW_;
      int ly = H == (int)modelH_ ? y : (y * H) / (int)modelH_;
      float c = at(lx, ly);
      float mean = (at(lx - 1, ly - 1) + at(lx, ly - 1) + at(lx + 1, ly - 1) +
                    at(lx - 1, ly) + c + at(lx + 1, ly) + at(lx - 1, ly + 1) +
                    at(lx, ly + 1) + at(lx + 1, ly + 1)) *
                   (1.0f / 9.0f);
      float var = 0.0f;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          float d = at(lx + dx, ly + dy) - mean;
          var += d * d;
        }
      var *= (1.0f / 9.0f);
      // var ~ [0, 0.25]. Scale + apply reactiveAverage + gain.
      float r = std::clamp(var * 8.0f * s.reactiveAverage * gain, 0.0f, 1.0f);
      out[(size_t)y * modelW_ + x] = static_cast<uint8_t>(r * 255.0f);
    }
  }

  if (!beginUploadCmd())
    return false;
  return copyBufferToImage(reactive_, staging_.buffer, 0, modelW_, modelH_,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
         endUploadCmd();
}

bool GpuImageUploader::clearTcMask() {
  if (modelW_ == 0)
    return false;
  const size_t sz = (size_t)modelW_ * modelH_;
  if (!ensureStaging(std::max(sz, (VkDeviceSize)4096)))
    return false;
  std::memset(stagingMapped_, 0, sz);
  if (!beginUploadCmd())
    return false;
  return copyBufferToImage(tcMask_, staging_.buffer, 0, modelW_, modelH_,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
         endUploadCmd();
}

bool GpuImageUploader::uploadExposure(float scalar) {
  if (!ensureStaging(4))
    return false;
  // fp16 encode.
  uint32_t u;
  std::memcpy(&u, &scalar, 4);
  uint32_t sign = (u >> 16) & 0x8000;
  int32_t exp = ((u >> 23) & 0xff) - 127 + 15;
  uint32_t mant = (u >> 13) & 0x3ff;
  uint16_t h =
      (exp <= 0) ? sign
                 : (exp >= 31 ? (sign | 0x7c00) : (sign | (exp << 10) | mant));
  auto *p = static_cast<uint8_t *>(stagingMapped_);
  p[0] = h & 0xff;
  p[1] = (h >> 8) & 0xff;
  if (!beginUploadCmd())
    return false;
  return copyBufferToImage(exposure_, staging_.buffer, 0, 1, 1,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
         endUploadCmd();
}

// dispatchEasu: run the FSR1-EASU compute pass to scale the native-res color
//               image to the 2x easuImage_ intermediate. Records into the
//               active frame-upload command buffer (must be called after
//               uploadColor + dispatchYuvConvert, before endFrameUploads).
bool GpuImageUploader::dispatchEasu() {
  if (easuPipeline_ == VK_NULL_HANDLE || easuImage_.image == VK_NULL_HANDLE ||
      rawPresentation_.image == VK_NULL_HANDLE || easuSet_ == VK_NULL_HANDLE)
    return false;
  if (!beginUploadCmd())
    return false;

  // uploadColor() may have submitted the YUV/DRM conversion separately.
  // Make that shader write visible to this dispatch even when the fallback
  // is not using the batched neural prefix path.
  VkImageMemoryBarrier sourceReady{};
  sourceReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  sourceReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  sourceReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  sourceReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  sourceReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  sourceReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  sourceReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  sourceReady.image = rawPresentation_.image;
  sourceReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &sourceReady);

  // Transition easuImage_ from UNDEFINED to GENERAL (first use per allocate).
  if (easuImage_.layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = easuImage_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    easuImage_.layout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Bicubic operates on display RGB, not the model-space RGB10 image used by
  // the FSR network. The raw image is written by the YUV conversion pass.
  std::array<VkDescriptorImageInfo, 2> imgInfos{};
  imgInfos[0].sampler = VK_NULL_HANDLE;
  imgInfos[0].imageView = rawPresentation_.view;
  imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  imgInfos[1].sampler = VK_NULL_HANDLE;
  imgInfos[1].imageView = easuImage_.view;
  imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  std::array<VkWriteDescriptorSet, 2> writes{};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = easuSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &imgInfos[i];
  }
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  // Bind pipeline + push constants + dispatch.
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          easuPipelineLayout_, 0, 1, &easuSet_, 0, nullptr);
  const uint32_t push[5] = {
      srcW_, srcH_, easuImage_.width, easuImage_.height,
      static_cast<uint32_t>(std::clamp(presentationScaler_.load(
          std::memory_order_acquire), 0, 4))};
  vkCmdPushConstants(cmd_, easuPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), push);
  const uint32_t groupsX = (easuImage_.width + 15u) / 16u;
  const uint32_t groupsY = (easuImage_.height + 15u) / 16u;
  vkCmdDispatch(cmd_, groupsX, groupsY, 1);

  easuActive_ = true;
  if (!frameUploadBatch_) {
    if (!endUploadCmd())
      return false;
  }
  return true;
}

bool GpuImageUploader::dispatchPresentationScaler(uint32_t width,
                                                   uint32_t height) {
  if (output_.image == VK_NULL_HANDLE || easuPipeline_ == VK_NULL_HANDLE ||
      width == 0 || height == 0)
    return false;
  if (width == output_.width && height == output_.height) {
    destroyGpuImage(device_, presentation_);
    return true;
  }
  if (presentation_.width != width || presentation_.height != height) {
    // Reuse the previous target when the window alternates between two
    // dimensions. The dispatch path is fence-serialized, so the inactive
    // image is no longer referenced by the queue at this point.
    bool reused = false;
    if (presentationRetained_.image != VK_NULL_HANDLE &&
        presentationRetained_.width == width &&
        presentationRetained_.height == height) {
      std::swap(presentation_, presentationRetained_);
      reused = true;
    } else {
      destroyGpuImage(device_, presentationRetained_);
      std::swap(presentation_, presentationRetained_);
    }
    if (!reused &&
        !createGpuImage(device_, physical_, width, height,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, presentation_,
                        "fsr4_presentation", queueFamily_,
                        presentationQueueFamily_))
      return false;
  }
  if (!beginUploadCmd())
    return false;
  const bool profilePresentation =
      std::getenv("TFORGE_FSR4_PROFILE_PRESENTATION") != nullptr;
  const auto presentationStart = std::chrono::steady_clock::now();
  if (presentation_.layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = presentation_.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = presentation_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    presentation_.layout = VK_IMAGE_LAYOUT_GENERAL;
  }
  VkDescriptorImageInfo sourceInfo{VK_NULL_HANDLE, output_.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo destinationInfo{VK_NULL_HANDLE, presentation_.view,
                                        VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = easuSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  writes[0].pImageInfo = &sourceInfo;
  writes[1].pImageInfo = &destinationInfo;
  vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
  const uint32_t push[5] = {
      output_.width, output_.height, width, height,
      static_cast<uint32_t>(std::clamp(presentationScaler_.load(
          std::memory_order_acquire), 0, 4))};
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          easuPipelineLayout_, 0, 1, &easuSet_, 0, nullptr);
  vkCmdPushConstants(cmd_, easuPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), push);
  vkCmdDispatch(cmd_, (width + 15u) / 16u, (height + 15u) / 16u, 1);
  if (!frameUploadBatch_) {
    const bool ok = endUploadCmd();
    if (profilePresentation) {
      const double elapsedMs = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - presentationStart).count();
      logInfo("GpuImageUploader: presentation scaler {}x{} -> {}x{} "
              "filter={} CPU/wait={:.3f}ms",
              output_.width, output_.height, width, height,
              presentationScaler_.load(std::memory_order_acquire), elapsedMs);
    }
    return ok;
  }
  return true;
}

bool GpuImageUploader::transitionOutputToGeneral() {
  if (output_.image == VK_NULL_HANDLE)
    return false;
  if (!beginUploadCmd())
    return false;
  // Transition the model-color image as well as output/history resources. The
  // first pass overwrites color from decoded YUV, while chained passes write
  // their color image from the preceding pass's RGB10 history on the GPU.
  for (auto *img :
       {&color_, &output_, &history_[0], &history_[1], &recurrent_[0],
        &recurrent_[1]}) {
    if (img->layout == VK_IMAGE_LAYOUT_GENERAL)
      continue;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    img->layout = VK_IMAGE_LAYOUT_GENERAL;
  }
  return endUploadCmd();
}

bool GpuImageUploader::readbackOutput(std::vector<uint8_t> &dst, uint32_t &outW,
                                      uint32_t &outH) {
  if (output_.image == VK_NULL_HANDLE || outW_ == 0)
    return false;
  // The native presentation images are RGBA8. Keep this diagnostic/readback
  // path honest with the actual image format; treating the image as RGBA32F
  // returned garbage and made it impossible to inspect the real backend
  // output.
  const size_t rgba8Size = (size_t)outW_ * outH_ * 4;
  if (!ensureStaging(std::max(rgba8Size, (VkDeviceSize)4096)))
    return false;

  if (!beginUploadCmd())
    return false;
  // GENERAL → TRANSFER_SRC_OPTIMAL (output is left in GENERAL by postpass).
  VkImageMemoryBarrier b1{};
  b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b1.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.image = output_.image;
  b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b1);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {outW_, outH_, 1};
  vkCmdCopyImageToBuffer(cmd_, output_.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_.buffer,
                         1, &region);

  // Transition back to GENERAL for the next postpass write.
  VkImageMemoryBarrier b2{};
  b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.image = output_.image;
  b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b2);
  if (!endUploadCmd())
    return false;

  dst.resize(rgba8Size);
  std::memcpy(dst.data(), stagingMapped_, rgba8Size);
  outW = outW_;
  outH = outH_;
  return true;
}

bool GpuImageUploader::readbackPresentation(std::vector<uint8_t> &dst,
                                            uint32_t &outW,
                                            uint32_t &outH) {
  if (presentation_.image == VK_NULL_HANDLE)
    return readbackOutput(dst, outW, outH);

  const uint32_t width = presentation_.width;
  const uint32_t height = presentation_.height;
  if (width == 0 || height == 0)
    return false;
  const size_t rgba8Size = static_cast<size_t>(width) * height * 4;
  if (!ensureStaging(std::max(rgba8Size, static_cast<size_t>(4096))))
    return false;
  if (!beginUploadCmd())
    return false;

  const VkImageLayout oldLayout = presentation_.layout;
  VkImageMemoryBarrier toTransfer{};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = oldLayout;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = presentation_.image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd_, presentation_.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_.buffer,
                         1, &region);

  VkImageMemoryBarrier backToGeneral = toTransfer;
  backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  backToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  backToGeneral.newLayout = oldLayout;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &backToGeneral);
  if (!endUploadCmd())
    return false;

  dst.resize(rgba8Size);
  std::memcpy(dst.data(), stagingMapped_, rgba8Size);
  outW = width;
  outH = height;
  return true;
}

bool GpuImageUploader::readbackEasu(std::vector<uint8_t> &dst, uint32_t &outW,
                                    uint32_t &outH) {
  if (easuImage_.image == VK_NULL_HANDLE || easuImage_.width == 0)
    return false;
  const size_t rgba8Size = static_cast<size_t>(easuImage_.width) *
                           easuImage_.height * 4;
  if (!ensureStaging(std::max(rgba8Size, static_cast<size_t>(4096))))
    return false;
  if (!beginUploadCmd())
    return false;

  VkImageMemoryBarrier toTransfer{};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = easuImage_.image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {easuImage_.width, easuImage_.height, 1};
  vkCmdCopyImageToBuffer(cmd_, easuImage_.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_.buffer,
                         1, &region);

  VkImageMemoryBarrier backToGeneral = toTransfer;
  backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  backToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  backToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &backToGeneral);
  if (!endUploadCmd())
    return false;

  dst.resize(rgba8Size);
  std::memcpy(dst.data(), stagingMapped_, rgba8Size);
  outW = easuImage_.width;
  outH = easuImage_.height;
  return true;
}

bool GpuImageUploader::readbackRaw(std::vector<uint8_t> &dst, uint32_t &outW,
                                   uint32_t &outH) {
  if (rawPresentation_.image == VK_NULL_HANDLE || srcW_ == 0)
    return false;
  const size_t rgba8Size = static_cast<size_t>(srcW_) * srcH_ * 4;
  if (!ensureStaging(std::max(rgba8Size, static_cast<size_t>(4096))))
    return false;
  if (!beginUploadCmd())
    return false;

  VkImageMemoryBarrier toTransfer{};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = rawPresentation_.layout;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = rawPresentation_.image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {srcW_, srcH_, 1};
  vkCmdCopyImageToBuffer(cmd_, rawPresentation_.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_.buffer,
                         1, &region);

  VkImageMemoryBarrier back{};
  back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  back.image = rawPresentation_.image;
  back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &back);
  rawPresentation_.layout = VK_IMAGE_LAYOUT_GENERAL;
  if (!endUploadCmd())
    return false;

  dst.resize(rgba8Size);
  std::memcpy(dst.data(), stagingMapped_, rgba8Size);
  outW = srcW_;
  outH = srcH_;
  return true;
}

} // namespace temporal_forge
