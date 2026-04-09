#pragma once

#include <cstdint>
#include <unordered_map>

namespace asterisk2 {

struct PairwiseKey {
  uint64_t hi{0};
  uint64_t lo{0};
};

// Session-level key manager for helper <-> computing-party pairwise keys.
// IDs: computing parties 0..nP-1, helper id nP.
class KeyManager {
 public:
  KeyManager(int nP, int id, int seed = 200);

  int helperId() const { return helper_id_; }
  int id() const { return id_; }

  // For computing party (id < helper_id_): key shared with helper.
  bool hasKeyWithHelper() const;
  PairwiseKey keyWithHelper() const;

  // For helper (id == helper_id_): key shared with party_id.
  bool hasKeyForParty(int party_id) const;
  PairwiseKey keyForParty(int party_id) const;

 private:
  static PairwiseKey derivePairwiseKey(int seed, int party_id);

  int nP_;
  int id_;
  int helper_id_;
  std::unordered_map<int, PairwiseKey> helper_side_keys_;
  PairwiseKey party_key_with_helper_{};
  bool party_has_key_{false};
};

}  // namespace asterisk2
