#include "nzl/chain.h"
#include <cstdio>
static int fails=0;
#define C(x) do{ if(!(x)){ std::printf("FAIL %s\n",#x); fails++; } else std::printf("ok: %s\n",#x); }while(0)
int main(){
  nzl::FakeMemory m; nzl::Offsets o;
  // blob helper: allocate objects as blobs of 0x400 bytes zeroed
  auto blob=[&](uint64_t a){ m.bytes[a]=std::vector<uint8_t>(0x400,0); };
  uint64_t ue=0x100000, game=0x200000, gm=0x300000, bm=0x400000, be=0x500000, we=0x600000, dict=0x700000, entries=0x800000, cam=0x900000, ch=0xA00000, tr=0xB00000;
  for(auto a:{ue,game,gm,bm,be,we,dict,entries,cam,ch,tr}) blob(a);
  m.put<uint64_t>(ue+o.unity_entry_game,game); m.put<uint64_t>(game+o.game_model,gm); m.put<uint64_t>(gm+o.game_battle,bm);
  m.put<uint64_t>(bm+0x90,be); m.put<uint64_t>(be+0x60,we); m.put<uint64_t>(we+0x18,dict); m.put<uint64_t>(bm+0x358,cam);
  m.put<uint64_t>(dict+0x18,entries); m.put<int32_t>(dict+0x20,1);
  m.put<uint64_t>(entries+0x18,1); // x64 IL2CPP array max_length
  uint64_t e0=entries+0x20; m.put<int32_t>(e0,1); m.put<int32_t>(e0+8,42); m.put<uint64_t>(e0+0x10,ch);
  m.put<uint64_t>(ch+0x58,tr); m.put<float>(tr+0x10,1.5f); m.put<float>(tr+0x14,2.5f); m.put<float>(tr+0x18,-3.5f);
  auto r=nzl::resolve_chain(m,ue,o); C(r.ok); C(r.camera_model==cam); C(r.dict==dict);
  auto h=nzl::read_dict(m,dict,o); C(h.has_value()&&h->count==1);
  auto v=nzl::dict_value_at(m,*h,0,o); C(v.has_value()&&*v==ch);
  auto p=nzl::read_char_pos(m,ch,o); C(p.has_value()&&p->x==1.5f&&p->z==-3.5f);
  auto bad=nzl::resolve_chain(m,0xdead,o); C(!bad.ok);
  auto bad_capacity = [&]() {
    m.put<uint64_t>(entries + o.array_length, UINT64_C(0x100000000));
    const bool rejected = !nzl::read_dict(m, dict, o).has_value();
    m.put<uint64_t>(entries + o.array_length, 1);
    return rejected;
  };
  C(bad_capacity()); // x64 capacity is read as a full size_t, not truncated
  m.put<int32_t>(dict + o.dict_count, 2);
  C(!nzl::read_dict(m, dict, o).has_value()); // Count must not exceed capacity
  m.put<int32_t>(dict + o.dict_count, 1);
  m.put<int32_t>(e0, -1);
  C(!nzl::dict_value_at(m, *h, 0, o).has_value()); // free Dictionary slots are ignored
  // Regression for "видит не всех": with Count=2 and ArrayLength=8 the live
  // entries can occupy slots 5 and 6. Bounding the scan by Count returned
  // nothing at all for such a dictionary.
  {
    auto sparse = [&]() {
      m.put<int32_t>(dict + o.dict_count, 2);
      m.put<uint64_t>(entries + o.array_length, 8);
      const auto slot = [&](int index) {
        return entries + o.array_data + static_cast<uint64_t>(index) * o.dict_stride;
      };
      for (int index = 0; index < 8; ++index) m.put<int32_t>(slot(index), -1);
      m.put<int32_t>(slot(5), 11); m.put<uint64_t>(slot(5) + o.dict_value, ch);
      m.put<int32_t>(slot(6), 12); m.put<uint64_t>(slot(6) + o.dict_value, ch);
      const auto sparse_header = nzl::read_dict(m, dict, o);
      const bool header_ok = sparse_header.has_value() &&
                             sparse_header->count == 2 &&
                             sparse_header->array_length == 8;
      const auto found5 = nzl::dict_value_at(m, *sparse_header, 5, o);
      const auto found6 = nzl::dict_value_at(m, *sparse_header, 6, o);
      const bool holes_ignored =
          !nzl::dict_value_at(m, *sparse_header, 0, o).has_value() &&
          !nzl::dict_value_at(m, *sparse_header, 7, o).has_value();
      const bool past_capacity =
          !nzl::dict_value_at(m, *sparse_header, 8, o).has_value();
      return header_ok && found5 && *found5 == ch && found6 && *found6 == ch &&
             holes_ignored && past_capacity;
    };
    C(sparse());
    m.put<int32_t>(dict + o.dict_count, 1);
    m.put<uint64_t>(entries + o.array_length, 1);
  }
  nzl::Offsets unknown_profile(nzl::game_profile::GameProfile{});
  const auto unknown_chain = nzl::resolve_chain(m, ue, unknown_profile);
  C(!unknown_chain.ok && unknown_chain.stage == "profile.unconfigured");
  if(fails){ std::printf("%d FAILED\n",fails); return 1; } std::printf("ALL NZL TESTS PASSED\n"); return 0;
}
// pipeline checks appended via separate main below are in test_pipeline.cpp
