#include "view/Renderer.h"
#include "scene/Cocoons.h"
#include "scene/Looks.h"
#include "scene/Refs.h"
#include "scene/Symmetry.h"
#include "scene/MeshMods.h"
#include "scene/Tubes.h"
#include "scene/Walls.h"
#include "view/Viewport.h"
#include <QPainter>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QVulkanFunctions>
#include <numeric>
#include <cstring>
#include <set>
#include <unordered_set>

namespace hm {

namespace {
struct MeshPC { float viewProj[16]; float color[4]; float lightDir[4]; float params[4]; float extra[4]; };   // (extra: the eye, PDO)
struct LinePC { float viewProj[16]; float eye[4]; float bias[4]; float tint[4]; };
struct PointPC { float viewProj[16]; float params[4]; float eye[4]; };
const double PI = 3.14159265358979323846;
}  // namespace

Renderer::Renderer(Viewport* w) : w_(w) {}

Renderer::Buf Renderer::makeBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
  Buf b; if (!size) return b;
  VkDevice dev = w_->device();
  // a big buffer (a heavy mesh: hundreds of MB) into the GPU's own memory through a staging copy — read from host
  // memory over the bus every frame it made a sculpt of a few million triangles draw at ~18 FPS
  if (size >= (4u << 20) && !(usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
    Buf st = makeBuffer(data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!st.buf) return {};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (df_->vkCreateBuffer(dev, &bi, nullptr, &b.buf) != VK_SUCCESS) { df_->vkDestroyBuffer(dev, st.buf, nullptr); df_->vkFreeMemory(dev, st.mem, nullptr); return {}; }
    VkMemoryRequirements req; df_->vkGetBufferMemoryRequirements(dev, b.buf, &req);
    uint32_t mt = w_->deviceLocalMemoryIndex();
    if (!(req.memoryTypeBits & (1u << mt))) {
      VkPhysicalDeviceMemoryProperties mp; w_->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(w_->physicalDevice(), &mp);
      for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = mt;
    if (df_->vkAllocateMemory(dev, &ai, nullptr, &b.mem) != VK_SUCCESS) {   // (no room on the GPU: the host buffer then)
      df_->vkDestroyBuffer(dev, b.buf, nullptr); df_->vkDestroyBuffer(dev, st.buf, nullptr); df_->vkFreeMemory(dev, st.mem, nullptr);
      return makeBuffer(data, size, usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    df_->vkBindBufferMemory(dev, b.buf, b.mem, 0);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = w_->graphicsCommandPool(); ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VkCommandBuffer cb; df_->vkAllocateCommandBuffers(dev, &ca, &cb);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    df_->vkBeginCommandBuffer(cb, &cbi);
    VkBufferCopy cp{0, 0, size}; df_->vkCmdCopyBuffer(cb, st.buf, b.buf, 1, &cp);
    df_->vkEndCommandBuffer(cb);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
    df_->vkQueueSubmit(w_->graphicsQueue(), 1, &sub, VK_NULL_HANDLE);
    df_->vkQueueWaitIdle(w_->graphicsQueue());
    df_->vkFreeCommandBuffers(dev, w_->graphicsCommandPool(), 1, &cb);
    df_->vkDestroyBuffer(dev, st.buf, nullptr); df_->vkFreeMemory(dev, st.mem, nullptr);
    b.size = size;
    return b;
  }
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage | (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT ? 0 : VK_BUFFER_USAGE_TRANSFER_DST_BIT); bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (df_->vkCreateBuffer(dev, &bi, nullptr, &b.buf) != VK_SUCCESS) { qWarning("vkCreateBuffer failed"); return {}; }
  VkMemoryRequirements req; df_->vkGetBufferMemoryRequirements(dev, b.buf, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size; ai.memoryTypeIndex = w_->hostVisibleMemoryIndex();
  if (df_->vkAllocateMemory(dev, &ai, nullptr, &b.mem) != VK_SUCCESS) { qWarning("vkAllocateMemory failed"); df_->vkDestroyBuffer(dev, b.buf, nullptr); return {}; }
  df_->vkBindBufferMemory(dev, b.buf, b.mem, 0);
  void* p = nullptr;
  if (df_->vkMapMemory(dev, b.mem, 0, size, 0, &p) == VK_SUCCESS) { std::memcpy(p, data, size_t(size)); df_->vkUnmapMemory(dev, b.mem); }
  b.size = size;
  return b;
}

void Renderer::retire(Buf& b) {
  if (b.buf) dead_.push_back({b, w_->concurrentFrameCount() + 1});
  b = {};
}

void Renderer::collectGarbage(bool all) {
  VkDevice dev = w_->device();
  for (size_t i = 0; i < dead_.size();) {
    if (all || --dead_[i].frames <= 0) {
      df_->vkDestroyBuffer(dev, dead_[i].b.buf, nullptr); df_->vkFreeMemory(dev, dead_[i].b.mem, nullptr);
      dead_[i] = dead_.back(); dead_.pop_back();
    } else i++;
  }
  for (size_t i = 0; i < deadTex_.size();) {
    if (all || --deadTex_[i].frames <= 0) {
      if (deadTex_[i].ds && texPool_) df_->vkFreeDescriptorSets(dev, texPool_, 1, &deadTex_[i].ds);
      destroyTex(deadTex_[i].t);
      deadTex_[i] = deadTex_.back(); deadTex_.pop_back();
    } else i++;
  }
}

VkShaderModule Renderer::loadShader(const QString& name) {
  QFile f(QStringLiteral(":/shaders/%1.spv").arg(name));
  if (!f.open(QIODevice::ReadOnly)) { qWarning("missing shader %s", qPrintable(name)); return VK_NULL_HANDLE; }
  QByteArray code = f.readAll();
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = size_t(code.size()); ci.pCode = reinterpret_cast<const uint32_t*>(code.constData());
  VkShaderModule m = VK_NULL_HANDLE;
  df_->vkCreateShaderModule(w_->device(), &ci, nullptr, &m);
  return m;
}

VkPipeline Renderer::makePipeline(VkPipelineLayout layout, const QString& vs, const QString& fs, VkPrimitiveTopology topo,
                                  const VkVertexInputBindingDescription& bind, const VkVertexInputAttributeDescription* attrs, int nAttrs,
                                  bool depthTest, bool depthWrite, bool blend, bool depthBias, bool lineWidthDynamic, bool colorWrite) {
  VkShaderModule v = loadShader(vs), f = loadShader(fs);
  VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = v; st[0].pName = "main";
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = f; st[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = uint32_t(nAttrs); vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topo;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;     // (the clip correction flips Y: GL's counter-clockwise front faces)
  rs.lineWidth = 1.0f;
  if (depthBias) { rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = 1.0f; rs.depthBiasSlopeFactor = 1.0f; }
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = w_->sampleCountFlagBits();
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = depthTest; ds.depthWriteEnable = depthWrite; ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = colorWrite ? 0xF : 0;
  if (blend) {
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1; cb.pAttachments = &ba;
  VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = lineWidthDynamic ? 3 : 2; dy.pDynamicStates = dyn;

  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
  pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb;
  pi.pDynamicState = &dy; pi.layout = layout; pi.renderPass = w_->defaultRenderPass();
  VkPipeline p = VK_NULL_HANDLE;
  if (df_->vkCreateGraphicsPipelines(w_->device(), cache_, 1, &pi, nullptr, &p) != VK_SUCCESS) qWarning("pipeline %s/%s failed", qPrintable(vs), qPrintable(fs));
  df_->vkDestroyShaderModule(w_->device(), v, nullptr); df_->vkDestroyShaderModule(w_->device(), f, nullptr);
  return p;
}

void Renderer::initResources() {
  VkDevice dev = w_->device();
  df_ = w_->vulkanInstance()->deviceFunctions(dev);
  VkPhysicalDeviceFeatures feat{};
  w_->vulkanInstance()->functions()->vkGetPhysicalDeviceFeatures(w_->physicalDevice(), &feat);
  wideLines_ = feat.wideLines;
  maxLineWidth_ = w_->physicalDeviceProperties()->limits.lineWidthRange[1];

  VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  df_->vkCreatePipelineCache(dev, &pc, nullptr, &cache_);

  auto layout = [&](uint32_t size, VkShaderStageFlags stages) {
    VkPushConstantRange r{stages, 0, size};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &r;
    VkPipelineLayout l = VK_NULL_HANDLE; df_->vkCreatePipelineLayout(dev, &li, nullptr, &l); return l;
  };
  const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  meshLayout_ = layout(sizeof(MeshPC), VF);
  lineLayout_ = layout(sizeof(LinePC), VF);
  pointLayout_ = layout(sizeof(PointPC), VF);

  VkVertexInputBindingDescription mb{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription ma[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 36}};
  meshPipe_ = makePipeline(meshLayout_, "mesh.vert", "mesh.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mb, ma, 4, true, true, false, true, false);
  // see-through meshes (a cocoon's shell): blended, depth tested, not written
  meshBlendPipe_ = makePipeline(meshLayout_, "mesh.vert", "mesh.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mb, ma, 4, true, false, true, false, false);
  meshDepthPipe_ = makePipeline(meshLayout_, "mesh.vert", "mesh.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mb, ma, 4, true, true, false, false, false, false);
  // see-through NURBS walls (above 0.6 opacity they still hide what is behind them, as setSurfMat's depthWrite)
  meshBlendWritePipe_ = makePipeline(meshLayout_, "mesh.vert", "mesh.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mb, ma, 4, true, true, true, true, false);

  // lines take a colour per vertex: a mesh's wireframe gets a LineVertex buffer of its own
  VkVertexInputBindingDescription lb{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription la[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}};
  linePipe_ = makePipeline(lineLayout_, "line.vert", "line.frag", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, lb, la, 2, true, false, true, false, wideLines_);
  lineTopPipe_ = makePipeline(lineLayout_, "line.vert", "line.frag", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, lb, la, 2, false, false, true, false, wideLines_);

  VkVertexInputBindingDescription pb{0, sizeof(PointVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription pa[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
                                             {2, 0, VK_FORMAT_R32_SFLOAT, 28}};
  pointPipe_ = makePipeline(pointLayout_, "point.vert", "point.frag", VK_PRIMITIVE_TOPOLOGY_POINT_LIST, pb, pa, 3, false, false, true, false, false);
  // (the anchors: hidden behind the head and the hair, X-ray shows them through at its opacity)
  pointDepthPipe_ = makePipeline(pointLayout_, "point.vert", "point.frag", VK_PRIMITIVE_TOPOLOGY_POINT_LIST, pb, pa, 3, true, false, true, false, false);

  // textured meshes: two images per texture set (colour, opacity) — a descriptor set each pair, the sampler shared
  VkDescriptorSetLayoutBinding tb[2]{};
  for (uint32_t i = 0; i < 2; i++) { tb[i].binding = i; tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tb[i].descriptorCount = 1; tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 2; dl.pBindings = tb;
  df_->vkCreateDescriptorSetLayout(dev, &dl, nullptr, &texSetLayout_);
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; dp.maxSets = 1024; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
  df_->vkCreateDescriptorPool(dev, &dp, nullptr, &texPool_);
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.maxLod = VK_LOD_CLAMP_NONE;
  df_->vkCreateSampler(dev, &si, nullptr, &sampler_);
  {
    VkPushConstantRange r{VF, 0, sizeof(MeshPC)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &texSetLayout_; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &r;
    df_->vkCreatePipelineLayout(dev, &li, nullptr, &texLayout_);
  }
  VkVertexInputBindingDescription tvb{0, sizeof(TexVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription tva[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
  texPipe_ = makePipeline(texLayout_, "mesh_tex.vert", "mesh_tex.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, tvb, tva, 3, true, true, false, true, false);
  // (see-through sets: blended and written to depth — three.js's transparent material; below 0.02 dropped)
  texBlendPipe_ = makePipeline(texLayout_, "mesh_tex.vert", "mesh_tex.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, tvb, tva, 3, true, true, true, true, false);
  // reference planes: blended, not written to depth; behind the model (drawn first, depth tested) or over it (no test)
  rpBehindPipe_ = makePipeline(texLayout_, "mesh_tex.vert", "mesh_tex.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, tvb, tva, 3, true, false, true, false, false);
  rpOverPipe_ = makePipeline(texLayout_, "mesh_tex.vert", "mesh_tex.frag", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, tvb, tva, 3, false, false, true, false, false);
  QImage w(1, 1, QImage::Format_RGBA8888); w.fill(Qt::white); white_ = uploadImage(w, false);
}

// a reference plane's picture (a data: URL kept in the project): known by its length and its ends
looks::MapSrc Renderer::refImage(const RefPlane& rp) const {
  looks::MapSrc s; if (rp.img.isEmpty()) return s;
  s.kind = looks::MapSrc::Kept; s.key = "rp:" + QString::number(rp.img.size()) + ":" + QString::number(qHash(QStringView(rp.img).left(8192))) + ":" + QString::number(qHash(QStringView(rp.img).right(8192)));
  if (!texCache_.count(s.key + "#c")) s.bytes = looks::dataUrlBytes(rp.img);   // (decoded only the first time)
  s.label = rp.name; return s;
}

// an image to the GPU (device-local, optimal tiling) with its mip levels made by blits; colour maps in sRGB
Renderer::GpuTex Renderer::uploadImage(const QImage& src, bool srgb) {
  GpuTex t; VkDevice dev = w_->device();
  QImage img = src.convertToFormat(QImage::Format_RGBA8888);
  const uint32_t maxDim = w_->physicalDeviceProperties()->limits.maxImageDimension2D;
  if (uint32_t(img.width()) > maxDim || uint32_t(img.height()) > maxDim) img = img.scaled(int(std::min<uint32_t>(maxDim, uint32_t(img.width()))), int(std::min<uint32_t>(maxDim, uint32_t(img.height()))), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  if (img.isNull() || !img.width() || !img.height()) return t;
  const uint32_t W = uint32_t(img.width()), H = uint32_t(img.height());
  uint32_t mips = 1; for (uint32_t s = std::max(W, H); s > 1; s >>= 1) mips++;
  const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {W, H, 1}; ii.mipLevels = mips; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (df_->vkCreateImage(dev, &ii, nullptr, &t.img) != VK_SUCCESS) { qWarning("vkCreateImage failed"); return {}; }
  VkMemoryRequirements req; df_->vkGetImageMemoryRequirements(dev, t.img, &req);
  uint32_t mt = w_->deviceLocalMemoryIndex();
  if (!(req.memoryTypeBits & (1u << mt))) {
    VkPhysicalDeviceMemoryProperties mp; w_->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(w_->physicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = mt;
  if (df_->vkAllocateMemory(dev, &ai, nullptr, &t.mem) != VK_SUCCESS) { qWarning("texture memory failed"); df_->vkDestroyImage(dev, t.img, nullptr); return {}; }
  df_->vkBindImageMemory(dev, t.img, t.mem, 0);
  Buf staging = makeBuffer(img.constBits(), VkDeviceSize(img.sizeInBytes()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = w_->graphicsCommandPool(); ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
  VkCommandBuffer cb; df_->vkAllocateCommandBuffers(dev, &ca, &cb);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  df_->vkBeginCommandBuffer(cb, &bi);
  auto barrier = [&](uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = t.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
    df_->vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  barrier(0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy cp{}; cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.imageExtent = {W, H, 1};
  df_->vkCmdCopyBufferToImage(cb, staging.buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  int32_t mw = int32_t(W), mh = int32_t(H);
  for (uint32_t i = 1; i < mips; i++) {
    barrier(i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1}; bl.srcOffsets[1] = {mw, mh, 1};
    int32_t nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}; bl.dstOffsets[1] = {nw, nh, 1};
    df_->vkCmdBlitImage(cb, t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    mw = nw; mh = nh;
  }
  if (mips > 1) barrier(0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  df_->vkEndCommandBuffer(cb);
  VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
  df_->vkQueueSubmit(w_->graphicsQueue(), 1, &sub, VK_NULL_HANDLE);
  { ProfScope pw("gpuWait"); df_->vkQueueWaitIdle(w_->graphicsQueue()); }
  df_->vkFreeCommandBuffers(dev, w_->graphicsCommandPool(), 1, &cb);
  df_->vkDestroyBuffer(dev, staging.buf, nullptr); df_->vkFreeMemory(dev, staging.mem, nullptr);

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = t.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
  df_->vkCreateImageView(dev, &vi, nullptr, &t.view);
  return t;
}

void Renderer::destroyTex(GpuTex& t) {
  VkDevice dev = w_->device();
  if (t.view) df_->vkDestroyImageView(dev, t.view, nullptr);
  if (t.img) df_->vkDestroyImage(dev, t.img, nullptr);
  if (t.mem) df_->vkFreeMemory(dev, t.mem, nullptr);
  t = {};
}

// a map's image, read once and kept (by its key: the same image — one texture); unreadable — remembered as none
QString Renderer::textureFor(const looks::MapSrc& s, bool srgb) {
  if (s.kind == looks::MapSrc::None) return {};
  QString key = s.key + (srgb ? "#c" : "#l");
  auto it = texCache_.find(key);
  if (it == texCache_.end()) {
    QImage img;
    img = s.kind == looks::MapSrc::Linked ? looks::loadImage(s.path) : looks::decodeImage(s.bytes);
    if (img.isNull()) qWarning("could not read the texture %s", qPrintable(s.label));
    it = texCache_.emplace(key, img.isNull() ? GpuTex{} : uploadImage(img, srgb)).first;
  }
  return it->second.view ? key : QString();
}

VkDescriptorSet Renderer::descriptorFor(const QString& mapKey, const QString& alphaKey) {
  QString key = mapKey + "|" + alphaKey;
  auto it = texSets_.find(key); if (it != texSets_.end()) return it->second;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = texPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &texSetLayout_;
  VkDescriptorSet ds = VK_NULL_HANDLE;
  if (df_->vkAllocateDescriptorSets(w_->device(), &ai, &ds) != VK_SUCCESS) { qWarning("descriptor set failed"); return VK_NULL_HANDLE; }
  VkDescriptorImageInfo im[2]{};
  for (int i = 0; i < 2; i++) { const QString& k = i ? alphaKey : mapKey; auto t = texCache_.find(k);
    im[i].sampler = sampler_; im[i].imageView = !k.isEmpty() && t != texCache_.end() && t->second.view ? t->second.view : white_.view; im[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
  VkWriteDescriptorSet wr[2]{};
  for (uint32_t i = 0; i < 2; i++) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &im[i]; }
  df_->vkUpdateDescriptorSets(w_->device(), 2, wr, 0, nullptr);
  texSets_.emplace(key, ds);
  return ds;
}

// an imported mesh with its textures shown (lookMats): a part per texture set — off / textures off for it: the plain grey;
// its colour × colour map, its opacity (the opacity map, the colour map's alpha); Flat — unlit
void Renderer::syncTextured(const Item& it, std::set<QString>& usedTex, std::set<QString>& usedSets, const PolyMesh* P, std::vector<GpuTexMesh>* out) {
  auto sets = looks::setsOf(it);
  QJsonObject L = looks::lookOf(it); QJsonArray LS = L.value("sets").toArray();
  TexDraw d = buildTexDraw(P ? *P : *it.poly, sets, !it.raw.value("smoothN").toBool());
  if (keepTexVerts_) *keepTexVerts_ = d.verts;
  GpuTexMesh g; g.itemId = it.id; g.flat = L.value("flat").toBool();
  std::vector<uint32_t> idx;
  for (size_t i = 0; i < d.bySet.size(); i++) {
    if (d.bySet[i].empty()) continue;
    TexPart p; p.first = uint32_t(idx.size()); p.count = uint32_t(d.bySet[i].size()); idx.insert(idx.end(), d.bySet[i].begin(), d.bySet[i].end());
    QString mk, ak;
    bool on = int(i) < LS.size() && LS[int(i)].toObject().value("on").toBool(true);
    if (on) {
      mk = textureFor(looks::resolve(it, int(i), 0), true); ak = textureFor(looks::resolve(it, int(i), 1), false);
      bool hasMap = !mk.isEmpty(), hasAlpha = !ak.isEmpty(), mapAlpha = hasMap && looks::setAlpha(it, int(i));
      p.transparent = hasAlpha || mapAlpha;
      p.flags = (hasMap ? 1 : 0) | (hasAlpha ? 2 : 0) | (mapAlpha ? 4 : 0) | (p.transparent ? 8 : 0);
      toLinear(QColor(looks::setColor(it, int(i), hasMap)), p.color);
    } else toLinear(QColor(QRgb(looks::HEAD_GREY)), p.color);
    if (!mk.isEmpty()) usedTex.insert(mk);
    if (!ak.isEmpty()) usedTex.insert(ak);
    p.ds = descriptorFor(mk, ak); usedSets.insert(mk + "|" + ak);
    g.parts.push_back(p);
  }
  g.vb = makeBuffer(d.verts.data(), d.verts.size() * sizeof(TexVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  g.ib = makeBuffer(idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  (out ? *out : texMeshes_).push_back(std::move(g));
}

// a checker of 8 × 8 labelled cells, A1…H8 (one period: the UV scaled by density / 8 — 64 cells across the tile by
// default): four colours, no two neighbours alike; the labels read wrong where a part is turned over or stretched
QString Renderer::checkerTex() {
  const QString key = "uv-checker#l2";
  auto it = texCache_.find(key);
  if (it == texCache_.end()) {
    const int C = 128, N = 8;
    QImage img(C * N, C * N, QImage::Format_RGBA8888);
    QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
    // (a checker: black cells between coloured ones — purple, red, green in turn, no two touching corners alike)
    const QColor dark(0x2c, 0x2c, 0x32), pal[3] = {QColor(0xb3, 0x8c, 0xe6), QColor(0xe8, 0x86, 0x78), QColor(0x7d, 0xd6, 0xa8)};
    QFont f("Segoe UI"); f.setPixelSize(46); f.setBold(true); p.setFont(f);
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
      const QRect cell(c * C, r * C, C, C); const bool lit = (c + r) % 2 == 0;
      p.fillRect(cell, lit ? pal[((c + 3 * r) / 2) % 3] : dark);
      p.setPen(lit ? QColor(0x24, 0x20, 0x22) : QColor(0xc8, 0xc8, 0xd0)); p.drawText(cell, Qt::AlignCenter, QString("%1%2").arg(QChar('A' + r)).arg(c + 1));
    }
    p.setPen(QPen(QColor(0, 0, 0, 70), 2)); for (int k = 0; k <= N; k++) { p.drawLine(QPointF(k * C, 0), QPointF(k * C, C * N)); p.drawLine(QPointF(0, k * C), QPointF(C * N, k * C)); }
    p.end();
    it = texCache_.emplace(key, uploadImage(img, true)).first;
  }
  return it->second.view ? key : QString();
}

void Renderer::syncChecker(const Item& it, std::set<QString>& usedTex, std::set<QString>& usedSets) {
  auto sets = looks::setsOf(it);
  TexDraw d = buildTexDraw(*it.poly, sets, !it.raw.value("smoothN").toBool());
  const float k = float(w_->uvo.density / 8); for (auto& v : d.verts) { v.uv[0] *= k; v.uv[1] *= k; }
  if (keepTexVerts_) *keepTexVerts_ = d.verts;
  GpuTexMesh g; g.itemId = it.id;
  std::vector<uint32_t> idx; for (auto& s : d.bySet) idx.insert(idx.end(), s.begin(), s.end());
  TexPart p; p.first = 0; p.count = uint32_t(idx.size()); p.flags = 1; toLinear(Qt::white, p.color);
  QString mk = checkerTex(); if (!mk.isEmpty()) usedTex.insert(mk);
  p.ds = descriptorFor(mk, QString()); usedSets.insert(mk + "|");
  g.parts.push_back(p);
  g.vb = makeBuffer(d.verts.data(), d.verts.size() * sizeof(TexVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  g.ib = makeBuffer(idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  texMeshes_.push_back(std::move(g));
}

// the element under the pointer in the UV Editor (Viewport::uvHoverDraw)
void Renderer::syncUvHover() {
  const auto& D = w_->uvHoverDraw();
  for (auto& g : uvHovLines_) retire(g.vb); for (auto& m : uvHovTris_) retire(m); retire(uvHovPts_);
  uvHovLines_.clear(); uvHovTris_.clear(); uvHovPtCount_ = 0;
  for (auto& l : D.lines) { LineBatch b; b.width = l.w; float c[4]; toLinear(l.c, c); for (auto& p : l.p) b.verts.push_back({{p[0], p[1], 0}, {c[0], c[1], c[2], 1}}); GpuLines g; uploadLines(g, b); uvHovLines_.push_back(g); }
  for (auto& t : D.tris) { if (t.p.empty()) continue; MeshDraw d;
    for (size_t i = 0; i < t.p.size(); i++) { MeshVertex v{}; v.p[0] = t.p[i][0]; v.p[1] = t.p[i][1]; v.n[2] = 1; float c[4]; toLinear(i / 3 < t.c.size() ? t.c[i / 3] : QColor(Qt::white), c);   // (each triangle its colour — the preview's veil dark)
      v.c[0] = c[0]; v.c[1] = c[1]; v.c[2] = c[2]; d.verts.push_back(v); d.tris.push_back(uint32_t(i)); }
    float col[4] = {1, 1, 1, t.op}; GpuMesh g = uploadMesh(0, d, col, nullptr); g.transparent = true; uvHovTris_.push_back(g); }
  std::vector<PointVertex> pts; for (auto& s : D.pts) for (size_t i = 0; i < s.p.size(); i++) { float c[4]; toLinear(s.c[i], c); pts.push_back({{s.p[i][0], s.p[i][1], 0}, {c[0], c[1], c[2], s.size}, 5.f}); }
  uvHovPts_ = makeBuffer(pts.data(), pts.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); uvHovPtCount_ = uint32_t(pts.size());
}

// the UV Editor's things (Viewport::uvDraw) to the GPU: in UV units (z 0)
void Renderer::syncUv() {
  const auto& D = w_->uvDraw();
  for (auto& g : uvGrid_) retire(g.vb); for (auto& g : uvLines_) retire(g.vb); for (auto& m : uvTris_) retire(m); retire(uvPts_); retire(uvQuad_);
  uvGrid_.clear(); uvLines_.clear(); uvTris_.clear(); uvPtCount_ = 0; uvTexDs_ = VK_NULL_HANDLE;
  auto lines = [&](const Viewport::UvDraw::Lines& l, std::vector<GpuLines>& out) {
    LineBatch b; b.width = l.w; float c[4]; toLinear(l.c, c);
    for (auto& p : l.p) b.verts.push_back({{p[0], p[1], 0}, {c[0], c[1], c[2], 1}});
    GpuLines g; uploadLines(g, b); out.push_back(g);
  };
  for (auto& l : D.grid) lines(l, uvGrid_);
  for (auto& l : D.lines) lines(l, uvLines_);
  for (auto& t : D.tris) {
    if (t.p.empty()) continue; MeshDraw d;
    for (size_t i = 0; i < t.p.size(); i++) { MeshVertex v{}; v.p[0] = t.p[i][0]; v.p[1] = t.p[i][1]; v.p[2] = 0; v.n[2] = 1; float c[4]; toLinear(i / 3 < t.c.size() ? t.c[i / 3] : QColor(Qt::white), c);
      v.c[0] = c[0]; v.c[1] = c[1]; v.c[2] = c[2]; d.verts.push_back(v); d.tris.push_back(uint32_t(i)); }
    float col[4] = {1, 1, 1, t.op}; GpuMesh g = uploadMesh(0, d, col, nullptr); g.transparent = true; uvTris_.push_back(g);
  }
  std::vector<PointVertex> pts;
  for (auto& s : D.pts) for (size_t i = 0; i < s.p.size(); i++) { float c[4]; toLinear(s.c[i], c); pts.push_back({{s.p[i][0], s.p[i][1], 0}, {c[0], c[1], c[2], s.size}, 5.f}); }
  uvPts_ = makeBuffer(pts.data(), pts.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); uvPtCount_ = uint32_t(pts.size());
  uvOp_ = D.op; uvChk_ = false;
  if (D.checker) { const QString mk = checkerTex(); if (!mk.isEmpty()) {           // (the labelled checker under the UV, its cells as on the mesh)
      uvTexDs_ = descriptorFor(mk, QString()); uvChk_ = true; const float k = float(w_->uvo.density / 8);
      const float C[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}; std::vector<TexVertex> q;
      for (auto& c : C) q.push_back({{c[0], c[1], 0}, {0, 0, 1}, {c[0] * k, c[1] * k}});
      uvQuad_ = makeBuffer(q.data(), q.size() * sizeof(TexVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); } }
  // the texture under the UV: the picked UV Mat's map, else the mesh's first one
  if (D.texMesh >= 0) if (const Item* it = w_->scene()->item(D.texMesh); it && it->poly) {
    auto sets = looks::setsOf(*it);
    int only = -1; if (D.texMat != -2) for (int i = 0; i < int(sets.size()); i++) if (sets[size_t(i)].mat == D.texMat) only = i;
    for (int i = 0; i < int(sets.size()) && !uvTexDs_; i++) { if (only >= 0 && i != only) continue;
      QString mk = textureFor(looks::resolve(*it, i, 0), true); if (mk.isEmpty()) continue;
      uvTexDs_ = descriptorFor(mk, QString()); const bool fy = it->poly->flipY;
      const float C[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}; std::vector<TexVertex> q;
      for (auto& c : C) q.push_back({{c[0], c[1], 0}, {0, 0, 1}, {c[0], fy ? 1 - c[1] : c[1]}});
      uvQuad_ = makeBuffer(q.data(), q.size() * sizeof(TexVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); }
  }
}

void Renderer::initSwapChainResources() {}
void Renderer::releaseSwapChainResources() {}

void Renderer::releaseResources() {
  VkDevice dev = w_->device();
  for (auto& m : static_) if (!m.cached) retire(m);
  for (auto& m : shells_) retire(m);
  for (auto& m : tubes_) retire(m);
  for (auto& m : crowns_) retire(m);
  for (auto& m : ribs_) retire(m);
  for (auto& m : ref_) retire(m);
  for (auto& [id, m] : walls_) retire(m);
  for (auto& m : texMeshes_) if (!m.cached) { retire(m.vb); retire(m.ib); }
  dropMeshCaches(); retire(vpts_.buf); vpts_ = {};
  for (auto& m : topoTris_) retire(m); for (auto& g : topoLines_) retire(g.vb); retire(topoPts_); retire(topoPtsX_); retire(topoPtsN_); topoPtNCount_ = 0; topoPtXCount_ = 0; topoTris_.clear(); topoTrisLift_.clear(); topoLines_.clear(); topoPtCount_ = 0; topoKey_ = {-1, -1, -1};
  for (auto& g : uvGrid_) retire(g.vb); for (auto& g : uvLines_) retire(g.vb); for (auto& m : uvTris_) retire(m); retire(uvPts_); retire(uvQuad_);
  uvGrid_.clear(); uvLines_.clear(); uvTris_.clear(); uvPtCount_ = 0; uvTexDs_ = VK_NULL_HANDLE; uvSeen_.clear(); checkerSig_.clear();
  for (auto& g : uvHovLines_) retire(g.vb); for (auto& m : uvHovTris_) retire(m); retire(uvHovPts_); uvHovLines_.clear(); uvHovTris_.clear(); uvHovPtCount_ = 0; uvHovSeen_.clear();
  static_.clear(); shells_.clear(); tubes_.clear(); crowns_.clear(); ribs_.clear(); ref_.clear(); walls_.clear(); texMeshes_.clear();
  for (auto& [k, ds] : texSets_) deadTex_.push_back({GpuTex{}, ds, 0});
  for (auto& [k, t] : texCache_) deadTex_.push_back({t, VK_NULL_HANDLE, 0});
  deadTex_.push_back({white_, VK_NULL_HANDLE, 0}); white_ = {};
  texSets_.clear(); texCache_.clear(); texRev_ = -1;
  for (GpuLines* g : {&curves_, &floor_, &axes_, &rings_, &ringsBack_, &lasso_, &joints_, &gizmo3_, &gizmo2_, &guides_, &masks_, &tubeGuides_, &tubeLoops_, &tubeWire_, &edgeSel_, &edgeHov_, &ribLines_, &paneFrames_, &paneActive_, &zone_}) retire(g->vb);
  retire(points_);
  collectGarbage(true);
  tubesRev_ = crownsRev_ = ribsRev_ = -1; crownStatic_ = -2;
  retire(pins_);
  for (VkPipeline* p : {&meshPipe_, &meshBlendPipe_, &meshDepthPipe_, &linePipe_, &lineTopPipe_, &pointPipe_, &meshBlendWritePipe_, &pointDepthPipe_}) if (*p) { df_->vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
  retire(rpVb_);
  for (VkPipeline* p : {&texPipe_, &texBlendPipe_, &rpBehindPipe_, &rpOverPipe_}) if (*p) { df_->vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
  for (VkPipelineLayout* l : {&meshLayout_, &lineLayout_, &pointLayout_, &texLayout_}) if (*l) { df_->vkDestroyPipelineLayout(dev, *l, nullptr); *l = VK_NULL_HANDLE; }
  if (sampler_) { df_->vkDestroySampler(dev, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
  if (texPool_) { df_->vkDestroyDescriptorPool(dev, texPool_, nullptr); texPool_ = VK_NULL_HANDLE; }
  if (texSetLayout_) { df_->vkDestroyDescriptorSetLayout(dev, texSetLayout_, nullptr); texSetLayout_ = VK_NULL_HANDLE; }
  if (cache_) { df_->vkDestroyPipelineCache(dev, cache_, nullptr); cache_ = VK_NULL_HANDLE; }
  staticRev_ = overlayRev_ = -1;
}

Renderer::GpuMesh Renderer::uploadMesh(int itemId, const MeshDraw& d, const float color[4], const std::vector<LineVertex>* wire) {
  GpuMesh g; g.itemId = itemId; std::memcpy(g.color, color, sizeof g.color);
  g.vb = makeBuffer(d.verts.data(), d.verts.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  g.ib = makeBuffer(d.tris.data(), d.tris.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  g.triCount = uint32_t(d.tris.size());
  if (wire && !wire->empty()) { g.wb = makeBuffer(wire->data(), wire->size() * sizeof(LineVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); g.wireCount = uint32_t(wire->size()); }
  return g;
}

void Renderer::uploadLines(GpuLines& g, const LineBatch& b) {
  retire(g.vb);
  if (b.verts.empty()) { g.vb = {}; g.count = 0; return; }
  g.vb = makeBuffer(b.verts.data(), b.verts.size() * sizeof(LineVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  g.count = uint32_t(b.verts.size()); g.width = b.width; g.bias = b.bias;
}

// the meshes: the head (imported meshes marked as the head), other imported meshes, converted meshes; floor and axes
Renderer::ViewOpts Renderer::viewOpts() const {
  QJsonObject v = w_->scene()->fileRaw.value("settings").toObject().value("view").toObject(); ViewOpts o;
  auto num = [&](const char* k, double def) { double x = v.value(k).toDouble(def); return x >= 0 && x <= 1 ? x : def; };
  o.xray = num("xray", 0.2); o.surfOp = std::max(0.05, num("surfOpacity", 0.85)); o.modOp = std::max(0.05, num("modOpacity", 0.45));
  o.anchorOp = num("anchorOpacity", 1); o.curveOp = num("curveOpacity", 1); o.guideOp = num("guideOpacity", 1); o.gridOp = num("gridOpacity", 1);
  o.showGrid = v.value("showGrid").toBool(false); o.showLoops = v.value("showLoops").toBool(false);   // (the walls' lines: off unless switched on in Display) o.showMods = v.value("showMods").toBool(true); o.showMeshWire = v.value("showMeshWire").toBool(true);
  return o;
}

// the heavy meshes' LOD copies: placed by the mesh's transform, its hidden polygroups left out, plain and textured as
// the mesh itself is shown (not the active mesh of a component mode, nor one wearing the UV checker — those stay whole)
void Renderer::syncLods(std::set<QString>& usedTex, std::set<QString>& usedSets) {
  ProfScope prof_("syncLods");
  for (auto& [id, c] : lodGpu_) c.used = false;
  const Scene& sc = *w_->scene(); const bool topoRoom = w_->room() == "topo";
  staticNav_.clear(); texNav_.clear();
  std::set<int> withLod;
  for (const GpuMesh& m : static_) {
    const Item* it = sc.item(m.itemId); auto lod = it ? w_->lodOf(m.itemId) : nullptr;
    if (!lod || (topoRoom && (w_->topoTwoTone(m.itemId) || (w_->uvDispShown(m.itemId) && w_->uvo.c3d)))) continue;
    Affine M = meshmods::meshMatrix(sc, *it); const bool flat = !it->raw.value("smoothN").toBool();
    const bool textured = m.textured;
    QByteArray key; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) key += QByteArray::number(M.L[r][c], 'g', 9) + ",";
    key += QByteArray::number(M.t.x, 'g', 9) + "," + QByteArray::number(M.t.y, 'g', 9) + "," + QByteArray::number(M.t.z, 'g', 9) + "|" + QJsonDocument(it->raw.value("hidGroups").toArray()).toJson(QJsonDocument::Compact)
         + (flat ? "f" : "s") + (textured ? QJsonDocument(looks::lookOf(*it)).toJson(QJsonDocument::Compact) + QByteArray::number(looks::revision()) : QByteArray("-")) + QByteArray::number(int(topoRoom)) + (m.paint ? "|pp" : "");
    LodGpu& L = lodGpu_[m.itemId];
    if (L.lod != lod || L.key != key) {
      retire(L.plain); for (auto& g : L.tex) { retire(g.vb); retire(g.ib); } L.tex.clear(); L.usedTex.clear(); L.usedSets.clear();
      PolyMesh W; W.pos.reserve(lod->pos.size()); for (auto& p : lod->pos) W.pos.push_back(M.apply(p));
      std::set<int> hid; for (const QJsonValue& v : it->raw.value("hidGroups").toArray()) hid.insert(v.toInt());
      const bool uv = lod->cornerUV.size() == lod->faceVerts.size(), fm = lod->faceMat.size() == size_t(lod->faceCount());
      for (int f = 0; f < lod->faceCount(); f++) { if (!hid.empty() && size_t(f) < lod->faceGroup.size() && hid.count(lod->faceGroup[size_t(f)])) continue;
        int s = lod->faceStart[size_t(f)], e = lod->faceStart[size_t(f) + 1]; W.addFace(&lod->faceVerts[size_t(s)], e - s);
        if (uv) W.cornerUV.insert(W.cornerUV.end(), lod->cornerUV.begin() + s, lod->cornerUV.begin() + e); if (fm) W.faceMat.push_back(lod->faceMat[size_t(f)]); }
      W.matNames = lod->matNames; W.matMaps = lod->matMaps; W.matColor = lod->matColor; W.matAlpha = lod->matAlpha; W.flipY = lod->flipY;
      std::vector<std::array<float, 3>> lin; if (m.paint && lod->vcol.size() == lod->pos.size()) for (auto& c : lod->vcol) lin.push_back({std::pow(c[0], 2.2f), std::pow(c[1], 2.2f), std::pow(c[2], 2.2f)});   // (the polypaint as the mesh shows it)
      MeshDraw d = buildMeshDraw(W, false, flat, lin.empty() ? nullptr : &lin); if (textured && !topoRoom) d.tris.clear();   // (shaded as the mesh itself)
      L.plain = uploadMesh(m.itemId, d, m.color, nullptr);
      if (textured) syncTextured(*it, L.usedTex, L.usedSets, &W, &L.tex);
      L.lod = lod; L.key = key;
    }
    L.used = true; usedTex.insert(L.usedTex.begin(), L.usedTex.end()); usedSets.insert(L.usedSets.begin(), L.usedSets.end());
    GpuMesh g = L.plain; std::memcpy(g.color, m.color, sizeof g.color); g.textured = m.textured; g.wireOn = false; g.hl = m.hl; g.transparent = m.transparent; g.mat = m.mat; g.unlit = m.unlit;
    for (auto& tg : L.tex) { tg.mat = m.mat; tg.unlit = m.unlit; }
    staticNav_.push_back(g); withLod.insert(m.itemId);
  }
  if (withLod.empty()) { staticNav_.clear(); }
  else {
    for (const GpuMesh& m : static_) if (!withLod.count(m.itemId)) staticNav_.push_back(m);
    for (const GpuTexMesh& g : texMeshes_) if (!withLod.count(g.itemId)) texNav_.push_back(g);
    for (int id : withLod) for (auto& g : lodGpu_[id].tex) texNav_.push_back(g);
  }
  for (auto c = lodGpu_.begin(); c != lodGpu_.end();) { if (c->second.used) { ++c; continue; } retire(c->second.plain); for (auto& g : c->second.tex) { retire(g.vb); retire(g.ib); } c = lodGpu_.erase(c); }
}

// ---------- a drag: the GPU copies patched where the vertices moved ----------
void Renderer::adjOf(Adj& a, const PolyMesh& P) {
  if (!a.vOff.empty()) return;
  const size_t nv = P.pos.size(), nc = P.faceVerts.size(); a.vOff.assign(nv + 1, 0); a.vCorner.resize(nc); a.faceOf.resize(nc);
  for (size_t k = 0; k < nc; k++) a.vOff[size_t(P.faceVerts[k]) + 1]++;
  for (size_t v = 0; v < nv; v++) a.vOff[v + 1] += a.vOff[v];
  std::vector<uint32_t> at(a.vOff.begin(), a.vOff.end() - 1); for (size_t k = 0; k < nc; k++) a.vCorner[at[size_t(P.faceVerts[k])]++] = uint32_t(k);
  for (int f = 0; f < P.faceCount(); f++) for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) a.faceOf[size_t(k)] = f;
}

// the elements (stride bytes each, at data) of these indices into dst: one staging buffer, a copy region per run
void Renderer::patchBuffer(const Buf& dst, const void* data, size_t stride, std::vector<uint32_t> idx) {
  if (!dst.buf || idx.empty()) return;
  // (every element, in order — a whole buffer: copied as it is, no sorting, no packing)
  const bool whole = idx.front() == 0 && idx.back() == idx.size() - 1 && std::is_sorted(idx.begin(), idx.end());
  std::vector<VkBufferCopy> regions; QByteArray packed;
  if (whole) regions.push_back({0, 0, VkDeviceSize(idx.size()) * stride});
  else {
    std::sort(idx.begin(), idx.end()); idx.erase(std::unique(idx.begin(), idx.end()), idx.end()); packed.reserve(qsizetype(idx.size() * stride));
    for (size_t i = 0; i < idx.size();) {
      size_t j = i; while (j + 1 < idx.size() && idx[j + 1] == idx[j] + 1) j++;
      const VkDeviceSize n = VkDeviceSize(idx[j] - idx[i] + 1) * stride;
      regions.push_back({VkDeviceSize(packed.size()), VkDeviceSize(idx[i]) * stride, n});
      packed.append(static_cast<const char*>(data) + size_t(idx[i]) * stride, qsizetype(n)); i = j + 1;
    }
  }
  VkDevice dev = w_->device();
  Buf st = whole ? makeBuffer(data, VkDeviceSize(idx.size()) * stride, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) : makeBuffer(packed.constData(), VkDeviceSize(packed.size()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT); if (!st.buf) return;
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = w_->graphicsCommandPool(); ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
  VkCommandBuffer cb; df_->vkAllocateCommandBuffers(dev, &ca, &cb);
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  df_->vkBeginCommandBuffer(cb, &cbi);
  df_->vkCmdCopyBuffer(cb, st.buf, dst.buf, uint32_t(regions.size()), regions.data());
  df_->vkEndCommandBuffer(cb);
  VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
  df_->vkQueueSubmit(w_->graphicsQueue(), 1, &sub, VK_NULL_HANDLE); df_->vkQueueWaitIdle(w_->graphicsQueue());
  df_->vkFreeCommandBuffers(dev, w_->graphicsCommandPool(), 1, &cb);
  df_->vkDestroyBuffer(dev, st.buf, nullptr); df_->vkFreeMemory(dev, st.mem, nullptr);
}

// the active mesh's every vertex as points (Vertex mode): made when the mesh / the selection changes, patched by a drag
void Renderer::syncVertPts() {
  ProfScope prof_("syncVertPts");
  const auto& D = w_->topoDraw(); const Item* it = D.pointsOf >= 0 ? w_->scene()->item(D.pointsOf) : nullptr;
  if (!it || !it->poly) { if (vpts_.buf.buf) retire(vpts_.buf); vpts_ = {}; return; }
  const PolyMesh& P = *it->poly; const auto& S = w_->topoSel();
  const QByteArray hid = meshmods::hiddenSig(*it);
  if (vpts_.m == D.pointsOf && vpts_.raw == it->poly.get() && vpts_.selVer == S.ver && vpts_.n == P.pos.size() && vpts_.hid == hid && vpts_.buf.buf) return;
  retire(vpts_.buf); vpts_ = {}; vpts_.m = D.pointsOf; vpts_.raw = it->poly.get(); vpts_.selVer = S.ver; vpts_.n = P.pos.size(); vpts_.hid = hid;
  float wc[4], bc[4]; toLinear(Qt::white, wc); toLinear(Qt::black, bc); const bool mine = S.id == D.pointsOf;
  // (a hidden polygroup's / part's vertices not shown: only those of a shown polygon)
  std::vector<char> on; { const PolyMesh& B = it->base ? *it->base : P; const std::vector<char> hf = meshmods::hiddenFaces(*it, B);
    if (!hf.empty() && B.pos.size() == P.pos.size()) { on.assign(P.pos.size(), 0);
      for (int f = 0; f < B.faceCount(); f++) if (size_t(f) >= hf.size() || !hf[size_t(f)]) for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) on[size_t(B.faceVerts[size_t(k)])] = 1; } }
  vpts_.pts.resize(P.pos.size());
  for (size_t v = 0; v < P.pos.size(); v++) { const float* c = mine && S.v.count(int(v)) ? wc : bc; const float sz = on.empty() || on[v] ? 5.f : 0.f; vpts_.pts[v] = {{float(P.pos[v].x), float(P.pos[v].y), float(P.pos[v].z)}, {c[0], c[1], c[2], sz}, 5.f}; }
  vpts_.buf = makeBuffer(vpts_.pts.data(), vpts_.pts.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

// → false: these copies can't follow (made again then)
bool Renderer::patchLive(int m, const std::shared_ptr<PolyMesh>& poly, const PolyMesh* prev, const std::vector<int>& verts, const std::vector<int>& corners) {
  if (!poly) return false;
  const PolyMesh& P = *poly;
  if (prev) { for (auto& [id, c] : plainCache_) if (c.raw == prev) { c.poly = poly; c.raw = poly.get(); }   // (the same content in a new copy: taken over)
              for (auto& [id, c] : texMeshCache_) if (c.raw == prev) { c.poly = poly; c.raw = poly.get(); } }
  bool ok = true;
  if (prev && vpts_.raw == prev) vpts_.raw = poly.get();
  if (vpts_.m == m && vpts_.raw == poly.get() && vpts_.buf.buf && !verts.empty()) {   // (the vertices' points)
    ProfScope pv("patchLive.pts");
    std::vector<uint32_t> ch; for (int v : verts) if (size_t(v) < vpts_.pts.size()) { const Vec3& p = P.pos[size_t(v)]; vpts_.pts[size_t(v)].p[0] = float(p.x); vpts_.pts[size_t(v)].p[1] = float(p.y); vpts_.pts[size_t(v)].p[2] = float(p.z); ch.push_back(uint32_t(v)); }
    patchBuffer(vpts_.buf, vpts_.pts.data(), sizeof(PointVertex), ch);
  }
  // the polygons round the moved vertices; for smooth shading the vertices of those polygons (their normals change)
  auto facesOf = [&](const Adj& a) { std::vector<int> fs; for (int v : verts) if (size_t(v) + 1 < a.vOff.size()) for (uint32_t i = a.vOff[size_t(v)]; i < a.vOff[size_t(v) + 1]; i++) fs.push_back(a.faceOf[a.vCorner[i]]);
    std::sort(fs.begin(), fs.end()); fs.erase(std::unique(fs.begin(), fs.end()), fs.end()); return fs; };
  auto faceN = [&](int f) { Vec3 n; int s = P.faceStart[size_t(f)], e = P.faceStart[size_t(f) + 1];
    for (int k = s + 1; k < e - 1; k++) { const Vec3 &a = P.pos[size_t(P.faceVerts[size_t(s)])], &b = P.pos[size_t(P.faceVerts[size_t(k)])], &c = P.pos[size_t(P.faceVerts[size_t(k + 1)])]; n += (b - a).cross(c - a); }
    return n.normalized(); };
  auto vertN = [&](const Adj& a, int v) { Vec3 n;                 // (as buildMeshDraw: the fan triangles holding it, area-weighted)
    for (uint32_t i = a.vOff[size_t(v)]; i < a.vOff[size_t(v) + 1]; i++) { int f = a.faceOf[a.vCorner[i]], s = P.faceStart[size_t(f)], e = P.faceStart[size_t(f) + 1];
      for (int k = s + 1; k < e - 1; k++) { int x = P.faceVerts[size_t(s)], y = P.faceVerts[size_t(k)], z = P.faceVerts[size_t(k + 1)]; if (x != v && y != v && z != v) continue;
        n += (P.pos[size_t(y)] - P.pos[size_t(x)]).cross(P.pos[size_t(z)] - P.pos[size_t(x)]); } }
    return n.normalized(); };
  // (most of the mesh moved — a high-poly one dragged whole: every normal in one pass over the polygons, the buffers
  // replaced whole, instead of each vertex's polygons looked up one by one)
  const bool bulk = verts.size() * 4 > P.pos.size();
  std::vector<Vec3> allN;
  auto allVertN = [&] { if (!allN.empty()) return; allN.assign(P.pos.size(), Vec3());
    for (int f = 0; f < P.faceCount(); f++) { const int s = P.faceStart[size_t(f)], e = P.faceStart[size_t(f) + 1];
      for (int k = s + 1; k < e - 1; k++) { const int x = P.faceVerts[size_t(s)], y = P.faceVerts[size_t(k)], z = P.faceVerts[size_t(k + 1)];
        const Vec3 n = (P.pos[size_t(y)] - P.pos[size_t(x)]).cross(P.pos[size_t(z)] - P.pos[size_t(x)]); allN[size_t(x)] += n; allN[size_t(y)] += n; allN[size_t(z)] += n; } }
    for (auto& n : allN) n = n.normalized(); };
  auto everyIndex = [](size_t n) { std::vector<uint32_t> ch(n); std::iota(ch.begin(), ch.end(), 0u); return ch; };
  if (auto it = plainCache_.find(m); bulk && it != plainCache_.end() && it->second.keep && it->second.raw == poly.get()) {
    ProfScope pb("patchLive.bulk");
    PlainC& c = it->second;
    // (all of it moved by one offset — the gizmo moving the whole mesh: its normals stay, only the places change)
    bool shift = prev && prev->pos.size() == P.pos.size() && verts.size() == P.pos.size() && !P.pos.empty();
    if (shift) { const Vec3 d0 = P.pos[0] - prev->pos[0]; const double tol = 1e-7 * (1 + d0.length());
      for (size_t i = 0; i < 64 && shift; i++) { const size_t v = (P.pos.size() - 1) * i / 63; shift = (P.pos[v] - prev->pos[v] - d0).length() <= tol; } }
    if (shift) { if (c.flat ? c.verts.size() != P.faceVerts.size() : c.verts.size() != P.pos.size()) return false;
      if (c.flat) { for (size_t k = 0; k < P.faceVerts.size(); k++) { const Vec3& p = P.pos[size_t(P.faceVerts[k])]; MeshVertex& mv = c.verts[k]; mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); } }
      else for (size_t v = 0; v < P.pos.size(); v++) { const Vec3& p = P.pos[v]; MeshVertex& mv = c.verts[v]; mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); } }
    else if (c.flat) { if (c.verts.size() != P.faceVerts.size()) return false;
      for (int f = 0; f < P.faceCount(); f++) { const Vec3 n = faceN(f); for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) { const Vec3& p = P.pos[size_t(P.faceVerts[size_t(k)])];
        MeshVertex& mv = c.verts[size_t(k)]; mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); mv.n[0] = float(n.x); mv.n[1] = float(n.y); mv.n[2] = float(n.z); } } }
    else { if (c.verts.size() != P.pos.size()) return false; allVertN();
      for (size_t v = 0; v < P.pos.size(); v++) { const Vec3& p = P.pos[v]; const Vec3& n = allN[v]; MeshVertex& mv = c.verts[v];
        mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); mv.n[0] = float(n.x); mv.n[1] = float(n.y); mv.n[2] = float(n.z); } }
    patchBuffer(c.g.vb, c.verts.data(), sizeof(MeshVertex), everyIndex(c.verts.size()));
    if (!c.lv.empty() && c.g.wb.buf && c.wire.size() == c.lv.size()) { for (size_t j = 0; j < c.wire.size(); j++) std::memcpy(c.lv[j].p, c.verts[c.wire[j]].p, sizeof c.lv[j].p);
      patchBuffer(c.g.wb, c.lv.data(), sizeof(LineVertex), everyIndex(c.lv.size())); }
  } else if (auto it = plainCache_.find(m); it != plainCache_.end() && !verts.empty()) {
    ProfScope pp("patchLive.plain");
    PlainC& c = it->second;
    if (!c.keep || c.raw != poly.get()) ok = false;
    else {
      adjOf(c.adj, P); std::vector<uint32_t> ch; auto fs = facesOf(c.adj);
      if (c.flat) { if (c.verts.size() != P.faceVerts.size()) return false;
        for (int f : fs) { Vec3 n = faceN(f); for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) { const Vec3& p = P.pos[size_t(P.faceVerts[size_t(k)])];
          MeshVertex& mv = c.verts[size_t(k)]; mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); mv.n[0] = float(n.x); mv.n[1] = float(n.y); mv.n[2] = float(n.z); ch.push_back(uint32_t(k)); } } }
      else { if (c.verts.size() != P.pos.size()) return false;
        std::vector<int> vs; for (int f : fs) for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) vs.push_back(P.faceVerts[size_t(k)]);
        std::sort(vs.begin(), vs.end()); vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
        for (int v : vs) { Vec3 n = vertN(c.adj, v); const Vec3& p = P.pos[size_t(v)]; MeshVertex& mv = c.verts[size_t(v)];
          mv.p[0] = float(p.x); mv.p[1] = float(p.y); mv.p[2] = float(p.z); mv.n[0] = float(n.x); mv.n[1] = float(n.y); mv.n[2] = float(n.z); ch.push_back(uint32_t(v)); } }
      patchBuffer(c.g.vb, c.verts.data(), sizeof(MeshVertex), ch);
      if (!c.lv.empty() && c.g.wb.buf) {                          // (its wire: the line ends at those vertices)
        if (c.wOff.empty()) { c.wOff.assign(c.verts.size() + 1, 0); for (uint32_t i : c.wire) c.wOff[i + 1]++; for (size_t i = 0; i < c.verts.size(); i++) c.wOff[i + 1] += c.wOff[i];
          c.wSlot.resize(c.wire.size()); std::vector<uint32_t> at(c.wOff.begin(), c.wOff.end() - 1); for (size_t j = 0; j < c.wire.size(); j++) c.wSlot[at[c.wire[j]]++] = uint32_t(j); }
        std::vector<uint32_t> wch; for (uint32_t i : ch) for (uint32_t s = c.wOff[i]; s < c.wOff[i + 1]; s++) { uint32_t j = c.wSlot[s]; std::memcpy(c.lv[j].p, c.verts[i].p, sizeof c.lv[j].p); wch.push_back(j); }
        patchBuffer(c.g.wb, c.lv.data(), sizeof(LineVertex), wch);
      }
    }
  }
  if (auto it = texMeshCache_.find(m); bulk && it != texMeshCache_.end() && it->second.keep && it->second.raw == poly.get() && it->second.verts.size() == P.faceVerts.size()) {
    TexC& c = it->second; if (!c.flat) allVertN(); const bool uv = P.cornerUV.size() == P.faceVerts.size();
    for (int f = 0; f < P.faceCount(); f++) { const Vec3 fn = c.flat ? faceN(f) : Vec3();
      for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) { const int v = P.faceVerts[size_t(k)]; const Vec3& p = P.pos[size_t(v)]; const Vec3& n = c.flat ? fn : allN[size_t(v)];
        TexVertex& tv = c.verts[size_t(k)]; tv.p[0] = float(p.x); tv.p[1] = float(p.y); tv.p[2] = float(p.z); tv.n[0] = float(n.x); tv.n[1] = float(n.y); tv.n[2] = float(n.z);
        if (uv) { const auto& u = P.cornerUV[size_t(k)]; tv.uv[0] = u[0] * c.uvScale; tv.uv[1] = (P.flipY ? 1 - u[1] : u[1]) * c.uvScale; } } }
    patchBuffer(c.g.vb, c.verts.data(), sizeof(TexVertex), everyIndex(c.verts.size()));
  } else if (auto it = texMeshCache_.find(m); it != texMeshCache_.end() && (!verts.empty() || !corners.empty())) {
    TexC& c = it->second;
    if (!c.keep || c.raw != poly.get() || c.verts.size() != P.faceVerts.size()) ok = false;
    else {
      adjOf(c.adj, P); std::vector<uint32_t> ch; auto fs = facesOf(c.adj);
      std::map<int, Vec3> vn; if (!c.flat) for (int f : fs) for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) { int v = P.faceVerts[size_t(k)]; if (!vn.count(v)) vn[v] = vertN(c.adj, v); }
      std::set<int> fset(fs.begin(), fs.end());
      if (!c.flat) for (auto& [v, n] : vn) for (uint32_t i = c.adj.vOff[size_t(v)]; i < c.adj.vOff[size_t(v) + 1]; i++) fset.insert(c.adj.faceOf[c.adj.vCorner[i]]);
      for (int f : fset) { Vec3 fn = c.flat ? faceN(f) : Vec3();
        for (int k = P.faceStart[size_t(f)]; k < P.faceStart[size_t(f) + 1]; k++) { int v = P.faceVerts[size_t(k)]; const Vec3& p = P.pos[size_t(v)];
          Vec3 n = c.flat ? fn : (vn.count(v) ? vn[v] : Vec3(c.verts[size_t(k)].n[0], c.verts[size_t(k)].n[1], c.verts[size_t(k)].n[2]));
          TexVertex& tv = c.verts[size_t(k)]; tv.p[0] = float(p.x); tv.p[1] = float(p.y); tv.p[2] = float(p.z); tv.n[0] = float(n.x); tv.n[1] = float(n.y); tv.n[2] = float(n.z); ch.push_back(uint32_t(k)); } }
      if (P.cornerUV.size() == P.faceVerts.size()) for (int k : corners) { const auto& uv = P.cornerUV[size_t(k)]; TexVertex& tv = c.verts[size_t(k)];
        tv.uv[0] = uv[0] * c.uvScale; tv.uv[1] = (P.flipY ? 1 - uv[1] : uv[1]) * c.uvScale; ch.push_back(uint32_t(k)); }
      patchBuffer(c.g.vb, c.verts.data(), sizeof(TexVertex), ch);
    }
  }
  return ok;
}

float Renderer::matCode(const Item& it, bool topoRoom) {
  (void)topoRoom;                                               // (both rooms: the mesh's own material)
  if (it.type != ItemType::IMesh || !it.raw.value("mat").isObject() || it.raw.value("matOff").toBool()) return 0;
  QJsonObject m = it.raw.value("mat").toObject(); auto q = [&](const char* k, double d) { return double(std::lround(std::clamp(m.value(k).toDouble(d), 0.0, 1.0) * 255)); };
  return float(1 + q("roughness", 0.7) + q("metalness", 0) * 256 + q("specular", 1) * 65536);
}

void Renderer::dropMeshCaches() {
  for (auto& [id, c] : plainCache_) retire(c.g);
  for (auto& [id, c] : texMeshCache_) { retire(c.g.vb); retire(c.g.ib); }
  plainCache_.clear(); texMeshCache_.clear();
  for (auto& [id, c] : lodGpu_) { retire(c.plain); for (auto& g : c.tex) { retire(g.vb); retire(g.ib); } }
  lodGpu_.clear(); staticNav_.clear(); texNav_.clear();
}

void Renderer::syncStatic() {
  ProfScope prof_("syncStatic");
  for (auto& m : static_) if (!m.cached) retire(m);
  static_.clear();
  for (auto& m : texMeshes_) if (!m.cached) { retire(m.vb); retire(m.ib); }
  texMeshes_.clear();
  for (auto& [id, c] : plainCache_) c.used = false;
  for (auto& [id, c] : texMeshCache_) c.used = false;
  std::set<QString> usedTex, usedSets;
  const Scene& sc = *w_->scene();
  const ViewOpts V = viewOpts();
  for (auto& [id, it] : sc.items) {
    if (!it.poly || !sc.isVisible(it)) continue;
    if (it.raw.contains("collCap")) continue;                   // (a collision capsule: its rings only, as an overlay)
    bool converted = it.type == ItemType::Mesh, imesh = it.type == ItemType::IMesh;
    const bool topoRoom = w_->room() == "topo";
    bool wire = (converted || (imesh && V.showMeshWire && w_->topoWire(it)) || ((it.headTag || imesh) && w_->showWire && !topoRoom)) && !meshmods::wireless(it);   // (W in the Hair room: the head and the imported meshes)
    // (an imported mesh keeps its wire built while it may be shown: switched on / off without a rebuild)
    const bool wireBuilt = imesh ? !meshmods::wireless(it) : wire;
    // (flat faces unless Smooth normals (display) is on; a heavy mesh — no wireframe)
    const bool flat = !it.raw.value("smoothN").toBool();
    const bool checker = topoRoom && w_->uvo.c3d && w_->uvDispShown(id) && uv::has(*it.poly);   // (the object's UV checker — in every mode)
    const bool textured = checker || (it.type == ItemType::IMesh && looks::shown(it) && !(topoRoom && w_->tdisp.noTex));   // (Topology: No textures)
    const bool cacheable = imesh;                                // (an imported mesh: a new PolyMesh whenever it changes)
    // (polypaint: shown with the standard material only — a texture, the checker, a custom material, the silhouette, the
    // polygroups or the UV stretch take its place)
    const bool matOn = topoRoom && it.raw.value("mat").isObject() && !it.raw.value("matOff").toBool();
    const bool paint = imesh && it.poly->vcol.size() == it.poly->pos.size() && !it.poly->vcol.empty() && !textured && !matOn && !it.raw.value("unlit").toBool()
                       && !it.raw.value("pgView").toBool() && !(topoRoom && w_->uvo.s3d && w_->uvDispShown(id));
    if (textured) {
      QByteArray tkey = checker ? "checker" + QByteArray::number(w_->uvo.density) : QJsonDocument(looks::lookOf(it)).toJson(QJsonDocument::Compact) + QByteArray::number(looks::revision());
      tkey += flat ? "|f" : "|s";
      auto c = texMeshCache_.find(id);
      if (cacheable && c != texMeshCache_.end() && c->second.poly.lock() == it.poly && c->second.key == tkey) {
        c->second.used = true; texMeshes_.push_back(c->second.g); texMeshes_.back().mat = matCode(it, topoRoom); texMeshes_.back().unlit = it.raw.value("unlit").toBool(); usedTex.insert(c->second.tex.begin(), c->second.tex.end()); usedSets.insert(c->second.sets.begin(), c->second.sets.end());
      } else {
        std::set<QString> ut, us; std::vector<TexVertex> kept; const bool keep = cacheable && !meshmods::heavy(it);
        keepTexVerts_ = keep ? &kept : nullptr; if (checker) syncChecker(it, ut, us); else syncTextured(it, ut, us); keepTexVerts_ = nullptr;
        usedTex.insert(ut.begin(), ut.end()); usedSets.insert(us.begin(), us.end());
        if (cacheable) { if (c != texMeshCache_.end()) { retire(c->second.g.vb); retire(c->second.g.ib); }
          texMeshes_.back().cached = true; texMeshes_.back().mat = matCode(it, topoRoom); texMeshes_.back().unlit = it.raw.value("unlit").toBool(); TexC e{it.poly, tkey, texMeshes_.back(), ut, us, true};
          e.raw = it.poly.get(); e.keep = keep; e.flat = flat; e.uvScale = checker ? float(w_->uvo.density / 8) : 1.f; e.verts = std::move(kept);
          texMeshCache_[id] = std::move(e); }
      }
    }
    float color[4];
    if ((it.headTag && !topoRoom) || it.type == ItemType::Head) toLinear(QColor(0x9a, 0x9a, 0x9a), color); else itemBaseColor(sc, it, color);
    if (topoRoom && it.type == ItemType::IMesh && it.raw.value("mat").isObject() && !it.raw.value("matOff").toBool()) toLinear(QColor(it.raw.value("mat").toObject().value("color").toString("#9a9a9a")), color);   // (its material: the colour)
    const bool noTris = textured && !topoRoom;                    // (its textured copy instead of the plain grey)
    const QByteArray pkey = QByteArray::number(int(wireBuilt)) + "|" + QByteArray::number(int(flat)) + QByteArray::number(int(noTris)) + QByteArray::number(int(converted)) + (paint ? "|pp" : "");
    if (paint) { color[0] = color[1] = color[2] = 1; }            // (the polypaint'"'"'s own colours)
    auto pc = plainCache_.find(id);
    if (cacheable && pc != plainCache_.end() && pc->second.poly.lock() == it.poly && pc->second.key == pkey) {
      pc->second.used = true; GpuMesh g = pc->second.g; std::memcpy(g.color, color, sizeof color); g.textured = textured; g.paint = paint; g.wireOn = wire; g.mat = matCode(it, topoRoom); g.unlit = it.raw.value("unlit").toBool(); static_.push_back(g); continue;
    }
    std::vector<std::array<float, 3>> lin; if (paint) { lin.reserve(it.poly->vcol.size()); for (auto& c : it.poly->vcol) lin.push_back({std::pow(c[0], 2.2f), std::pow(c[1], 2.2f), std::pow(c[2], 2.2f)}); }   // (sRGB → linear)
    MeshDraw d = buildMeshDraw(*it.poly, wireBuilt, flat, paint ? &lin : nullptr);
    if (noTris) d.tris.clear();
    std::vector<LineVertex> lv;
    if (wireBuilt) {                                           // (converted: 0x0c0c0c at 0.65; imported: 0x151515 at 0.6 × 0.6)
      float c[4]; if (converted) toLinear(QColor(0x0c, 0x0c, 0x0c), c, 0.65f); else toLinear(QColor(0x15, 0x15, 0x15), c, 0.36f);
      lv.reserve(d.wire.size()); for (uint32_t i : d.wire) { const auto& v = d.verts[i]; lv.push_back({{v.p[0], v.p[1], v.p[2]}, {c[0], c[1], c[2], c[3]}}); }
    }
    static_.push_back(uploadMesh(id, d, color, &lv)); static_.back().textured = textured; static_.back().paint = paint; static_.back().wireOn = wire; static_.back().mat = matCode(it, topoRoom); static_.back().unlit = it.raw.value("unlit").toBool();
    if (cacheable) { if (pc != plainCache_.end()) retire(pc->second.g); static_.back().cached = true; PlainC e{it.poly, pkey, static_.back(), true};
      e.raw = it.poly.get(); e.flat = flat; e.keep = !meshmods::heavy(it);
      if (e.keep) { e.verts = std::move(d.verts); e.wire = std::move(d.wire); e.lv = std::move(lv); }
      plainCache_[id] = std::move(e); }
  }
  for (auto c = plainCache_.begin(); c != plainCache_.end();) { if (c->second.used) { ++c; continue; } retire(c->second.g); c = plainCache_.erase(c); }
  for (auto c = texMeshCache_.begin(); c != texMeshCache_.end();) { if (c->second.used) { ++c; continue; } retire(c->second.g.vb); retire(c->second.g.ib); c = texMeshCache_.erase(c); }
  syncLods(usedTex, usedSets);
  // (the UV Editor's texture under the UV stays too)
  if (w_->uvMode()) for (auto& [id, it] : sc.items) if (w_->uvShown(id)) { auto sets = looks::setsOf(it);
    for (int i = 0; i < int(sets.size()); i++) { QString mk = textureFor(looks::resolve(it, i, 0), true); if (!mk.isEmpty()) { usedTex.insert(mk); usedSets.insert(mk + "|"); } } }
  // (the reference planes' images stay too)
  for (const RefPlane& rp : sc.refPlanes) { QString mk = textureFor(refImage(rp), true); if (!mk.isEmpty()) usedTex.insert(mk); usedSets.insert(mk + "|"); }
  // textures and their descriptor sets no mesh uses any more: freed once the frames in flight are done with them
  for (auto it = texSets_.begin(); it != texSets_.end();) { if (usedSets.count(it->first)) { ++it; continue; } deadTex_.push_back({GpuTex{}, it->second, w_->concurrentFrameCount() + 1}); it = texSets_.erase(it); }
  for (auto it = texCache_.begin(); it != texCache_.end();) { if (usedTex.count(it->first) || !it->second.view) { ++it; continue; } deadTex_.push_back({it->second, VK_NULL_HANDLE, w_->concurrentFrameCount() + 1}); it = texCache_.erase(it); }
  const Camera& cam = w_->camera();
  Box3 b = sc.bounds();
  // the floor: under the model (its bottom) or at the world origin (⊙), raised / lowered by a share of the model's
  // height; its opacity (0 — off); the world axes' opacity (applyFloor)
  const auto& F = w_->floor; double hh = b.empty() ? 1 : std::max(1e-9, b.size().y);
  Vec3 fc = F.zero ? Vec3(0, 0, 0) : cam.sceneCenter; double fy = (F.zero ? 0 : (b.empty() ? 0 : b.min.y)) + F.dy * hh;
  { LineBatch fb = buildFloor(cam.sceneScale, fc, fy, F.op);   // (the view's snapping grid with it, when shown)
    if (const auto g = w_->viewGrid(cam.view); g.on) { const LineBatch gb = buildViewGrid(cam.view, cam.sceneScale, fc, fy, g.step); fb.verts.insert(fb.verts.end(), gb.verts.begin(), gb.verts.end()); }
    uploadLines(floor_, fb); }
  uploadLines(axes_, buildAxes(cam.sceneScale, F.ax));
}

// one wall (gone / hidden — dropped): smooth-shaded, no lines on it
void Renderer::syncWall(int id) { ProfScope prof_("syncWall");
  auto old = walls_.find(id);
  if (old != walls_.end()) { retire(old->second); walls_.erase(old); }
  const Scene& sc = *w_->scene(); const Item* sf = sc.item(id);
  auto wi = w_->hair().walls.find(id);
  if (!sf || wi == w_->hair().walls.end() || !sc.isVisible(*sf) || !w_->display().showSurfaces) return;
  const WallMesh& wm = wi->second;
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  // (a symmetric group's wall: its live mirrored copy too — the faces turned to keep their winding)
  int ax = sym::symAxis(sc, *sf); const Vec3 cen = sym::symCenter(sc, *sf, w_->camera().sceneCenter); const int n0 = int(wm.pos.size());
  PolyMesh pm; pm.pos = wm.pos;
  for (const auto& q : wm.quads) pm.addFace(q.data(), 4);
  if (ax >= 0) {
    for (const Vec3& p : wm.pos) pm.pos.push_back(sym::mirrorPt(p, ax, cen));
    for (const auto& q : wm.quads) { int r[4] = {q[3] + n0, q[2] + n0, q[1] + n0, q[0] + n0}; pm.addFace(r, 4); }
  }
  MeshDraw d = buildMeshDraw(pm, false);
  std::vector<LineVertex> lv;
  const ViewOpts V = viewOpts();
  // (Edges: its width segments' lines along it — the lines between its two curves and the curves' own places at its
  // edges; no loops across a NURBS surface: on a wall they'd read as cuts that aren't there)
  if (V.showGrid) { float c[4]; toLinear(QColor(0x0c, 0x0c, 0x0c), c, float(0.55 * V.gridOp * (1 - 0.5 * V.xray)));
    for (int half = 0; half < (ax >= 0 ? 2 : 1); half++) for (const auto& G : wm.grids) for (int j = 0; j < G.M; j++) for (int k = 0; k + 1 < G.R; k++) {
      const Vec3 &a = pm.pos[size_t(half * n0 + G.start + k * G.M + j)], &b = pm.pos[size_t(half * n0 + G.start + (k + 1) * G.M + j)];
      for (const Vec3* p : {&a, &b}) lv.push_back({{float(p->x), float(p->y), float(p->z)}, {c[0], c[1], c[2], c[3]}}); } }
  // X-ray also makes the NURBS surfaces see-through (setSurfMat: the NURBS opacity × (1 − 0.7 X-ray); above 0.6 — depth written)
  float color[4]; itemBaseColor(sc, *sf, color);
  if (const int hg = w_->hoverGroup(); hg >= 0 && sc.isDescendant(sf->id, hg)) { const float L[3] = {0.25f, 0.75f, 1.0f}; for (int k = 0; k < 3; k++) color[k] = color[k] * 0.35f + L[k] * 0.65f; }   // (Ctrl+Shift: its group lit)
  double op = V.surfOp * (1 - 0.7 * V.xray); color[3] = float(op < 0.995 ? op : 1);
  GpuMesh g = uploadMesh(id, d, color, &lv); g.transparent = op < 0.995; g.depthWrite = op > 0.6;
  walls_[id] = g;
}

// the cocoons: the rings on the lock (shown while the group the cocoon stands in is, unless its rings are off); the
// shell and a faint copy of the rings through everything — only while the cocoon is selected
void Renderer::syncRings() { ProfScope prof_("syncRings");
  for (auto& m : shells_) retire(m);
  shells_.clear();
  const Scene& sc = *w_->scene();
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  LineBatch rl, rb; rl.width = 1.8f; rl.bias = 0.0008f; rb.width = 1.2f;   // (barely off the lock: a lock lying on the skin doesn't show its rings through the head)
  const bool loopsOn = viewOpts().showLoops; CocoonSystem* cs = const_cast<CocoonSystem*>(w_->hair().cocoonSystem());
  float ringC[4], backC[4]; double gop = view.value("gridOpacity").toDouble(1);
  toLinear(QColor(0x10, 0x1c, 0x1e), ringC, float(0.9 * std::max(gop, 0.5))); toLinear(QColor(0x10, 0x1c, 0x1e), backC, 0.12f);
  if (w_->display().showSurfaces) for (auto& [id, k] : w_->hair().cocoons()) {
    const Item* it = sc.item(id); if (!it) continue;
    const Item* cg = it->parent >= 0 ? sc.item(it->parent) : nullptr;
    while (cg && cg->type == ItemType::Cocoon) cg = cg->parent >= 0 ? sc.item(cg->parent) : nullptr;
    bool sel = w_->isItemSelected(id), ringsOn = !it->raw.value("ringsOff").toBool();
    // (shown with Loops on — or while the cocoon is selected; never while its lock's group or the cocoon itself is hidden)
    const Item* lg = cs ? sc.item(cs->lockGroup(id)) : nullptr;
    if (k.lockMode && ringsOn && (loopsOn || sel) && sc.isVisible(*it) && (!cg || sc.isVisible(*cg)) && (!lg || sc.isVisible(*lg))) for (size_t i = 0; i + 1 < k.lockSegs.size(); i += 2) {
      for (const Vec3* p : {&k.lockSegs[i], &k.lockSegs[i + 1]}) rl.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {ringC[0], ringC[1], ringC[2], ringC[3]}});
      if (sel) for (const Vec3* p : {&k.lockSegs[i], &k.lockSegs[i + 1]}) rb.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {backC[0], backC[1], backC[2], backC[3]}});
    }
    if (!sel || !sc.isVisible(*it)) continue;
    const CocoonGeo& G = k.geo; const int M = 24;                    // (COCOON_SIDES)
    PolyMesh pm;
    for (int i = 0; i < G.N; i++) for (int j = 0; j < M; j++) { double th = j * 2 * PI / M; pm.pos.push_back(G.c[i] + G.u[i] * (std::cos(th) * G.a[i]) + G.v[i] * (std::sin(th) * G.b[i])); }
    for (int i = 0; i < G.N - 1; i++) for (int j = 0; j < M; j++) { int j1 = (j + 1) % M, q[4] = {i * M + j, i * M + j1, (i + 1) * M + j1, (i + 1) * M + j}; pm.addFace(q, 4); }
    double op = it->raw.contains("op") && !it->raw.value("op").isNull() ? it->raw.value("op").toDouble() : 0.2;   // (COCOON_OP)
    float color[4]; toLinear(QColor(0x7f, 0xc8, 0xd0), color, float(clamp(op + 0.06, 0, 1) * (1 - 0.6 * viewOpts().xray)));
    GpuMesh g = uploadMesh(id, buildMeshDraw(pm, false), color, nullptr); g.transparent = true;
    shells_.push_back(g);
  }
  uploadLines(rings_, rl);
  uploadLines(ringsBack_, rb);
  // the NURBS discs: a see-through annulus between their two rings (green)
  if (w_->display().showGuides) for (auto& [id, g] : sc.items) {
    if (g.type != ItemType::Guide || g.raw.value("kind").toString() != "disc" || !sc.isVisible(g)) continue;
    QJsonArray rs = g.raw.value("rings").toArray(); if (rs.size() < 2) continue;
    auto A0 = w_->hair().curves.find(rs[0].toInt()), B0 = w_->hair().curves.find(rs[1].toInt());
    if (A0 == w_->hair().curves.end() || B0 == w_->hair().curves.end() || A0->second.p.size() < 4 || B0->second.p.size() < 4) continue;
    auto res = [](const std::vector<Vec3>& Q, int n) {                // (resample by length)
      std::vector<double> L{0}; for (size_t i = 1; i < Q.size(); i++) L.push_back(L.back() + (Q[i] - Q[i - 1]).length());
      double T = L.back() > 0 ? L.back() : 1; std::vector<Vec3> o; size_t j = 0;
      for (int k = 0; k < n; k++) { double s = T * k / (n - 1); while (j + 2 < Q.size() && L[j + 1] < s) j++; o.push_back(Q[j].lerp(Q[j + 1], L[j + 1] > L[j] ? clamp((s - L[j]) / (L[j + 1] - L[j]), 0, 1) : 0)); }
      return o; };
    const int NR = 96; auto A = res(A0->second.p, NR + 1), B = res(B0->second.p, NR + 1);
    PolyMesh pm; for (int k = 0; k <= NR; k++) { pm.pos.push_back(A[size_t(k)]); pm.pos.push_back(B[size_t(k)]); }
    for (int k = 0; k < NR; k++) { int a = 2 * k, q[4] = {a, a + 2, a + 3, a + 1}; pm.addFace(q, 4); }
    float color[4]; toLinear(QColor(0x6f, 0xdc, 0x9a), color, 0.18f);
    GpuMesh m = uploadMesh(id, buildMeshDraw(pm, false), color, nullptr); m.transparent = true;
    shells_.push_back(m);
  }
}

// the crowns: smooth-shaded like the walls, their grid shown (its colour — the crown's group's)
void Renderer::syncCrowns() { ProfScope prof_("syncCrowns");
  for (auto& m : crowns_) retire(m);
  crowns_.clear();
  const Scene& sc = *w_->scene();
  if (!w_->display().showSurfaces) return;
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  for (auto& [id, k] : w_->crownMeshes()) {
    const Item* it = sc.item(id); if (!it || !sc.isVisible(*it) || k.faces.empty()) continue;
    PolyMesh pm; pm.pos = k.pos; for (auto& f : k.faces) pm.addFace(f.data(), int(f.size()));
    const ViewOpts V = viewOpts();
    MeshDraw d = buildMeshDraw(pm, false);
    std::vector<LineVertex> lv;
    // (its grid split: Edges — the lines along it, from the lower connector up to the parting; Loops — the loops across it,
    // ring by ring, and the parting line)
    if (V.showGrid || V.showLoops) { float c[4]; toLinear(QColor(0x0c, 0x0c, 0x0c), c, float(0.4 * V.gridOp * (1 - 0.5 * V.xray)));
      auto ringOf = [&](int v) { return size_t(v) < k.ring.size() ? k.ring[size_t(v)] : -2; };
      std::unordered_set<uint64_t> seen; seen.reserve(k.faces.size() * 3);   // (each shared edge once)
      for (const auto& f : k.faces) for (size_t q = 0; q < f.size(); q++) { int a = f[q], b = f[(q + 1) % f.size()]; if (!seen.insert(uint64_t(uint32_t(std::min(a, b))) << 32 | uint32_t(std::max(a, b))).second) continue;
        const bool across = ringOf(a) == ringOf(b); if (across ? !V.showLoops : !V.showGrid) continue;
        for (int x : {a, b}) { const Vec3& p = k.pos[size_t(x)]; lv.push_back({{float(p.x), float(p.y), float(p.z)}, {c[0], c[1], c[2], c[3]}}); } } }
    float color[4]; itemBaseColor(sc, *it, color); double op = V.surfOp * (1 - 0.7 * V.xray); color[3] = float(op < 0.995 ? op : 1);
    GpuMesh g = uploadMesh(id, d, color, &lv); g.transparent = op < 0.995; g.depthWrite = op > 0.6;
    crowns_.push_back(g);
  }
}

// the flat locks: a see-through ribbon / tube (#b04fc0, the brush's weights as colours) at 0.6, its outline (#a8e8e8)
void Renderer::syncRibs() { ProfScope prof_("syncRibs");
  for (auto& m : ribs_) retire(m);
  ribs_.clear();
  LineBatch ol; ol.width = 2.5f; float oc[4]; toLinear(QColor(0xa8, 0xe8, 0xe8), oc, 0.95f);
  auto line = [&](const Vec3& a, const Vec3& b) { for (const Vec3* p : {&a, &b}) ol.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {oc[0], oc[1], oc[2], oc[3]}}); };
  const ViewOpts V = viewOpts();
  for (auto& [id, r] : w_->ribs()) {
    const size_t N = r.rings.size(), M = N ? r.rings[0].size() : 0; if (N < 2 || M < 2 || !V.showMods) continue;
    PolyMesh pm; for (auto& R : r.rings) pm.pos.insert(pm.pos.end(), R.begin(), R.end());
    for (size_t i = 0; i + 1 < N; i++) for (size_t j = 0; j < (r.tube ? M : M - 1); j++) { size_t j1 = (j + 1) % M; int q[4] = {int(i * M + j), int(i * M + j1), int((i + 1) * M + j1), int((i + 1) * M + j)}; pm.addFace(q, 4); }
    MeshDraw d = buildMeshDraw(pm, false);
    if (!r.col.empty()) for (size_t v = 0; v < d.verts.size() && v < r.col.size(); v++) std::memcpy(d.verts[v].c, r.col[v].data(), sizeof(float) * 3);
    // (shown like the modifiers: their common opacity, a selected one a little more solid, X-ray)
    double op = std::min(1.0, V.modOp + (w_->isItemSelected(id) ? 0.15 : 0)) * (1 - 0.65 * V.xray);
    float color[4]; if (!r.col.empty()) { color[0] = color[1] = color[2] = 1; color[3] = float(op); } else toLinear(QColor(0xb0, 0x4f, 0xc0), color, float(op));
    GpuMesh g = uploadMesh(id, d, color, nullptr); g.transparent = true; g.hl = true; ribs_.push_back(g);
    // the outline: the edges along (every point of a tube) and the root (the tip too where it isn't a point)
    for (size_t j = 0; j < M; j++) for (size_t i = 0; i + 1 < N; i++) line(r.rings[i][j], r.rings[i + 1][j]);
    auto ring = [&](size_t i) { for (size_t j = 0; j < (r.tube ? M : M - 1); j++) line(r.rings[i][j], r.rings[i][(j + 1) % M]); };
    ring(0); if (!r.pointTip) ring(N - 1);
  }
  uploadLines(ribLines_, ol);
}

// the curve modifiers: a see-through surface (#8f78e0 at 0.5; painted weights — per vertex, at 0.62), a fat guide along
// the lock per profile point (its profile-editor colour, darker where masked) and a few loops across it
void Renderer::syncTubes() { ProfScope prof_("syncTubes");
  for (auto& m : tubes_) retire(m);
  tubes_.clear();
  LineBatch gl, ll, wl; gl.width = 3; ll.width = 1; wl.width = 1;
  const ViewOpts V = viewOpts();
  float loopC[4]; toLinear(QColor(0x2a, 0x1f, 0x48), loopC, float(0.6 * (1 - 0.6 * V.xray)));
  for (auto& [id, t] : w_->tubes()) {
    const int n = t.n, NR = t.NR; if ((!t.mesh && (n < 3 || NR < 2)) || !V.showMods) continue;
    PolyMesh pm; if (t.mesh) pm = *t.mesh; else { pm.pos = t.pos;
      for (int i = 0; i + 1 < NR; i++) for (int j = 0; j < n; j++) { int j1 = (j + 1) % n, q[4] = {i * n + j, i * n + j1, (i + 1) * n + j1, (i + 1) * n + j}; pm.addFace(q, 4); } }
    if (t.mesh) {   // (a surface of its own polygons: drawn solid, every edge of it over it)
      MeshDraw dm = buildMeshDraw(pm, false); float c4[4]; toLinear(t.color.isValid() ? t.color : QColor(0x8f, 0x78, 0xe0), c4, 1.f);
      GpuMesh gm = uploadMesh(t.item >= 0 ? t.item : id, dm, c4, nullptr); gm.transparent = false; gm.hl = true; tubes_.push_back(gm);
      float wc[4]; toLinear(QColor(0x30, 0x24, 0x48), wc, 0.45f);
      for (int f = 0; f < pm.faceCount(); f++) { const int s = pm.faceStart[size_t(f)], e = pm.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) for (int v : {pm.faceVerts[size_t(k)], pm.faceVerts[size_t(k + 1 < e ? k + 1 : s)]}) { const Vec3& p = pm.pos[size_t(v)]; wl.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {wc[0], wc[1], wc[2], wc[3]}}); } }
      continue; }
    MeshDraw d = buildMeshDraw(pm, false);
    bool painted = !t.col.empty();
    if (painted) for (size_t v = 0; v < d.verts.size() && v < t.col.size(); v++) std::memcpy(d.verts[v].c, t.col[v].data(), sizeof(float) * 3);
    // (the common Modifiers opacity, a selected one a little more solid, X-ray makes it see-through like the walls)
    const int own = t.item >= 0 ? t.item : id;
    double op = std::min(1.0, (t.opacity >= 0 ? t.opacity : V.modOp) + (w_->isItemSelected(own) && V.modOp < 0.95 ? 0.15 : 0)) * (1 - 0.65 * V.xray);
    float color[4]; if (painted) { color[0] = color[1] = color[2] = 1; color[3] = float(op); } else toLinear(t.color.isValid() ? t.color : QColor(0x8f, 0x78, 0xe0), color, float(op));
    GpuMesh g = uploadMesh(own, d, color, nullptr); g.transparent = !(t.opacity >= 1); g.hl = true;
    tubes_.push_back(g);
    if (t.guides) for (int j = 0; j < n; j++) {
      float pc[4]; toLinear(tube::profColor(j), pc, 0.95f);
      for (int i = 0; i + 1 < NR; i++) for (int v : {i * n + j, (i + 1) * n + j}) {
        float k = painted ? std::min(1.f, (t.col[size_t(v)][0] + t.col[size_t(v)][1] + t.col[size_t(v)][2]) / 1.5f) : 1.f;
        const Vec3& p = t.pos[size_t(v)];
        gl.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {pc[0] * k, pc[1] * k, pc[2] * k, pc[3]}});
      }
    }
    if (t.ringLines) { float lc[4]; toLinear(QColor(0xd8, 0xc8, 0xf0), lc, 0.5f); for (int i = 0; i < NR; i++) for (int j = 0; j < n; j++) for (int v : {i * n + j, i * n + (j + 1) % n}) { const Vec3& p = t.pos[size_t(v)]; wl.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {lc[0], lc[1], lc[2], lc[3]}}); } continue; }   // (a braid's cocoon: its levels)
    if (t.wire) { float wc[4]; toLinear(QColor(0x30, 0x24, 0x48), wc, 0.45f); for (int i = 0; i < NR; i++) for (int j = 0; j < n; j++) for (int v : {i * n + j, i * n + (j + 1) % n}) { const Vec3& p = t.pos[size_t(v)]; wl.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {wc[0], wc[1], wc[2], wc[3]}}); }
      for (int j = 0; j < n; j++) for (int i = 0; i + 1 < NR; i++) for (int v : {i * n + j, (i + 1) * n + j}) { const Vec3& p = t.pos[size_t(v)]; wl.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {wc[0], wc[1], wc[2], wc[3]}}); } continue; }
    int ls = std::max(1, int(std::lround(NR / 16.0)));
    for (int i = 0; i < NR; i += ls) for (int j = 0; j < n; j++) for (int v : {i * n + j, i * n + (j + 1) % n}) {
      const Vec3& p = t.pos[size_t(v)]; ll.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {loopC[0], loopC[1], loopC[2], loopC[3]}});
    }
  }
  uploadLines(tubeGuides_, gl);
  uploadLines(tubeLoops_, ll);
  uploadLines(tubeWire_, wl); tubeWire_.bias = 0.002f;
}

// the Topology room over the active mesh (Viewport::topoDraw): see-through polygons (unlit), lines, square points
void Renderer::syncTopo() {
  ProfScope prof_("syncTopo");
  for (auto& m : topoTris_) retire(m); for (auto& g : topoLines_) retire(g.vb); retire(topoPts_); retire(topoPtsX_);
  topoTris_.clear(); topoTrisLift_.clear(); topoLines_.clear(); topoPtCount_ = 0; topoPtXCount_ = 0; retire(topoPtsN_); topoPtNCount_ = 0;
  const auto& D = w_->topoDraw();
  for (auto& t : D.tris) {
    MeshDraw d; for (size_t i = 0; i < t.p.size(); i++) { MeshVertex v{}; v.p[0] = float(t.p[i].x); v.p[1] = float(t.p[i].y); v.p[2] = float(t.p[i].z); v.c[0] = v.c[1] = v.c[2] = 1; d.verts.push_back(v); d.tris.push_back(uint32_t(i)); }
    float c[4]; toLinear(t.c, c, t.op); GpuMesh g = uploadMesh(0, d, c, nullptr); g.transparent = true; topoTris_.push_back(g); topoTrisLift_.push_back(t.lift);
  }
  for (auto& l : D.lines) {
    LineBatch b; b.width = l.w; b.bias = 0.004f; float c[4]; toLinear(l.c, c, float(l.c.alphaF()));   // (nearer than the mesh's own wire: the picked edges over it, no flicker; the colour's alpha — its opacity)
    for (auto& p : l.p) b.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {c[0], c[1], c[2], c[3]}});
    GpuLines g; uploadLines(g, b); g.noPdo = l.noPdo; g.xray = l.xray; topoLines_.push_back(g);
  }
  std::vector<PointVertex> pts;
  std::vector<PointVertex> ptsX, ptsN;
  for (auto& s : D.pts) for (size_t i = 0; i < s.p.size(); i++) { float c[4]; toLinear(s.c[i], c); (s.xray ? ptsX : s.noPdo ? ptsN : pts).push_back({{float(s.p[i].x), float(s.p[i].y), float(s.p[i].z)}, {c[0], c[1], c[2], s.size}, 5.f}); }
  if (!ptsN.empty()) { topoPtsN_ = makeBuffer(ptsN.data(), ptsN.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); topoPtNCount_ = uint32_t(ptsN.size()); }
  if (!ptsX.empty()) { topoPtsX_ = makeBuffer(ptsX.data(), ptsX.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); topoPtXCount_ = uint32_t(ptsX.size()); }
  topoPts_ = makeBuffer(pts.data(), pts.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); topoPtCount_ = uint32_t(pts.size());
}

// curves (selected — orange; else their group's colour toward white, or light grey), anchors, the lasso
void Renderer::syncOverlays() {
  ProfScope prof_("syncOverlays");
  const Scene& sc = *w_->scene();
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  float op = float(view.value("curveOpacity").toDouble(1));
  LineBatch cl; cl.width = 2; cl.bias = 0.004f;
  float picked[4], plain[4], snap[4]; toLinear(QColor(0xff, 0x9a, 0x2e), picked, op); toLinear(QColor(0xdd, 0xdd, 0xdd), plain, op);
  toLinear(QColor(0x4c, 0xc9, 0xf0), snap, op);
  float lit[4]; toLinear(QColor(0x7f, 0xe8, 0xff), lit, 1); const int hg = w_->hoverGroup();   // (Ctrl+Shift: the group under the pointer)
  for (const Item* c : sc.curves()) {
    if (c->ring >= 0) continue;                              // (disc rings — with the guides, later)
    bool shown = curveShown(sc, w_->display(), w_->selection(), *c) || c->id == w_->snapCurve() || (w_->showAllCurves && sc.isVisible(*c));
    const bool inHg = hg >= 0 && sc.isDescendant(c->id, hg) && sc.isVisible(*c); shown = shown || inHg;
    if (!shown) continue;
    auto it = w_->hair().curves.find(c->id); if (it == w_->hair().curves.end()) continue;
    float col[4];
    if (c->id == w_->snapCurve()) std::memcpy(col, snap, sizeof col);            // (the curve an anchor slides along)
    else if (w_->surfCutCurve(c->id)) toLinear(QColor(0xc7, 0x7d, 0xff), col, op);   // (the two curves of the wall being cut)
    else if (w_->isItemSelected(c->id)) std::memcpy(col, picked, sizeof col);
    else if (inHg) std::memcpy(col, lit, sizeof col);
    else if (QString t = sc.tintOf(*c); !t.isEmpty()) { float tc[4]; toLinear(QColor(t), tc); for (int k = 0; k < 3; k++) col[k] = tc[k] + (1 - tc[k]) * 0.35f; col[3] = op; }
    else std::memcpy(col, plain, sizeof col);
    const auto& P = it->second.p;
    for (size_t i = 0; i + 1 < P.size(); i++) for (const Vec3* p : {&P[i], &P[i + 1]}) cl.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {col[0], col[1], col[2], col[3]}});
    if (int ax = sym::symAxis(sc, *c); ax >= 0) {               // (a symmetric group's curve: its live mirrored copy)
      const Vec3 cen = sym::symCenter(sc, *c, w_->camera().sceneCenter);
      for (size_t i = 0; i + 1 < P.size(); i++) for (size_t j : {i, i + 1}) {
        Vec3 m = sym::mirrorPt(P[j], ax, cen); cl.verts.push_back({{float(m.x), float(m.y), float(m.z)}, {col[0], col[1], col[2], col[3]}});
      }
    }
  }
  uploadLines(curves_, cl);

  std::vector<PointVertex> pts;
  for (auto& a : w_->shownAnchors()) {
    const Anchor* an = sc.anchor(a.id); if (!an) continue;
    float c[4]; toLinear(QColor::fromRgbF(a.rgb[0], a.rgb[1], a.rgb[2]), c);
    pts.push_back({{float(an->pos.x), float(an->pos.y), float(an->pos.z)}, {c[0], c[1], c[2], a.size}, float(a.shape)});
  }
  std::vector<PointVertex> pins;                              // (the Weld locks pins: over everything)
  // ⚠ the selected walls' dents: red rings
  if (w_->display().showSurfaces) for (auto& k : w_->kinks()) { const Item* wi = sc.item(k.w); const Anchor* an = sc.anchor(k.a); if (!wi || !an || !sc.isVisible(*wi)) continue;
    float c[4]; toLinear(QColor(0xff, 0x4a, 0x3a), c); pts.push_back({{float(an->pos.x), float(an->pos.y), float(an->pos.z)}, {c[0], c[1], c[2], 22.f}, 2.f}); }
  // Weld locks places (Check connections): their edges red (marked correct — green), a pin on each — '!' to fix, '✓'
  // marked correct; the highlighted one lighter and bigger
  LineBatch jl; jl.width = 4;
  const auto& spots = w_->weldSpots();
  for (size_t i = 0; i < spots.size(); i++) {
    const auto& p = spots[i]; bool hot = int(i) == w_->weldHot();
    // (an auto-fix under review: blue — fixed, shown after; orange — shown before)
    int fx = p.fix >= 0 && p.fix < int(w_->fixes().size()) ? (w_->fixes()[size_t(p.fix)].on ? 1 : 0) : -1;
    float ec[3] = {1, 0.25f, 0.2f};
    if (fx == 1) { ec[0] = 0.3f; ec[1] = 0.65f; ec[2] = 1; } else if (fx == 0) { ec[0] = 1; ec[1] = 0.7f; ec[2] = 0.2f; } else if (p.ok) { ec[0] = 0.35f; ec[1] = 1; ec[2] = 0.45f; }
    if (hot) for (float& v : ec) v += (1 - v) * 0.55f;
    QColor lc = QColor::fromRgbF(ec[0], ec[1], ec[2]); float c[4]; toLinear(lc, c);
    for (auto& e : p.edges) for (const Vec3* q : {&e.a, &e.b}) jl.verts.push_back({{float(q->x), float(q->y), float(q->z)}, {c[0], c[1], c[2], 1}});
    QColor pcol = fx == 1 ? (hot ? QColor(0x7f, 0xb6, 0xf2) : QColor(0x37, 0x8a, 0xdd)) : fx == 0 ? (hot ? QColor(0xff, 0xc3, 0x7a) : QColor(0xe8, 0x90, 0x1e))
                : p.ok ? (hot ? QColor(0x8f, 0xd4, 0x7a) : QColor(0x3e, 0x9a, 0x2c)) : (hot ? QColor(0xf2, 0x80, 0x7f) : QColor(0xe2, 0x4b, 0x4a));
    float pc[4]; toLinear(pcol, pc);
    pins.push_back({{float(p.c.x), float(p.c.y), float(p.c.z)}, {pc[0], pc[1], pc[2], hot ? 52.f : 36.f}, (p.ok || fx == 1) ? 4.f : 3.f});
  }
  uploadLines(joints_, jl);
  retire(points_);
  points_ = makeBuffer(pts.data(), pts.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  pointCount_ = uint32_t(pts.size());
  retire(pins_);
  pins_ = makeBuffer(pins.data(), pins.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  pinCount_ = uint32_t(pins.size());

  // the overlays (the lasso, an RMB stroke): in Vulkan's NDC straight (drawn with an identity matrix)
  LineBatch lb; lb.width = 1.5f; double W = w_->paneW(), H = w_->paneH();   // (the active pane's coordinates)
  for (const auto& o : w_->overlays()) {
    float lc[4]; toLinear(o.color, lc); const auto& L = o.pts;
    for (size_t i = 0; i + 1 < L.size() + (o.closed && L.size() > 2 ? 1 : 0); i++) {
      auto a = L[i], b = L[(i + 1) % L.size()];
      for (auto q : {a, b}) lb.verts.push_back({{float(q.first / W * 2 - 1), float(q.second / H * 2 - 1), 0.5f}, {lc[0], lc[1], lc[2], 1}});
    }
  }
  uploadLines(lasso_, lb);

  // the guides: a line guide — the ring it makes round its cocoon; a disc — its two rings (green; selected — orange; off — dim)
  LineBatch gl; gl.width = 3;
  const float gop = float(viewOpts().guideOp);
  float gOn[4], gOff[4], gSel[4]; toLinear(QColor(0x5f, 0xe3, 0x9a), gOn, gop); toLinear(QColor(0x5f, 0xe3, 0x9a), gOff, 0.45f * gop); toLinear(QColor(0xff, 0x9a, 0x2e), gSel, gop);
  for (auto& d : w_->guideLines()) {
    const float* c = d.sel ? gSel : d.off ? gOff : gOn;
    for (size_t i = 0; i + 1 < d.pts.size(); i++) for (const Vec3* p : {&d.pts[i], &d.pts[i + 1]}) gl.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {c[0], c[1], c[2], c[3]}});
  }
  uploadLines(guides_, gl); gl = LineBatch{}; gl.width = 3;     // (the zone below: over everything — with the masks)
  // a guide's influence zone (shown for a moment): dark green … light green by the weight, over everything
  { float zf[4], zn[4]; toLinear(QColor(0xa6, 0xff, 0xc4), zf); toLinear(QColor(0x17, 0x4f, 0x2e), zn);
    auto zc = [&](double w, float* o) { for (int k = 0; k < 3; k++) o[k] = float(zn[k] + (zf[k] - zn[k]) * w); o[3] = 1; };
    for (auto& z : w_->zoneLines()) { float a[4], b[4]; zc(z.wa, a); zc(z.wb, b);
      gl.verts.push_back({{float(z.a.x), float(z.a.y), float(z.a.z)}, {a[0], a[1], a[2], 1}}); gl.verts.push_back({{float(z.b.x), float(z.b.y), float(z.b.z)}, {b[0], b[1], b[2], 1}}); } }
  { float oc[4]; toLinear(QColor(0xff, 0xb0, 0x40), oc);       // (a collision capsule being placed: its rings, orange)
    for (auto& L : w_->capsuleRings()) for (size_t i = 0; i + 1 < L.size(); i++) for (const Vec3* q : {&L[i], &L[i + 1]}) gl.verts.push_back({{float(q->x), float(q->y), float(q->z)}, {oc[0], oc[1], oc[2], 1}}); }
  uploadLines(zone_, gl);
  // the painted masks along the cocoons (Density — yellow, its Even spacing — cyan; Parent influence — red / green)
  LineBatch ml; ml.width = 6;
  for (auto& s : w_->maskLines()) {
    float ca[4], cb[4]; toLinear(QColor::fromRgbF(s.ca[0], s.ca[1], s.ca[2]), ca); toLinear(QColor::fromRgbF(s.cb[0], s.cb[1], s.cb[2]), cb);
    ml.verts.push_back({{float(s.a.x), float(s.a.y), float(s.a.z)}, {ca[0], ca[1], ca[2], 1}});
    ml.verts.push_back({{float(s.b.x), float(s.b.y), float(s.b.z)}, {cb[0], cb[1], cb[2], 1}});
  }
  uploadLines(masks_, ml);
  // the Edges tool: the selected edges (orange) and the one under the pointer (yellow), over everything
  LineBatch es, eh; es.width = 3.5f; eh.width = 2.5f;
  float eo[4], ey[4]; toLinear(QColor(0xff, 0x9a, 0x2e), eo); toLinear(QColor(0xff, 0xf0, 0x7a), ey);
  for (auto& s : w_->edgeLines()) { LineBatch& b = s.hover ? eh : es; const float* c = s.hover ? ey : eo;
    for (const Vec3* p : {&s.a, &s.b}) b.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {c[0], c[1], c[2], 1}}); }
  uploadLines(edgeSel_, es); uploadLines(edgeHov_, eh);
  // the reference cube (Convert → Set reference): its sides orange, its top face — the one to drag — light
  for (auto& m : ref_) retire(m);
  ref_.clear();
  if (const auto& rc = w_->refCube) {
    PolyMesh pm; std::vector<std::array<float, 3>> tint; double s = rc->size / 2;
    float side[4], topc[4]; toLinear(QColor(0xd0, 0x8a, 0x2e), side); toLinear(QColor(0xff, 0xd2, 0x7a), topc);
    auto P = [&](double x, double y, double z) { return rc->base + Vec3(x * s, y * rc->height, z * s); };
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, const float* col) { int o = int(pm.pos.size()); for (auto& v : {a, b, c, d}) { pm.pos.push_back(v); tint.push_back({col[0], col[1], col[2]}); } int q[4] = {o, o + 1, o + 2, o + 3}; pm.addFace(q, 4); };
    quad(P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1), topc);
    quad(P(-1, 0, -1), P(1, 0, -1), P(1, 0, 1), P(-1, 0, 1), side);
    quad(P(-1, 0, -1), P(-1, 1, -1), P(1, 1, -1), P(1, 0, -1), side); quad(P(-1, 0, 1), P(1, 0, 1), P(1, 1, 1), P(-1, 1, 1), side);
    quad(P(-1, 0, -1), P(-1, 0, 1), P(-1, 1, 1), P(-1, 1, -1), side); quad(P(1, 0, -1), P(1, 1, -1), P(1, 1, 1), P(1, 0, 1), side);
    MeshDraw d = buildMeshDraw(pm, false);
    for (size_t v = 0; v < d.verts.size(); v++) std::memcpy(d.verts[v].c, tint[v].data(), sizeof(float) * 3);
    float white[4] = {1, 1, 1, 0.85f}; GpuMesh g = uploadMesh(0, d, white, nullptr); g.transparent = true; ref_.push_back(g);
  }
}

void Renderer::startNextFrame() {
  ProfScope prof_("frame");
  static const bool prof = qEnvironmentVariableIsSet("HM_PROF"); QElapsedTimer pt; pt.start(); qint64 tBefore = 0;
  // (low latency: the previous frame finished on the GPU before this one is made — the program never runs frames
  // ahead of the screen, so a camera drag doesn't lag more and more and then catch up; a frame here takes ~1 ms)
  { ProfScope pw("gpuWait"); df_->vkQueueWaitIdle(w_->graphicsQueue()); }
  const qint64 tWait = prof ? pt.nsecsElapsed() : 0;
  collectGarbage(false);
  w_->beforeFrame();
  { ProfScope pl("patchLive"); for (auto& mv : w_->takeLiveMoves()) if (!patchLive(mv.m, mv.poly, mv.prev, mv.verts, mv.corners)) {   // (a drag: patched — else made again)
      // (its cached copies dropped: the same mesh made again would take them back unchanged — the old shape left on the
      // screen, a ghost of it where it was)
      if (auto c = plainCache_.find(mv.m); c != plainCache_.end()) { retire(c->second.g); plainCache_.erase(c); }
      if (auto c = texMeshCache_.find(mv.m); c != texMeshCache_.end()) { retire(c->second.g.vb); retire(c->second.g.ib); texMeshCache_.erase(c); }
      staticRev_ = -1; } }
  if (w_->room() == "topo") syncVertPts();
  if (prof) tBefore = pt.nsecsElapsed();
  if (texRev_ != looks::revision()) { texRev_ = looks::revision(); staticRev_ = -1; }   // (the texture folder changed: linked maps read again)
  if (staticRev_ != w_->staticRevision()) { syncStatic(); staticRev_ = w_->staticRevision(); }
  if (w_->takeAllWalls()) {
    for (auto& [id, m] : walls_) retire(m);
    walls_.clear();
    for (auto& [id, wm] : w_->hair().walls) syncWall(id);
    w_->takeDirtyWalls();
  } else for (int id : w_->takeDirtyWalls()) syncWall(id);
  if (w_->takeRingsDirty()) syncRings();
  if (overlayRev_ != w_->overlayRevision()) { syncOverlays(); overlayRev_ = w_->overlayRevision(); }
  if (tubesRev_ != w_->tubesRevision()) { syncTubes(); tubesRev_ = w_->tubesRevision(); }
  if (ribsRev_ != w_->ribsRevision()) { syncRibs(); ribsRev_ = w_->ribsRevision(); }
  if (crownsRev_ != w_->crownsRevision() || staticRev_ != crownStatic_) { syncCrowns(); crownsRev_ = w_->crownsRevision(); crownStatic_ = staticRev_; }
  { const std::array<long long, 3> k{w_->topoRevision(), w_->staticRevision(), w_->hairInspKey()};   // (the Hair room's checks: anew as the hair changes)
    if (topoKey_ != k) { syncTopo(); topoKey_ = k; } }
  { int am = w_->topoMesh(); topoOvPdo_ = am >= 0 && w_->tdisp.pdo > 0 ? float(w_->topoPdo(am) + 0.0005) : 0.f; }
  // (the UV mode: which meshes wear the checker — a change rebuilds the meshes)
  { QByteArray cs; if (w_->uvo.c3d) { cs = QByteArray::number(w_->uvo.density); for (int id : w_->topoMeshes()) if (w_->uvDispShown(id)) cs += "," + QByteArray::number(id); }
    if (cs != checkerSig_) { checkerSig_ = cs; syncStatic(); staticRev_ = w_->staticRevision(); } }
  if (w_->uvPaneShown()) { w_->uvDraw(); if (w_->uvDrawSig() != uvSeen_) { syncUv(); uvSeen_ = w_->uvDrawSig(); }
    w_->uvHoverDraw(); if (w_->uvHoverSig() != uvHovSeen_) { syncUvHover(); uvHovSeen_ = w_->uvHoverSig(); } }

  VkCommandBuffer cb = w_->currentCommandBuffer();
  const QSize sz = w_->swapChainImageSize();
  const double dpr = w_->devicePixelRatio();
  // (the window's panes in swapchain pixels: one, or the split screen's 2 — 4)
  auto devRect = [&](int i) { QRect r = w_->paneRect(i); int x0 = int(std::lround(r.left() * dpr)), y0 = int(std::lround(r.top() * dpr)), x1 = int(std::lround((r.left() + r.width()) * dpr)), y1 = int(std::lround((r.top() + r.height()) * dpr));
    x1 = std::min(x1, sz.width()); y1 = std::min(y1, sz.height()); return QRect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)); };
  const std::vector<int> panes = w_->shownPanes(); const int act = w_->activePane();
  { QRect r = devRect(act); Camera& c = w_->camera(); c.width = r.width(); c.height = r.height(); }   // (the active pane's size: picking)

  // background #2a2a2a (linear: the swapchain is sRGB)
  float bg[4]; toLinear(QColor(0x2a, 0x2a, 0x2a), bg);
  VkClearValue clear[3]{};
  clear[0].color = {{bg[0], bg[1], bg[2], 1}}; clear[1].depthStencil = {1, 0}; clear[2].color = clear[0].color;
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = w_->defaultRenderPass(); rp.framebuffer = w_->currentFramebuffer();
  rp.renderArea.extent = {uint32_t(sz.width()), uint32_t(sz.height())};
  rp.clearValueCount = w_->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2; rp.pClearValues = clear;
  df_->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

  const VkDeviceSize zero = 0;
  const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  QMatrix4x4 ident;
  // the gizmo (it follows the active camera: built every frame) — drawn in the active pane
  if (auto gl = w_->gizmoLines(); true) {
    for (float wd : {3.f, 2.f}) {
      LineBatch b; b.width = wd;
      for (auto& l : gl) if (l.width == wd) { float c[4]; toLinear(l.c, c, float(l.c.alphaF())); for (const Vec3* p : {&l.a, &l.b}) b.verts.push_back({{float(p->x), float(p->y), float(p->z)}, {c[0], c[1], c[2], c[3]}}); }
      uploadLines(wd == 3.f ? gizmo3_ : gizmo2_, b);
    }
  }

  // the reference planes of each pane (a camera-following one is placed in front of that pane's camera; "only in its
  // view" — the panes in that view): quads, two triangles each
  struct RpDraw { size_t pane; uint32_t first; bool over; VkDescriptorSet ds; float color[4]; int flags; };
  std::vector<RpDraw> rpDraws; std::vector<TexVertex> rpv;
  for (int pi : panes) {
    const Camera& pc = w_->paneCamera(pi);
    for (const RefPlane& rp0 : w_->scene()->refPlanes) {
      if (!refs::shownIn(rp0, pc)) continue;
      RefPlane rp = rp0; refs::apply(rp, pc); M4 M = rp.matrix();
      QString mk = textureFor(refImage(rp), true); bool img = !rp.img.isEmpty();
      RpDraw d{size_t(pi), uint32_t(rpv.size()), rp.over, descriptorFor(mk, QString()), {}, (mk.isEmpty() ? 0 : 1) | 8};
      toLinear(img ? QColor(Qt::white) : QColor(0x77, 0x77, 0x77), d.color, float(img ? rp.opacity : 0.35 * rp.opacity / 0.6));
      Vec3 n = rp.quat.rotate({0, 0, 1});
      const double C[6][4] = {{-0.5, 0.5, 0, 0}, {-0.5, -0.5, 0, 1}, {0.5, -0.5, 1, 1}, {-0.5, 0.5, 0, 0}, {0.5, -0.5, 1, 1}, {0.5, 0.5, 1, 0}};
      for (auto& c : C) { Vec3 p = m4Apply(M, {c[0], c[1], 0}); rpv.push_back({{float(p.x), float(p.y), float(p.z)}, {float(n.x), float(n.y), float(n.z)}, {float(c[2]), float(c[3])}}); }
      rpDraws.push_back(d);
    }
  }
  retire(rpVb_); if (!rpv.empty()) rpVb_ = makeBuffer(rpv.data(), rpv.size() * sizeof(TexVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  const ViewOpts V = viewOpts();
  for (int pi : panes) {
    const bool active = pi == act;
    Camera cam = w_->paneCamera(pi); QRect R = devRect(pi); cam.width = R.width(); cam.height = R.height();
    VkViewport vpt{float(R.x()), float(R.y()), float(R.width()), float(R.height()), 0, 1};
    VkRect2D sc{{R.x(), R.y()}, {uint32_t(R.width()), uint32_t(R.height())}};
    df_->vkCmdSetViewport(cb, 0, 1, &vpt); df_->vkCmdSetScissor(cb, 0, 1, &sc);
    QMatrix4x4 vp = w_->clipCorrectionMatrix() * cam.projMatrix() * cam.viewMatrix();

    auto drawLines = [&](const GpuLines& g, bool top, const QMatrix4x4& m, float mul = 1, float pdo = 0, const float* tint = nullptr) {
      if (!g.count || !g.vb.buf || mul <= 0) return;
      df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, top ? lineTopPipe_ : linePipe_);
      if (wideLines_) df_->vkCmdSetLineWidth(cb, std::min(g.width * float(dpr), maxLineWidth_));
      LinePC pc{}; std::memcpy(pc.viewProj, m.constData(), 64);
      pc.eye[0] = float(cam.pos.x); pc.eye[1] = float(cam.pos.y); pc.eye[2] = float(cam.pos.z); pc.eye[3] = cam.ortho() ? 0 : 1;
      pc.bias[0] = cam.ortho() ? 0 : g.bias + pdo; pc.bias[1] = mul; pc.bias[2] = cam.ortho() ? pdo * 0.5f : 0;
      if (tint) std::memcpy(pc.tint, tint, sizeof pc.tint);
      df_->vkCmdPushConstants(cb, lineLayout_, VF, 0, sizeof(pc), &pc);
      df_->vkCmdBindVertexBuffers(cb, 0, 1, &g.vb.buf, &zero);
      df_->vkCmdDraw(cb, g.count, 1, 0, 0);
    };

    auto drawPlanes = [&](bool over) {
      bool bound = false;
      for (const RpDraw& d : rpDraws) {
        if (d.pane != size_t(pi) || d.over != over || !d.ds || !rpVb_.buf) continue;
        if (!bound) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, over ? rpOverPipe_ : rpBehindPipe_); df_->vkCmdBindVertexBuffers(cb, 0, 1, &rpVb_.buf, &zero); bound = true; }
        MeshPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64); std::memcpy(pc.color, d.color, 16);
        pc.params[0] = 1; pc.params[3] = float(d.flags);                                      // (unlit: the picture as it is)
        df_->vkCmdPushConstants(cb, texLayout_, VF, 0, sizeof(pc), &pc);
        df_->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, texLayout_, 0, 1, &d.ds, 0, nullptr);
        df_->vkCmdDraw(cb, 6, 1, d.first, 0);
      }
    };
    if (cam.view == "uv") {                                     // the UV Editor: the UV with the window's own matrix, no 3D
      const double s = cam.uvScale(R.width() / dpr, R.height() / dpr) * dpr;
      QMatrix4x4 um; um(0, 0) = float(2 * s / R.width()); um(0, 3) = float(-2 * s * cam.uvCx / R.width()); um(1, 1) = float(-2 * s * cam.uvAsp / R.height()); um(1, 3) = float(2 * s * cam.uvAsp * cam.uvCy / R.height()); um(2, 2) = 0; um(2, 3) = 0.5f;
      if (uvTexDs_ && uvQuad_.buf) {
        df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rpOverPipe_); df_->vkCmdBindVertexBuffers(cb, 0, 1, &uvQuad_.buf, &zero);
        MeshPC pc{}; std::memcpy(pc.viewProj, um.constData(), 64); toLinear(uvChk_ ? QColor(Qt::white) : QColor(0xd8, 0xd8, 0xd8), pc.color); pc.color[3] = uvOp_; pc.params[0] = 1; pc.params[3] = float(1 | 8);
        df_->vkCmdPushConstants(cb, texLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, texLayout_, 0, 1, &uvTexDs_, 0, nullptr);
        df_->vkCmdDraw(cb, 6, 1, 0, 0);
      }
      for (const GpuLines& g : uvGrid_) drawLines(g, true, um);
      if (!uvTris_.empty()) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipe_);
        for (const GpuMesh& m : uvTris_) { MeshPC pc{}; std::memcpy(pc.viewProj, um.constData(), 64); std::memcpy(pc.color, m.color, 16); pc.params[0] = 1; pc.params[3] = 2;
          df_->vkCmdPushConstants(cb, meshLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
          df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32); df_->vkCmdDrawIndexed(cb, m.triCount, 1, 0, 0, 0); } }
      for (const GpuLines& g : uvLines_) drawLines(g, true, um);
      auto uvPoints = [&](const Buf& b, uint32_t n) { if (!n || !b.buf) return; df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pointPipe_);
        PointPC pc{}; std::memcpy(pc.viewProj, um.constData(), 64); pc.params[0] = float(dpr); pc.params[1] = 1; pc.params[2] = 1; pc.params[3] = float(R.height());
        df_->vkCmdPushConstants(cb, pointLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindVertexBuffers(cb, 0, 1, &b.buf, &zero); df_->vkCmdDraw(cb, n, 1, 0, 0); };
      uvPoints(uvPts_, uvPtCount_);
      // (the hover layer over all of it: the polygon under the pointer, the preview's veil and lines)
      if (!uvHovTris_.empty()) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipe_);
        for (const GpuMesh& m : uvHovTris_) { MeshPC pc{}; std::memcpy(pc.viewProj, um.constData(), 64); std::memcpy(pc.color, m.color, 16); pc.params[0] = 1; pc.params[3] = 2;
          df_->vkCmdPushConstants(cb, meshLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
          df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32); df_->vkCmdDrawIndexed(cb, m.triCount, 1, 0, 0, 0); } }
      for (const GpuLines& g : uvHovLines_) drawLines(g, true, um);
      uvPoints(uvHovPts_, uvHovPtCount_);
      if (active) drawLines(lasso_, true, ident);
      continue;
    }
    drawPlanes(false);                                         // (behind the model: first, writing no depth)
    drawLines(floor_, false, vp);

    Vec3 lightPos = cam.pos + cam.upVec() * (cam.sceneScale * 3) + cam.right() * (cam.sceneScale * 1.5);
    Vec3 L = (lightPos - cam.sceneCenter).normalized();
    const bool topoRoom = w_->room() == "topo", unlitAll = topoRoom && w_->tdisp.unlit;
    auto drawMesh = [&](const GpuMesh& m, bool wall = false) {
      if (!m.triCount) return;
      MeshPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64);
      std::memcpy(pc.color, m.color, 16);
      pc.lightDir[0] = float(L.x); pc.lightDir[1] = float(L.y); pc.lightDir[2] = float(L.z); pc.lightDir[3] = m.mat;
      pc.params[0] = unlitAll || m.unlit ? 1.f : 0.f; pc.params[1] = 1.3f; pc.params[2] = 1.7f;
      const bool two = topoRoom && w_->topoTwoTone(m.itemId);
      pc.params[3] = float((two ? 1 : 0) | (cam.ortho() ? 2 : 0));
      pc.extra[0] = float(cam.pos.x); pc.extra[1] = float(cam.pos.y); pc.extra[2] = float(cam.pos.z); pc.extra[3] = topoRoom ? float(w_->topoPdo(m.itemId)) : 0.f;
      if (topoRoom) { double op = w_->topoOpacity(m.itemId); if (op < 0.995) pc.color[3] = float(op); }
      if (!topoRoom && m.itemId >= 0 && m.itemId == w_->surfCutWall()) {   // (the Scissors' NURBS mode: the wall being cut — violet, apart from the orange selection)
        float v[4], e[4]; toLinear(QColor(0x9b, 0x5c, 0xff), v); toLinear(QColor(0x2a, 0x10, 0x48), e);
        for (int k = 0; k < 3; k++) pc.color[k] = pc.color[k] + (v[k] - pc.color[k]) * 0.6f + e[k] * 3.14159f;
      } else
      if ((!m.transparent || m.hl || wall) && w_->isItemSelected(m.itemId) && !two && !topoRoom) {   // (a see-through wall too: its selection shown)        // (selected: the HTML version's emissive 0x4a2800 — not in the Topology room: its meshes keep their own grey / material)
        float e[4]; toLinear(QColor(0x4a, 0x28, 0x00), e);
        if (wall) {                                                // (a wall: its group's colour may be anything — a red one hid the glow; painted over by a colour unlike its own)
          const QColor own = QColor::fromRgbF(std::pow(pc.color[0], 1 / 2.2f), std::pow(pc.color[1], 1 / 2.2f), std::pow(pc.color[2], 1 / 2.2f));
          const int h = own.hsvHue(); const bool warm = own.hsvSaturation() > 40 && (h < 75 || h > 300);   // (red, orange, yellow, pink: the selection light blue — else orange)
          float t[4]; toLinear(warm ? QColor(0x5a, 0xd8, 0xff) : QColor(0xff, 0xa8, 0x30), t);
          for (int k = 0; k < 3; k++) pc.color[k] = pc.color[k] + (t[k] - pc.color[k]) * 0.65f;
        } else { pc.color[0] += e[0] * 3.14159f; pc.color[1] += e[1] * 3.14159f; pc.color[2] += e[2] * 3.14159f; }
      }
      df_->vkCmdPushConstants(cb, meshLayout_, VF, 0, sizeof(pc), &pc);
      df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
      df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32);
      df_->vkCmdDrawIndexed(cb, m.triCount, 1, 0, 0, 0);
    };
    df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipe_);
    auto seeThrough = [&](const GpuMesh& m) { return topoRoom && w_->topoOpacity(m.itemId) < 0.995; };   // (Topology: the selected meshes' opacity)
    auto plainShown = [&](const GpuMesh& m) { return !m.textured || (topoRoom && w_->topoTwoTone(m.itemId)); };
    const bool nav = w_->navigating() && !staticNav_.empty();   // (the camera moving: heavy meshes by their LOD)
    if (nav && active) w_->lodFrames++;
    const std::vector<GpuMesh>& S = nav ? staticNav_ : static_; const std::vector<GpuTexMesh>& TM = nav ? texNav_ : texMeshes_;
    for (const GpuMesh& m : S) if (!seeThrough(m) && plainShown(m)) drawMesh(m);
    for (auto& [id, m] : walls_) if (!m.transparent) drawMesh(m, true);
    for (const GpuMesh& m : crowns_) if (!m.transparent) drawMesh(m);
    for (const GpuMesh& m : tubes_) if (!m.transparent) drawMesh(m);   // (the braids: solid — the strands one over another clean)
    // textured meshes: the opaque sets, then the see-through ones (blended)
    auto drawTex = [&](bool transparent) {
      bool bound = false;
      for (const GpuTexMesh& m : TM) {
        if (!m.vb.buf || !m.ib.buf || (topoRoom && w_->topoTwoTone(m.itemId))) continue;
        for (const TexPart& p : m.parts) {
          if (p.transparent != transparent || !p.count || !p.ds) continue;
          if (!bound) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, transparent ? texBlendPipe_ : texPipe_); bound = true; }
          MeshPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64); std::memcpy(pc.color, p.color, 16);
          pc.lightDir[0] = float(L.x); pc.lightDir[1] = float(L.y); pc.lightDir[2] = float(L.z); pc.lightDir[3] = m.mat;
          pc.params[0] = m.flat || unlitAll || m.unlit ? 1.f : 0.f; pc.params[1] = 1.3f; pc.params[2] = 1.7f; pc.params[3] = float(p.flags | (cam.ortho() ? 16 : 0));
          pc.extra[0] = float(cam.pos.x); pc.extra[1] = float(cam.pos.y); pc.extra[2] = float(cam.pos.z); pc.extra[3] = topoRoom ? float(w_->topoPdo(m.itemId)) : 0.f;
          if (topoRoom) { double op = w_->topoOpacity(m.itemId); if (op < 0.995) { pc.color[3] *= float(op); pc.params[3] = float(int(pc.params[3]) | 8); } }
          if (w_->isItemSelected(m.itemId)) { float e[4]; toLinear(QColor(0x4a, 0x28, 0x00), e); for (int k = 0; k < 3; k++) pc.color[k] += e[k] * 3.14159f; }
          df_->vkCmdPushConstants(cb, texLayout_, VF, 0, sizeof(pc), &pc);
          df_->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, texLayout_, 0, 1, &p.ds, 0, nullptr);
          df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
          df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32);
          df_->vkCmdDrawIndexed(cb, p.count, 1, p.first, 0, 0);
        }
      }
    };
    drawTex(false);
    drawTex(true);
    // (a see-through mesh: its depth first — then only its surface nearest the camera blended in; its far side and its
    // inner layers not seen through it)
    for (const GpuMesh& m : S) if (seeThrough(m) && plainShown(m)) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshDepthPipe_); drawMesh(m);
      df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipe_); drawMesh(m); }
    // (the Topology room's Object mode: a halo round each selected mesh — which of them are picked, seen at a glance)
    if (topoRoom && w_->topoMode == "object") { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipe_);   // (after the see-through meshes: their depth there — the halo round them, not over them)
      const double wpp = w_->worldPerPixelAt(cam.pivot), dp = std::max(1e-9, cam.pos.distanceTo(cam.pivot)); float hc[4]; toLinear(QColor(0xb0, 0x6c, 0xff), hc);
      for (const GpuMesh& m : S) { if (!m.triCount || !w_->isItemSelected(m.itemId)) continue;
        MeshPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64); std::memcpy(pc.color, hc, 16);
        const Vec3 vd = cam.dir(); pc.lightDir[0] = float(vd.x); pc.lightDir[1] = float(vd.y); pc.lightDir[2] = float(vd.z);
        pc.params[0] = 1; pc.params[1] = float(0.75 * wpp / dp); pc.params[2] = float(0.75 * wpp); pc.params[3] = float(64 | (cam.ortho() ? 2 : 0));
        pc.extra[0] = float(cam.pos.x); pc.extra[1] = float(cam.pos.y); pc.extra[2] = float(cam.pos.z); pc.extra[3] = 0;
        df_->vkCmdPushConstants(cb, meshLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
        df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32); df_->vkCmdDrawIndexed(cb, m.triCount, 1, 0, 0, 0); } }
    // the NURBS walls and crowns see-through (the NURBS opacity, X-ray): after the solid scene
    for (bool write : {true, false}) {
      bool bound = false;
      auto tr = [&](const GpuMesh& m, bool wall = false) { if (!m.transparent || m.depthWrite != write) return; if (!bound) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, write ? meshBlendWritePipe_ : meshBlendPipe_); bound = true; } drawMesh(m, wall); };
      for (auto& [id, m] : walls_) tr(m, true);
      for (const GpuMesh& m : crowns_) tr(m);
    }
    auto drawWire = [&](const GpuMesh& m) { if (!m.wireCount || !m.wireOn || (topoRoom && w_->topoTwoTone(m.itemId))) return; GpuLines g; g.vb = m.wb; g.count = m.wireCount; drawLines(g, false, vp, 1, topoRoom ? float(w_->topoPdo(m.itemId)) : 0.f); };
    for (const GpuMesh& m : S) drawWire(m);
    for (auto& [id, m] : walls_) drawWire(m);
    for (const GpuMesh& m : crowns_) drawWire(m);
    // the Topology room over the active mesh: the selection's polygons, the polygroups, the masks; its edges; vertices
    if (!topoTris_.empty() || !topoLines_.empty() || topoPts_.buf || (topoRoom && w_->topoDraw().edgesOf >= 0)) {   // (the Hair room: the polygroups' colours, the Inspector's highlights)
      if (!topoTris_.empty()) { df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipe_);
        for (size_t ti = 0; ti < topoTris_.size(); ti++) { const GpuMesh& m = topoTris_[ti]; MeshPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64); std::memcpy(pc.color, m.color, 16); pc.params[0] = 1; pc.params[3] = float(cam.ortho() ? 2 : 0);
          pc.extra[0] = float(cam.pos.x); pc.extra[1] = float(cam.pos.y); pc.extra[2] = float(cam.pos.z); pc.extra[3] = std::max({topoOvPdo_, 0.0004f, ti < topoTrisLift_.size() ? topoTrisLift_[ti] : 0.f});   // (a hair nearer the camera: over the surface they lie on — the snap mask on a reference too)
          df_->vkCmdPushConstants(cb, meshLayout_, VF, 0, sizeof(pc), &pc); df_->vkCmdBindVertexBuffers(cb, 0, 1, &m.vb.buf, &zero);
          df_->vkCmdBindIndexBuffer(cb, m.ib.buf, 0, VK_INDEX_TYPE_UINT32); df_->vkCmdDrawIndexed(cb, m.triCount, 1, 0, 0, 0); } }
      // (the active mesh's every edge in a component mode: its own wire — patched as it is dragged — in the overlay's
      // colour; first, so the picked edges lie over it)
      { const auto& D = w_->topoDraw();
        if (D.edgesOf >= 0) if (auto c = plainCache_.find(D.edgesOf); c != plainCache_.end() && c->second.g.wireCount && c->second.g.wb.buf) {
          GpuLines g; g.vb = c->second.g.wb; g.count = c->second.g.wireCount; g.width = D.edgesW; g.bias = 0.002f; float tc[4]; toLinear(D.edgesC, tc);
          if (wideLines_) { GpuLines u = g; u.width = D.edgesW + 1; float lc[4]; toLinear(QColor(0xc9, 0xa6, 0xe6), lc, 0.35f); drawLines(u, false, vp, 1, topoOvPdo_, lc); }   // (a light line under the dark one: the wire seen on the shaded side too)
          drawLines(g, false, vp, 1, topoOvPdo_, tc); } }
      for (const GpuLines& g : topoLines_) { if (g.xray > 0) drawLines(g, true, vp, g.xray); drawLines(g, false, vp, 1, g.noPdo ? 0.f : topoOvPdo_); }   // (an xray one: faint through what hides it first, itself over that)
    }

    drawLines(curves_, false, vp);
    drawLines(rings_, false, vp);
    drawLines(axes_, false, vp);
    drawLines(tubeLoops_, false, vp);
    if (!shells_.empty() || !tubes_.empty() || !ribs_.empty() || !ref_.empty()) {                  // (see-through shells and modifiers over the opaque scene)
      df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipe_);
      for (const GpuMesh& m : tubes_) if (m.transparent) drawMesh(m);
      for (const GpuMesh& m : ribs_) drawMesh(m);
      for (const GpuMesh& m : ref_) drawMesh(m);
      for (const GpuMesh& m : shells_) drawMesh(m);
    }
    drawLines(tubeGuides_, false, vp);
    drawLines(tubeWire_, false, vp);
    drawLines(ribLines_, false, vp);
    drawPlanes(true);                                          // (over the model)
    drawLines(ringsBack_, true, vp);
    drawLines(joints_, true, vp);
    drawLines(guides_, false, vp);
    drawLines(zone_, true, vp);
    drawLines(masks_, true, vp);
    drawLines(edgeSel_, true, vp);
    drawLines(edgeHov_, true, vp);

    // X-ray: the curves, the guides and the anchors again over everything at its opacity (renderViewport's pass 2)
    if (V.xray > 0.001) { drawLines(curves_, true, vp, float(V.xray)); drawLines(guides_, true, vp, float(V.xray)); }
    auto drawPoints = [&](const Buf& b, uint32_t n, VkPipeline pipe, float op, float pdo = 0) {
      if (!n || !b.buf) return;
      df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      PointPC pc{}; std::memcpy(pc.viewProj, vp.constData(), 64);
      pc.params[0] = float(dpr); pc.params[1] = op;   // (sizes above 1 px need largePoints — enabled)
      pc.params[2] = cam.ortho() ? 1.f : 0.f; pc.params[3] = float(R.height());
      pc.eye[0] = float(cam.pos.x); pc.eye[1] = float(cam.pos.y); pc.eye[2] = float(cam.pos.z); pc.eye[3] = pdo;
      df_->vkCmdPushConstants(cb, pointLayout_, VF, 0, sizeof(pc), &pc);
      df_->vkCmdBindVertexBuffers(cb, 0, 1, &b.buf, &zero);
      df_->vkCmdDraw(cb, n, 1, 0, 0);
    };
    drawPoints(points_, pointCount_, pointDepthPipe_, float(V.anchorOp));
    if (V.xray > 0.001) drawPoints(points_, pointCount_, pointPipe_, float(V.anchorOp * V.xray));
    drawPoints(pins_, pinCount_, pointPipe_, 1);
    drawPoints(topoPts_, topoPtCount_, pointDepthPipe_, 1, topoOvPdo_); drawPoints(topoPtsN_, topoPtNCount_, pointDepthPipe_, 1, 0);   // (the Hair room too: the Inspector's markers)
    drawPoints(topoPtsX_, topoPtXCount_, pointPipe_, 1);       // (the interior markers: seen through the mesh)
    if (topoRoom) { if (vpts_.buf.buf && w_->topoDraw().pointsOf == vpts_.m) drawPoints(vpts_.buf, uint32_t(vpts_.pts.size()), pointDepthPipe_, 1, topoOvPdo_); }
    if (active) {                                              // (the lasso, the gizmo: the active pane's)
      drawLines(lasso_, true, ident);
      drawLines(gizmo3_, true, vp); drawLines(gizmo2_, true, vp);
    }
  }
  // split screen: the windows' frames, the active one outlined
  if (panes.size() > 1) {
    VkViewport vpt{0, 0, float(sz.width()), float(sz.height()), 0, 1};
    VkRect2D sc{{0, 0}, {uint32_t(sz.width()), uint32_t(sz.height())}};
    df_->vkCmdSetViewport(cb, 0, 1, &vpt); df_->vkCmdSetScissor(cb, 0, 1, &sc);
    LineBatch fb; fb.width = 1; LineBatch fa; fa.width = 2;
    float cn[4], ca[4]; toLinear(QColor(0x44, 0x44, 0x44), cn); toLinear(QColor(0x4a, 0x7f, 0xd6), ca);
    for (int pi : panes) {
      QRect R = devRect(pi); LineBatch& b = pi == act ? fa : fb; const float* c = pi == act ? ca : cn;
      double x0 = (R.left() + 0.5) / sz.width() * 2 - 1, x1 = (R.left() + R.width() - 0.5) / sz.width() * 2 - 1, y0 = (R.top() + 0.5) / sz.height() * 2 - 1, y1 = (R.top() + R.height() - 0.5) / sz.height() * 2 - 1;
      double P[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
      for (int k = 0; k < 4; k++) for (int j : {k, (k + 1) % 4}) b.verts.push_back({{float(P[j][0]), float(P[j][1]), 0.5f}, {c[0], c[1], c[2], 1}});
    }
    uploadLines(paneFrames_, fb); uploadLines(paneActive_, fa);
    for (GpuLines* g : {&paneFrames_, &paneActive_}) {
      if (!g->count) continue;
      df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lineTopPipe_);
      if (wideLines_) df_->vkCmdSetLineWidth(cb, std::min(g->width * float(dpr), maxLineWidth_));
      LinePC pc{}; std::memcpy(pc.viewProj, ident.constData(), 64); pc.bias[1] = 1;
      df_->vkCmdPushConstants(cb, lineLayout_, VF, 0, sizeof(pc), &pc);
      df_->vkCmdBindVertexBuffers(cb, 0, 1, &g->vb.buf, &zero);
      df_->vkCmdDraw(cb, g->count, 1, 0, 0);
    }
  }

  df_->vkCmdEndRenderPass(cb);
  if (prof) { static QFile pf(qEnvironmentVariable("HM_PROF")); if (!pf.isOpen()) pf.open(QIODevice::WriteOnly | QIODevice::Text);
    static QElapsedTimer last; double gap = last.isValid() ? last.nsecsElapsed() / 1e6 : 0; last.start();
    pf.write(QString("frame %1 ms (GPU wait %4 ms, beforeFrame %2 ms) since last %3 ms\n").arg(pt.nsecsElapsed() / 1e6, 0, 'f', 2).arg((tBefore - tWait) / 1e6, 0, 'f', 2).arg(gap, 0, 'f', 1).arg(tWait / 1e6, 0, 'f', 2).toUtf8()); pf.flush(); }
  w_->framesDrawn++; w_->noteFrame(pt.nsecsElapsed() / 1e6);
  w_->frameReady();
}

}  // namespace hm
