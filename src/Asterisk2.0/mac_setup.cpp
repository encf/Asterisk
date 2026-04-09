#include "mac_setup.h"

#include <cstdint>
#include <stdexcept>

#include <emp-tool/emp-tool.h>

namespace asterisk2 {
namespace {
using emp::block;

constexpr uint64_t kPrime64Minus59 = 18446744073709551557ULL;

// Domain separation labels for Pi_MACSetup-DH.
enum class MacPrgLabel : uint64_t {
  kMacDelta = 0x4d41435f44454c54ULL,      // "MAC_DELT"
  kMacDeltaInv = 0x4d41435f44494e56ULL,   // "MAC_DINV"
  kMacDeltaMaster = 0x4d41435f4d415354ULL // "MAC_MAST"
};

emp::PRG makePrg(int seed, int party_id, uint64_t idx, MacPrgLabel label) {
  const uint64_t lo = (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) ^
                      static_cast<uint32_t>(party_id);
  const uint64_t hi = idx ^ static_cast<uint64_t>(label);
  block key = emp::makeBlock(hi, lo);
  return emp::PRG(&key, 0);
}

uint64_t prgUint64(emp::PRG& prg) {
  uint64_t v = 0;
  prg.random_data(&v, sizeof(v));
  return v;
}

Field prgField(emp::PRG& prg) {
  return NTL::conv<Field>(NTL::to_ZZ(prgUint64(prg) % kPrime64Minus59));
}

Field prgNonZeroField(emp::PRG& prg) {
  Field f = Field(0);
  while (f == Field(0)) {
    f = prgField(prg);
  }
  return f;
}

}  // namespace

MacSetupResult runMacSetupDH(int nP, int id, std::shared_ptr<io::NetIOMP> network, int seed) {
  if (nP < 2) {
    throw std::runtime_error("Pi_MACSetup-DH requires at least 2 computing parties");
  }
  if (id < 0 || id > nP) {
    throw std::runtime_error("Pi_MACSetup-DH received invalid party id");
  }
  const int helper_id = nP;

  MacSetupResult out;

  if (id == helper_id) {
    // 1) Sample Delta in F_p^* and 2) compute inverse.
    auto master_prg = makePrg(seed, helper_id, 0, MacPrgLabel::kMacDeltaMaster);
    const Field delta = prgNonZeroField(master_prg);
    const Field delta_inv = NTL::inv(delta);

    // 3) Re-derive (delta_i, eta_i) for i in [0, nP-2].
    Field sum_delta = Field(0);
    Field sum_delta_inv = Field(0);
    for (int i = 0; i <= nP - 2; ++i) {
      auto delta_prg = makePrg(seed, i, 0, MacPrgLabel::kMacDelta);
      auto inv_prg = makePrg(seed, i, 0, MacPrgLabel::kMacDeltaInv);
      sum_delta += prgField(delta_prg);
      sum_delta_inv += prgField(inv_prg);
    }

    // 4) Compute completion shares for P_n (id nP-1).
    Field pack[2] = {delta - sum_delta, delta_inv - sum_delta_inv};

    // 5) Send (delta_n, eta_n) to last computing party only.
    network->send(nP - 1, pack, 2 * common::utils::FIELDSIZE);
    network->flush();

    // 7) Helper keeps plaintext state.
    out.helper.delta = delta;
    out.helper.delta_inv = delta_inv;
    out.helper.ready = true;
    return out;
  }

  // 6) Computing parties store local shares only.
  if (id <= nP - 2) {
    auto delta_prg = makePrg(seed, id, 0, MacPrgLabel::kMacDelta);
    auto inv_prg = makePrg(seed, id, 0, MacPrgLabel::kMacDeltaInv);
    out.party.delta_share = prgField(delta_prg);
    out.party.delta_inv_share = prgField(inv_prg);
    out.party.ready = true;
    return out;
  }

  // Last computing party receives completion shares from helper.
  Field pack[2];
  network->recv(helper_id, pack, 2 * common::utils::FIELDSIZE);
  out.party.delta_share = pack[0];
  out.party.delta_inv_share = pack[1];
  out.party.ready = true;
  return out;
}

}  // namespace asterisk2
