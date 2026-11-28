#include "nzl/skeleton.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace nzl {
namespace {

constexpr uint64_t kIl2CppArrayHeaderBytes = 0x20;
constexpr uint64_t kMaxSingleReadBytes = 0x10000;
constexpr uint64_t kMaxUserAddress = 0x00007FFFFFFFFFFFULL;

bool add_offset(uint64_t base, uint32_t offset, uint64_t& address) {
  if (base > std::numeric_limits<uint64_t>::max() - offset) return false;
  address = base + offset;
  return true;
}

bool read_pointer_field(const IMemory& memory, uint64_t base, uint32_t offset,
                        uint64_t& value) {
  uint64_t address = 0;
  if (!plausible_ptr(base) || !add_offset(base, offset, address) ||
      address > kMaxUserAddress - sizeof(uint64_t)) return false;
  const auto read = memory.ptr(address);
  if (!read || !plausible_ptr(*read)) return false;
  value = *read;
  return true;
}

bool read_i32_field(const IMemory& memory, uint64_t base, uint32_t offset,
                    int32_t& value) {
  uint64_t address = 0;
  if (!plausible_ptr(base) || !add_offset(base, offset, address) ||
      address > kMaxUserAddress - sizeof(int32_t)) return false;
  const auto read = memory.i32(address);
  if (!read) return false;
  value = *read;
  return true;
}

bool read_array_header(const IMemory& memory, uint64_t array,
                       const Offsets& offsets, uint64_t max_elements,
                       uint64_t& length, uint64_t& data) {
  if (!plausible_ptr(array) || array > UINT64_MAX - offsets.array_length ||
      array > UINT64_MAX - offsets.array_data ||
      array + offsets.array_length > kMaxUserAddress - sizeof(uint64_t) ||
      array + offsets.array_data >= kMaxUserAddress) return false;
  const auto read_length = memory.ptr(array + offsets.array_length);
  if (!read_length || *read_length == 0 || *read_length > max_elements)
    return false;
  length = *read_length;
  data = array + offsets.array_data;
  return data >= kIl2CppArrayHeaderBytes && data < 0x00007FFFFFFFFFFFULL;
}

bool finite_quaternion(const std::array<float, 4>& q) {
  float norm_squared = 0.0f;
  for (float component : q) {
    if (!std::isfinite(component)) return false;
    norm_squared += component * component;
  }
  // Rotation quaternions should be near unit length. These broad bounds allow
  // normal floating-point drift but reject empty or clearly corrupt records.
  return std::isfinite(norm_squared) && norm_squared >= 0.25f &&
         norm_squared <= 2.25f;
}

bool valid_index(int32_t index, int32_t count) {
  return index >= 0 && index < count;
}

bool valid_optional_index(int32_t index, int32_t count) {
  return index == -1 || valid_index(index, count);
}

}  // namespace

