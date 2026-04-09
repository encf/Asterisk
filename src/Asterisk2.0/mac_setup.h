#pragma once

#include <memory>

#include "../io/netmp.h"
#include "../utils/types.h"

namespace asterisk2 {

using common::utils::Field;

struct MacSetupPartyShares {
  Field delta_share{Field(0)};
  Field delta_inv_share{Field(0)};
  bool ready{false};
};

struct MacSetupHelperState {
  Field delta{Field(0)};
  Field delta_inv{Field(0)};
  bool ready{false};
};

struct MacSetupResult {
  MacSetupPartyShares party;
  MacSetupHelperState helper;
};

// Pi_MACSetup-DH(P, P_{n+1})
// nP: number of computing parties (IDs 0..nP-1), helper ID is nP.
MacSetupResult runMacSetupDH(int nP, int id, std::shared_ptr<io::NetIOMP> network,
                             int seed = 200);

}  // namespace asterisk2
