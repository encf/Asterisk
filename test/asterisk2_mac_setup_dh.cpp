#define BOOST_TEST_MODULE asterisk2_mac_setup_dh
#include <boost/test/included/unit_test.hpp>

#include <future>
#include <memory>
#include <vector>

#include <io/netmp.h>

#include "Asterisk2.0/mac_setup.h"
#include "utils/types.h"

using common::utils::Field;

struct GlobalFixture {
  GlobalFixture() {
    NTL::ZZ_p::init(NTL::conv<NTL::ZZ>(common::utils::kFieldPrimeDecimal));
  }
};
BOOST_GLOBAL_FIXTURE(GlobalFixture);

BOOST_AUTO_TEST_CASE(mac_setup_dh_invariants_and_visibility) {
  NTL::ZZ_pContext ZZ_p_ctx;
  ZZ_p_ctx.save();

  constexpr int nP = 3;
  constexpr int helper = nP;
  constexpr int base_port = 21600;

  struct LocalOut {
    bool is_helper{false};
    asterisk2::MacSetupResult out;
  };

  std::vector<std::future<LocalOut>> futures;
  futures.reserve(nP + 1);
  for (int pid = 0; pid <= nP; ++pid) {
    futures.push_back(std::async(std::launch::async, [&, pid]() {
      ZZ_p_ctx.restore();
      auto net = std::make_shared<io::NetIOMP>(pid, nP + 1, base_port, nullptr, true);
      LocalOut ret;
      ret.is_helper = (pid == helper);
      asterisk2::KeyManager km(nP, pid, 200);
      ret.out = asterisk2::runMacSetupDH(nP, pid, net, km, 200);
      return ret;
    }));
  }

  Field sum_delta_shares = Field(0);
  Field sum_delta_inv_shares = Field(0);
  Field delta = Field(0);
  Field delta_inv = Field(0);

  for (int pid = 0; pid <= nP; ++pid) {
    auto got = futures[pid].get();
    if (got.is_helper) {
      BOOST_TEST(got.out.helper.ready);
      BOOST_TEST(!got.out.party.ready);
      delta = got.out.helper.delta;
      delta_inv = got.out.helper.delta_inv;
      continue;
    }

    BOOST_TEST(got.out.party.ready);
    BOOST_TEST(!got.out.helper.ready);
    // Computing parties must not receive helper plaintext state.
    BOOST_TEST(got.out.helper.delta == Field(0));
    BOOST_TEST(got.out.helper.delta_inv == Field(0));
    sum_delta_shares += got.out.party.delta_share;
    sum_delta_inv_shares += got.out.party.delta_inv_share;
  }

  BOOST_TEST(sum_delta_shares == delta);
  BOOST_TEST(sum_delta_inv_shares == delta_inv);
  BOOST_TEST(delta * delta_inv == Field(1));
}
