#pragma once

#include "nzl/chain.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace nzl {

// One record decoded from SkeletonModel._bonesData using the live
// SkeletonConfig's float-index offsets. These are genuine model pose inputs,
// not world-space joint positions.
struct SkeletonBonePose {
  std::array<float, 4> local_position_orientation{};
  std::array<float, 4> model_orientation{};
  float length = 0.0f;
  bool values_valid = false;
};

struct SkeletonSnapshot {
  uint64_t character = 0;
  uint64_t model = 0;
  uint64_t description = 0;
  uint64_t bones_array = 0;
  uint64_t hierarchy_array = 0;
  int32_t bones_count = 0;
  int32_t bone_data_size = 0;  // floats per bone, read from SkeletonConfig
  int32_t local_pose_offset = 0;
  int32_t model_pose_offset = 0;
  int32_t bone_length_offset = 0;
  int32_t hierarchy_max_children = 0;
  int32_t root_index = -1;
  int32_t legs_index = -1;
  int32_t spine2_index = -1;
  int32_t spine3_index = -1;
  int32_t neck_index = -1;
  int32_t head1_index = -1;
  int32_t head2_index = -1;
  std::vector<SkeletonBonePose> bones;
  // Retained verbatim for later hierarchy decoding; its encoding is not
  // assumed by this reader.
  std::vector<int32_t> hierarchy_data;
  std::size_t valid_pose_records = 0;
};

enum class SkeletonReadStage {
  Ok,
  ProfileDisabled,
  InvalidCharacter,
  ModelPointer,
  DescriptionPointer,
  BonesArrayPointer,
  ConfigFields,
  ConfigShape,
  BonesArrayBounds,
  BonesDataRead,
  HierarchyPointer,
  HierarchyArrayBounds,
  HierarchyDataRead,
  PoseData,
  ModelClassMismatch,
  DescriptionClassMismatch,
};

struct SkeletonReadResult {
  SkeletonReadStage stage = SkeletonReadStage::ProfileDisabled;
  SkeletonSnapshot snapshot{};
  bool ok() const { return stage == SkeletonReadStage::Ok; }
};

// Read the bounded raw skeleton snapshot. The Windows overlay additionally
// class-gates SkeletonModel and SkeletonConfig before calling this function.
// This intentionally does not convert model pose data to world-space points.
SkeletonReadResult read_skeleton_snapshot(const IMemory& memory,
                                           uint64_t character,
                                           const Offsets& offsets);
std::string_view skeleton_read_stage_name(SkeletonReadStage stage);

}  // namespace nzl
