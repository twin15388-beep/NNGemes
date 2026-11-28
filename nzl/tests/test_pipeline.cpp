#include "nzl/esp_pipeline.h"
#include <cstdio>
static int fails=0;
#define C(x) do{ if(!(x)){ std::printf("FAIL %s\n",#x); fails++; } else std::printf("ok: %s\n",#x); }while(0)
int main(){
  const nzl::Vec3f fwd{0,0,1}, right{1,0,0}, up{0,1,0};
  const float identity_q[4] = {0,0,0,1};
  const auto aligned_basis = nzl::select_camera_basis(identity_q, fwd, right, up, true);
  C(aligned_basis.source==nzl::CameraBasisSource::Quaternion);
  C(aligned_basis.axes_valid && aligned_basis.quaternion_valid);
  C(aligned_basis.quaternion_axis_alignment>0.99f && !aligned_basis.disagreement);
  const float yaw90_q[4] = {0,0.70710678f,0,0.70710678f};
  const auto mismatched_basis = nzl::select_camera_basis(yaw90_q, fwd, right, up, true);
  C(mismatched_basis.source==nzl::CameraBasisSource::RawAxes);
  C(mismatched_basis.disagreement && mismatched_basis.quaternion_axis_alignment<0.90f);
  C(mismatched_basis.fwd.z>0.99f);
  const auto axis_preference = nzl::select_camera_basis(identity_q, fwd, right, up, false);
  C(axis_preference.source==nzl::CameraBasisSource::RawAxes);
  const float invalid_q[4] = {0,0,0,0};
  const auto axes_fallback = nzl::select_camera_basis(invalid_q, fwd, right, up, true);
  C(axes_fallback.source==nzl::CameraBasisSource::RawAxes && !axes_fallback.quaternion_valid);
  const auto quat_fallback = nzl::select_camera_basis(identity_q, {}, {}, {}, true);
  C(quat_fallback.source==nzl::CameraBasisSource::Quaternion && !quat_fallback.axes_valid);
  const auto invalid_basis = nzl::select_camera_basis(invalid_q, {}, {}, {}, true);
  C(invalid_basis.source==nzl::CameraBasisSource::Invalid);

  nzl::FakeMemory m; nzl::Offsets o;
  auto blob=[&](uint64_t a){ m.bytes[a]=std::vector<uint8_t>(0x400,0); };
  uint64_t ue=0x100000, game=0x200000, gm=0x300000, bm=0x400000, be=0x500000, we=0x600000, dict=0x700000, entries=0x800000, cam=0x900000, ch=0xA00000, tr=0xB00000;
  for(auto a:{ue,game,gm,bm,be,we,dict,entries,cam,ch,tr}) blob(a);
  m.put<uint64_t>(ue+o.unity_entry_game,game); m.put<uint64_t>(game+o.game_model,gm); m.put<uint64_t>(gm+o.game_battle,bm);
  m.put<uint64_t>(bm+0x90,be); m.put<uint64_t>(be+0x60,we); m.put<uint64_t>(we+0x18,dict); m.put<uint64_t>(bm+0x358,cam);
  m.put<uint64_t>(dict+0x18,entries); m.put<int32_t>(dict+0x20,1);
  m.put<uint64_t>(entries+0x18,1); // x64 IL2CPP array max_length
  uint64_t e0=entries+0x20; m.put<int32_t>(e0,1); m.put<int32_t>(e0+8,42); m.put<uint64_t>(e0+0x10,ch);
  m.put<uint64_t>(ch+0x58,tr); m.put<float>(tr+0x10,0.f); m.put<float>(tr+0x14,0.f); m.put<float>(tr+0x18,10.f);
  m.put<int32_t>(ch+0x38,7); m.put<uint8_t>(ch+0xF0,0);
  auto chain=nzl::resolve_chain(m,ue,o); C(chain.ok);
  auto snaps=nzl::snapshot_players(m,chain,o,{0,0,0}); C(snaps.size()==1); C(snaps[0].id==7); C(snaps[0].dist>9.9f&&snaps[0].dist<10.1f);
  nzl::CameraView c; c.width=1920; c.height=1080; c.fov_deg=90;
  auto cmds=nzl::build_draw_list(c,snaps,400.f); C(cmds.size()==2); C(cmds[0].kind==nzl::DrawCmd::Rect); C(cmds[1].kind==nzl::DrawCmd::Text);
  nzl::PlayerSnap behind=snaps[0]; behind.pos={0,0,-5};
  auto none=nzl::build_draw_list(c,{behind},400.f); C(none.empty());
  if(fails){ std::printf("%d FAILED\n",fails); return 1; } std::printf("ALL PIPELINE TESTS PASSED\n"); return 0;
}
