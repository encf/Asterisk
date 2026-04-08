#define BOOST_TEST_MODULE asterisk2_multiplication
#include <boost/test/included/unit_test.hpp>

#include <future>
#include <memory>
#include <unordered_map>

#include <io/netmp.h>
#include <utils/circuit.h>

#include "Asterisk2.0/protocol.h"
#include "utils/types.h"

using common::utils::Field;

struct GlobalFixture {
  GlobalFixture() {
    NTL::ZZ_p::init(NTL::conv<NTL::ZZ>(common::utils::kFieldPrimeDecimal));
  }
};
BOOST_GLOBAL_FIXTURE(GlobalFixture);

BOOST_AUTO_TEST_CASE(single_mul_correctness) {
  NTL::ZZ_pContext ZZ_p_ctx;
  ZZ_p_ctx.save();
  constexpr int nP = 3;
  constexpr int helper = nP;
  constexpr int base_port = 21000;

  common::utils::Circuit<Field> circ;
  auto w0 = circ.newInputWire();
  auto w1 = circ.newInputWire();
  auto wm = circ.addGate(common::utils::GateType::kMul, w0, w1);
  circ.setAsOutput(wm);
  auto level_circ = circ.orderGatesByLevel();

  std::vector<std::future<Field>> parties;
  parties.reserve(nP + 1);
  for (int pid = 0; pid <= nP; ++pid) {
    parties.push_back(std::async(std::launch::async, [&, pid]() {
      ZZ_p_ctx.restore();
      auto network = std::make_shared<io::NetIOMP>(pid, nP + 1, base_port, nullptr, true);
      asterisk2::Protocol proto(nP, pid, network, level_circ, 200);

      auto triples = proto.offline();
      network->sync();

      if (pid == helper) {
        return Field(0);
      }

      std::unordered_map<common::utils::wire_t, Field> inputs;
      inputs[w0] = (pid == 0) ? Field(7) : Field(0);
      inputs[w1] = (pid == 0) ? Field(9) : Field(0);
      auto out = proto.online(inputs, triples);
      BOOST_REQUIRE_EQUAL(out.size(), 1);
      return out[0];
    }));
  }

  Field rec = Field(0);
  for (int pid = 0; pid <= nP; ++pid) {
    auto share = parties[pid].get();
    if (pid < nP) {
      rec += share;
    }
  }

  BOOST_TEST(rec == Field(63));
}
