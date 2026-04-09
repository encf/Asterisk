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
