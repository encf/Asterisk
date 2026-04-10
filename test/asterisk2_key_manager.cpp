#define BOOST_TEST_MODULE asterisk2_key_manager
#include <boost/test/included/unit_test.hpp>

#include "Asterisk2.0/key_manager.h"

BOOST_AUTO_TEST_CASE(helper_and_party_see_same_pairwise_key) {
  constexpr int nP = 3;
  constexpr int seed = 200;
  constexpr int helper = nP;

  asterisk2::KeyManager helper_km(nP, helper, seed);
  for (int pid = 0; pid < nP; ++pid) {
    asterisk2::KeyManager party_km(nP, pid, seed);
    BOOST_TEST(party_km.hasKeyWithHelper());
    BOOST_TEST(helper_km.hasKeyForParty(pid));
    const auto pkey = party_km.keyWithHelper();
    const auto hkey = helper_km.keyForParty(pid);
    BOOST_TEST(pkey.hi == hkey.hi);
    BOOST_TEST(pkey.lo == hkey.lo);
  }
}

BOOST_AUTO_TEST_CASE(computing_party_does_not_hold_other_parties_keys) {
  constexpr int nP = 3;
  asterisk2::KeyManager party_km(nP, 0, 200);
  BOOST_TEST(party_km.hasKeyWithHelper());
  BOOST_CHECK_THROW((void)party_km.keyForParty(1), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(computing_parties_share_kp_and_helper_does_not_have_it) {
  constexpr int nP = 3;
  constexpr int seed = 200;
  constexpr int helper = nP;

  asterisk2::KeyManager p0(nP, 0, seed);
  asterisk2::KeyManager p1(nP, 1, seed);
  asterisk2::KeyManager p2(nP, 2, seed);
  asterisk2::KeyManager h(nP, helper, seed);

  BOOST_TEST(p0.hasComputingPartiesKey());
  BOOST_TEST(p1.hasComputingPartiesKey());
  BOOST_TEST(p2.hasComputingPartiesKey());
  BOOST_TEST(!h.hasComputingPartiesKey());

  const auto k0 = p0.computingPartiesKey();
  const auto k1 = p1.computingPartiesKey();
  const auto k2 = p2.computingPartiesKey();
  BOOST_TEST(k0.hi == k1.hi);
  BOOST_TEST(k0.lo == k1.lo);
  BOOST_TEST(k0.hi == k2.hi);
  BOOST_TEST(k0.lo == k2.lo);
  BOOST_CHECK_THROW((void)h.computingPartiesKey(), std::runtime_error);
}
