#include "key_manager.h"

#include <cstdint>
#include <stdexcept>

namespace asterisk2 {

KeyManager::KeyManager(int nP, int id, int seed)
    : nP_(nP), id_(id), helper_id_(nP) {
  if (nP_ < 2) {
    throw std::runtime_error("KeyManager requires at least 2 computing parties");
  }
  if (id_ < 0 || id_ > helper_id_) {
    throw std::runtime_error("KeyManager received invalid party id");
  }

  if (id_ == helper_id_) {
    for (int p = 0; p < helper_id_; ++p) {
      helper_side_keys_.emplace(p, derivePairwiseKey(seed, p));
    }
    return;
  }

  party_key_with_helper_ = derivePairwiseKey(seed, id_);
  party_has_key_ = true;
}

bool KeyManager::hasKeyWithHelper() const {
  return party_has_key_;
}

PairwiseKey KeyManager::keyWithHelper() const {
  if (!party_has_key_) {
    throw std::runtime_error("No helper pairwise key for this role");
  }
  return party_key_with_helper_;
}

bool KeyManager::hasKeyForParty(int party_id) const {
  return helper_side_keys_.find(party_id) != helper_side_keys_.end();
}

PairwiseKey KeyManager::keyForParty(int party_id) const {
  const auto it = helper_side_keys_.find(party_id);
  if (it == helper_side_keys_.end()) {
    throw std::runtime_error("No pairwise key for requested party");
  }
  return it->second;
}

PairwiseKey KeyManager::derivePairwiseKey(int seed, int party_id) {
  PairwiseKey k;
  k.lo = (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) ^
         static_cast<uint32_t>(party_id);
  k.hi = 0x4b4d475250414952ULL ^ static_cast<uint64_t>(party_id);
  return k;
}

}  // namespace asterisk2