SkeletonReadResult read_skeleton_snapshot(const IMemory& memory,
                                           uint64_t character,
                                           const Offsets& offsets) {
  SkeletonReadResult result;
  if (!offsets.skeleton_offsets_configured) {
    result.stage = SkeletonReadStage::ProfileDisabled;
    return result;
  }
  if (!plausible_ptr(character)) {
    result.stage = SkeletonReadStage::InvalidCharacter;
    return result;
  }

  auto& snapshot = result.snapshot;
  snapshot.character = character;
  if (!read_pointer_field(memory, character, offsets.skeleton_character_model,
                          snapshot.model)) {
    result.stage = SkeletonReadStage::ModelPointer;
    return result;
  }
  if (!read_pointer_field(memory, snapshot.model,
                          offsets.skeleton_model_description,
                          snapshot.description)) {
    result.stage = SkeletonReadStage::DescriptionPointer;
    return result;
  }
  if (!read_pointer_field(memory, snapshot.model,
                          offsets.skeleton_model_bones_data,
                          snapshot.bones_array)) {
    result.stage = SkeletonReadStage::BonesArrayPointer;
    return result;
  }

  if (!read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_bones_count,
                      snapshot.bones_count) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_bone_data_size,
                      snapshot.bone_data_size) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_local_pose_offset,
                      snapshot.local_pose_offset) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_model_pose_offset,
                      snapshot.model_pose_offset) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_bone_length_offset,
                      snapshot.bone_length_offset) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_hierarchy_max_children,
                      snapshot.hierarchy_max_children) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_root_index,
                      snapshot.root_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_legs_index,
                      snapshot.legs_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_spine2_index,
                      snapshot.spine2_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_spine3_index,
                      snapshot.spine3_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_neck_index,
                      snapshot.neck_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_head1_index,
                      snapshot.head1_index) ||
      !read_i32_field(memory, snapshot.description,
                      offsets.skeleton_config_head2_index,
                      snapshot.head2_index)) {
    result.stage = SkeletonReadStage::ConfigFields;
    return result;
  }

  const int32_t count = snapshot.bones_count;
  const int32_t stride = snapshot.bone_data_size;
  const int32_t local_offset = snapshot.local_pose_offset;
  const int32_t model_offset = snapshot.model_pose_offset;
  const int32_t length_offset = snapshot.bone_length_offset;
  const bool offsets_fit = stride >= 9 &&
      stride <= static_cast<int32_t>(offsets.skeleton_max_bone_data_floats) &&
      local_offset >= 0 && local_offset <= stride - 4 &&
      model_offset >= 0 && model_offset <= stride - 4 &&
      length_offset >= 0 && length_offset < stride &&
      (local_offset + 4 <= model_offset || model_offset + 4 <= local_offset) &&
      (length_offset < local_offset || length_offset >= local_offset + 4) &&
      (length_offset < model_offset || length_offset >= model_offset + 4);
  const bool indices_fit = valid_index(snapshot.root_index, count) &&
      valid_index(snapshot.head1_index, count) &&
      valid_optional_index(snapshot.legs_index, count) &&
      valid_optional_index(snapshot.spine2_index, count) &&
      valid_optional_index(snapshot.spine3_index, count) &&
      valid_optional_index(snapshot.neck_index, count) &&
      valid_optional_index(snapshot.head2_index, count);
  if (count < 2 || count > static_cast<int32_t>(offsets.skeleton_max_bones) ||
      !offsets_fit || snapshot.hierarchy_max_children < 1 ||
      snapshot.hierarchy_max_children > count || !indices_fit) {
    result.stage = SkeletonReadStage::ConfigShape;
    return result;
  }

  const uint64_t max_pose_floats =
      static_cast<uint64_t>(offsets.skeleton_max_bones) *
      offsets.skeleton_max_bone_data_floats;
  uint64_t bones_array_length = 0, bones_data_address = 0;
  if (!read_array_header(memory, snapshot.bones_array, offsets,
                         max_pose_floats, bones_array_length,
                         bones_data_address)) {
    result.stage = SkeletonReadStage::BonesArrayBounds;
    return result;
  }
  const uint64_t required_floats = static_cast<uint64_t>(count) * stride;
  if (required_floats > bones_array_length ||
      required_floats > max_pose_floats ||
      required_floats > kMaxSingleReadBytes / sizeof(float) ||
      bones_data_address > kMaxUserAddress - required_floats * sizeof(float)) {
    result.stage = SkeletonReadStage::BonesArrayBounds;
    return result;
  }

  std::vector<float> packed_pose(static_cast<size_t>(required_floats), 0.0f);
  if (!memory.read(bones_data_address, packed_pose.data(),
                   packed_pose.size() * sizeof(float))) {
    result.stage = SkeletonReadStage::BonesDataRead;
    return result;
  }
  snapshot.bones.reserve(static_cast<size_t>(count));
  for (int32_t index = 0; index < count; ++index) {
    const float* record = packed_pose.data() +
                          static_cast<size_t>(index) *
                              static_cast<size_t>(stride);
    SkeletonBonePose bone;
    std::memcpy(bone.local_position_orientation.data(),
                record + local_offset, 4 * sizeof(float));
    std::memcpy(bone.model_orientation.data(), record + model_offset,
                4 * sizeof(float));
    bone.length = record[length_offset];
    bone.values_valid = finite_quaternion(bone.local_position_orientation) &&
                        finite_quaternion(bone.model_orientation) &&
                        std::isfinite(bone.length) && bone.length >= 0.0f &&
                        bone.length <= 10.0f;
    if (bone.values_valid) ++snapshot.valid_pose_records;
    snapshot.bones.push_back(bone);
  }
  if (snapshot.valid_pose_records != snapshot.bones.size()) {
    result.stage = SkeletonReadStage::PoseData;
    return result;
  }

  if (!read_pointer_field(memory, snapshot.description,
                          offsets.skeleton_config_hierarchy_data,
                          snapshot.hierarchy_array)) {
    result.stage = SkeletonReadStage::HierarchyPointer;
    return result;
  }
  uint64_t hierarchy_length = 0, hierarchy_data_address = 0;
  if (!read_array_header(memory, snapshot.hierarchy_array, offsets,
                         offsets.skeleton_max_hierarchy_ints,
                         hierarchy_length, hierarchy_data_address) ||
      hierarchy_length > kMaxSingleReadBytes / sizeof(int32_t) ||
      hierarchy_data_address > kMaxUserAddress -
                                   hierarchy_length * sizeof(int32_t)) {
    result.stage = SkeletonReadStage::HierarchyArrayBounds;
    return result;
  }
  snapshot.hierarchy_data.resize(static_cast<size_t>(hierarchy_length));
  if (!memory.read(hierarchy_data_address, snapshot.hierarchy_data.data(),
                   snapshot.hierarchy_data.size() * sizeof(int32_t))) {
    result.stage = SkeletonReadStage::HierarchyDataRead;
    return result;
  }

  result.stage = SkeletonReadStage::Ok;
  return result;
}

std::string_view skeleton_read_stage_name(SkeletonReadStage stage) {
  switch (stage) {
    case SkeletonReadStage::Ok: return "ok";
    case SkeletonReadStage::ProfileDisabled: return "profile";
    case SkeletonReadStage::InvalidCharacter: return "character";
    case SkeletonReadStage::ModelPointer: return "model";
    case SkeletonReadStage::DescriptionPointer: return "description";
    case SkeletonReadStage::BonesArrayPointer: return "bones-pointer";
    case SkeletonReadStage::ConfigFields: return "config-read";
    case SkeletonReadStage::ConfigShape: return "config-shape";
    case SkeletonReadStage::BonesArrayBounds: return "bones-bounds";
    case SkeletonReadStage::BonesDataRead: return "bones-read";
    case SkeletonReadStage::HierarchyPointer: return "hierarchy-pointer";
    case SkeletonReadStage::HierarchyArrayBounds: return "hierarchy-bounds";
    case SkeletonReadStage::HierarchyDataRead: return "hierarchy-read";
    case SkeletonReadStage::PoseData: return "pose-data";
    case SkeletonReadStage::ModelClassMismatch: return "model-class";
    case SkeletonReadStage::DescriptionClassMismatch: return "description-class";
  }
  return "unknown";
}

}  // namespace nzl
