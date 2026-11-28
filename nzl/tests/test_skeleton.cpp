#include "nzl/skeleton.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::printf("FAIL %s\n", #condition);                                  \
      ++failures;                                                              \
    } else {                                                                   \
      std::printf("ok: %s\n", #condition);                                   \
    }                                                                          \
  } while (false)

constexpr uint64_t kCharacter = 0xA00000;
constexpr uint64_t kSkeleton = 0xB00000;
constexpr uint64_t kConfig = 0xC00000;
constexpr uint64_t kBonesArray = 0xD00000;
constexpr uint64_t kHierarchyArray = 0xE00000;
constexpr int32_t kBoneCount = 3;
constexpr int32_t kBoneStride = 9;
constexpr uint64_t kPoseFloatCount = kBoneCount * kBoneStride;

void put_float_array(nzl::FakeMemory& memory, uint64_t address,
                     const std::vector<float>& values) {
  std::vector<uint8_t> bytes(0x20 + values.size() * sizeof(float), 0);
  std::memcpy(bytes.data() + 0x20, values.data(),
              values.size() * sizeof(float));
  memory.bytes[address] = std::move(bytes);
  memory.put<uint64_t>(address + 0x18, values.size());
}

void put_int_array(nzl::FakeMemory& memory, uint64_t address,
                   const std::vector<int32_t>& values) {
  std::vector<uint8_t> bytes(0x20 + values.size() * sizeof(int32_t), 0);
  std::memcpy(bytes.data() + 0x20, values.data(),
              values.size() * sizeof(int32_t));
  memory.bytes[address] = std::move(bytes);
  memory.put<uint64_t>(address + 0x18, values.size());
}

nzl::FakeMemory make_memory(const nzl::Offsets& offsets) {
  nzl::FakeMemory memory;
  memory.bytes[kCharacter] = std::vector<uint8_t>(0x200, 0);
  memory.bytes[kSkeleton] = std::vector<uint8_t>(0x40, 0);
  memory.bytes[kConfig] = std::vector<uint8_t>(0x80, 0);

  memory.put<uint64_t>(kCharacter + offsets.skeleton_character_model,
                       kSkeleton);
  memory.put<uint64_t>(kSkeleton + offsets.skeleton_model_description,
                       kConfig);
  memory.put<uint64_t>(kSkeleton + offsets.skeleton_model_bones_data,
                       kBonesArray);

  memory.put<int32_t>(kConfig + offsets.skeleton_config_bones_count,
                      kBoneCount);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_bone_data_size,
                      kBoneStride);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_local_pose_offset, 0);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_model_pose_offset, 4);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_bone_length_offset, 8);
  memory.put<uint64_t>(kConfig + offsets.skeleton_config_hierarchy_data,
                       kHierarchyArray);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_hierarchy_max_children,
                      2);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_root_index, 0);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_legs_index, 1);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_spine2_index, 1);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_spine3_index, 2);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_neck_index, 2);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_head1_index, 2);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_head2_index, 2);

  std::vector<float> pose(kPoseFloatCount, 0.0f);
  for (int32_t i = 0; i < kBoneCount; ++i) {
    float* record = pose.data() + static_cast<size_t>(i) * kBoneStride;
    record[3] = 1.0f;  // local identity quaternion
    record[7] = 1.0f;  // model identity quaternion
    record[8] = 0.1f * static_cast<float>(i + 1);
  }
  put_float_array(memory, kBonesArray, pose);
  put_int_array(memory, kHierarchyArray, {0, 1, 2, -1, 0, 1});
  return memory;
}
}  // namespace

int main() {
  const nzl::Offsets offsets;
  CHECK(offsets.profile_configured);
  CHECK(offsets.skeleton_offsets_configured);
  CHECK(offsets.skeleton_character_model == 0x138);
  CHECK(offsets.skeleton_model_description == 0x10);
  CHECK(offsets.skeleton_model_bones_data == 0x18);

  auto memory = make_memory(offsets);
  auto result = nzl::read_skeleton_snapshot(memory, kCharacter, offsets);
  CHECK(result.ok());
  CHECK(result.snapshot.model == kSkeleton);
  CHECK(result.snapshot.description == kConfig);
  CHECK(result.snapshot.bones_array == kBonesArray);
  CHECK(result.snapshot.hierarchy_array == kHierarchyArray);
  CHECK(result.snapshot.bones_count == kBoneCount);
  CHECK(result.snapshot.bone_data_size == kBoneStride);
  CHECK(result.snapshot.root_index == 0);
  CHECK(result.snapshot.head1_index == 2);
  CHECK(result.snapshot.bones.size() == kBoneCount);
  CHECK(result.snapshot.hierarchy_data.size() == 6);
  CHECK(result.snapshot.valid_pose_records == kBoneCount);
  CHECK(std::fabs(result.snapshot.bones[1].length - 0.2f) < 1e-6f);
  CHECK(result.snapshot.bones[2].values_valid);

  auto unknown = nzl::Offsets(nzl::game_profile::GameProfile{});
  CHECK(nzl::read_skeleton_snapshot(memory, kCharacter, unknown).stage ==
        nzl::SkeletonReadStage::ProfileDisabled);
  CHECK(nzl::read_skeleton_snapshot(memory, 0x1000, offsets).stage ==
        nzl::SkeletonReadStage::InvalidCharacter);

  memory.put<uint64_t>(kCharacter + offsets.skeleton_character_model, 0);
  CHECK(nzl::read_skeleton_snapshot(memory, kCharacter, offsets).stage ==
        nzl::SkeletonReadStage::ModelPointer);
  memory.put<uint64_t>(kCharacter + offsets.skeleton_character_model,
                       kSkeleton);

  memory.put<int32_t>(kConfig + offsets.skeleton_config_head1_index, -1);
  CHECK(nzl::read_skeleton_snapshot(memory, kCharacter, offsets).stage ==
        nzl::SkeletonReadStage::ConfigShape);
  memory.put<int32_t>(kConfig + offsets.skeleton_config_head1_index, 2);

  memory.put<uint64_t>(kBonesArray + offsets.array_length, 8);
  CHECK(nzl::read_skeleton_snapshot(memory, kCharacter, offsets).stage ==
        nzl::SkeletonReadStage::BonesArrayBounds);
  memory.put<uint64_t>(kBonesArray + offsets.array_length, kPoseFloatCount);

  // A non-normalized/corrupt quaternion is rejected instead of becoming a
  // misleading skeleton point.
  float invalid_quaternion[4] = {0, 0, 0, 0};
  std::memcpy(memory.bytes[kBonesArray].data() + 0x20,
              invalid_quaternion, sizeof(invalid_quaternion));
  CHECK(nzl::read_skeleton_snapshot(memory, kCharacter, offsets).stage ==
        nzl::SkeletonReadStage::PoseData);

  if (failures) {
    std::printf("%d skeleton checks failed\n", failures);
    return 1;
  }
  std::printf("ALL SKELETON TESTS PASSED\n");
  return 0;
}
