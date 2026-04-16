#include <io/netmp.h>

#include <boost/program_options.hpp>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "utils.h"
#include "Asterisk2.0/protocol.h"
#include "utils/types.h"

using common::utils::Field;
using json = nlohmann::json;
namespace bpo = boost::program_options;

namespace {

common::utils::Circuit<Field> buildMulCircuit() {
  common::utils::Circuit<Field> circ;
  auto w0 = circ.newInputWire();
  auto w1 = circ.newInputWire();
  auto out = circ.addGate(common::utils::GateType::kMul, w0, w1);
  circ.setAsOutput(out);
  return circ;
}

}  // namespace

void benchmark(const bpo::variables_map& opts) {
  const auto nP = opts["num-parties"].as<size_t>();
  const auto pid = opts["pid"].as<size_t>();
  const auto seed = opts["seed"].as<size_t>();
  const auto repeat = opts["repeat"].as<size_t>();
  const auto port = opts["port"].as<int>();
  const auto x_clear = opts["x-clear"].as<int64_t>();
  const auto y_clear = opts["y-clear"].as<int64_t>();
  const auto security_model_str = opts["security-model"].as<std::string>();

  asterisk2::SecurityModel security_model = asterisk2::SecurityModel::kSemiHonest;
  if (security_model_str == "malicious") {
    security_model = asterisk2::SecurityModel::kMalicious;
  } else if (security_model_str != "semi-honest") {
    throw std::runtime_error("Unsupported security-model, expected semi-honest or malicious");
  }

  if (!opts["localhost"].as<bool>()) {
    throw std::runtime_error("Asterisk2.0 multiplication check currently supports localhost only");
  }

  auto network = std::make_shared<io::NetIOMP>(pid, nP + 1, port, nullptr, true);
  auto circ = buildMulCircuit().orderGatesByLevel();

  std::unordered_map<common::utils::wire_t, Field> inputs;
  if (pid < nP) {
    inputs[0] = (pid == 0) ? Field(x_clear) : Field(0);
    inputs[1] = (pid == 0) ? Field(y_clear) : Field(0);
  }

  json output_data;
  output_data["details"] = {{"num-parties", nP},
                            {"pid", pid},
                            {"seed", seed},
                            {"repeat", repeat},
                            {"security_model", security_model_str},
                            {"x_clear", x_clear},
                            {"y_clear", y_clear}};
  output_data["benchmarks"] = json::array();

  for (size_t r = 0; r < repeat; ++r) {
    asterisk2::ProtocolConfig cfg;
    cfg.security_model = security_model;
    asterisk2::Protocol proto(static_cast<int>(nP), static_cast<int>(pid), network, circ,
                              static_cast<int>(seed), cfg);

    network->sync();
    StatsPoint offline_start(*network);
    auto off_data = proto.mul_offline();
    StatsPoint offline_end(*network);

    network->sync();
    StatsPoint online_start(*network);
    Field out_share = Field(0);
    Field delta_out_share = Field(0);
    Field delta_share = Field(0);
    if (security_model == asterisk2::SecurityModel::kSemiHonest) {
      auto out = proto.mul_online(inputs, off_data);
      if (pid < nP && out.size() != 1) {
        throw std::runtime_error("Expected exactly one multiplication output");
      }
      if (pid < nP) {
        out_share = out[0];
      }
    } else {
      auto auth_inputs = proto.maliciousInputShareForTesting(inputs, off_data);
      if (pid < nP) {
        auto auth_out = proto.mul_online_malicious_single(auth_inputs.x_shares.at(0),
                                                          auth_inputs.delta_x_shares.at(0),
                                                          auth_inputs.x_shares.at(1),
                                                          auth_inputs.delta_x_shares.at(1),
                                                          off_data);
        out_share = auth_out.share;
        delta_out_share = auth_out.delta_share;
        delta_share = off_data.delta_share;
      }
    }
    StatsPoint online_end(*network);

    auto offline_bench = offline_end - offline_start;
    auto online_bench = online_end - online_start;

    size_t offline_bytes = 0;
    for (const auto& val : offline_bench["communication"]) {
      offline_bytes += val.get<int64_t>();
    }
    size_t online_bytes = 0;
    for (const auto& val : online_bench["communication"]) {
      online_bytes += val.get<int64_t>();
    }

    output_data["benchmarks"].push_back({
        {"offline", offline_bench},
        {"online", online_bench},
        {"offline_bytes", offline_bytes},
        {"online_bytes", online_bytes},
        {"output_share", (pid < nP) ? NTL::conv<uint64_t>(NTL::rep(out_share)) : 0},
        {"delta_output_share", (pid < nP) ? NTL::conv<uint64_t>(NTL::rep(delta_out_share)) : 0},
        {"delta_share", (pid < nP) ? NTL::conv<uint64_t>(NTL::rep(delta_share)) : 0},
    });
  }

  if (opts.count("output") != 0) {
    saveJson(output_data, opts["output"].as<std::string>());
  }
}

bpo::options_description programOptions() {
  bpo::options_description desc("Asterisk2.0 single multiplication check options");
  desc.add_options()
      ("num-parties,n", bpo::value<size_t>()->required(), "Number of computing parties.")
      ("pid,p", bpo::value<size_t>()->required(), "Party ID.")
      ("seed", bpo::value<size_t>()->default_value(200), "Value of the random seed.")
      ("security-model", bpo::value<std::string>()->default_value("semi-honest"),
       "Security model: semi-honest or malicious.")
      ("repeat,r", bpo::value<size_t>()->default_value(1), "Number of repetitions.")
      ("x-clear", bpo::value<int64_t>()->required(), "Clear signed input x.")
      ("y-clear", bpo::value<int64_t>()->required(), "Clear signed input y.")
      ("localhost", bpo::bool_switch(), "All parties are on same machine.")
      ("port", bpo::value<int>()->default_value(10000), "Base port for networking.")
      ("output,o", bpo::value<std::string>(), "File to save benchmarks.");
  return desc;
}

int main(int argc, char* argv[]) {
  ZZ_p::init(conv<ZZ>(common::utils::kFieldPrimeDecimal));

  auto prog_opts(programOptions());
  bpo::options_description cmdline("Benchmark Asterisk2.0 single multiplication.");
  cmdline.add(prog_opts);
  cmdline.add_options()("help,h", "produce help message");

  bpo::variables_map opts;
  bpo::store(bpo::command_line_parser(argc, argv).options(cmdline).run(), opts);

  if (opts.count("help") != 0) {
    std::cout << cmdline << std::endl;
    return 0;
  }

  try {
    bpo::notify(opts);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }

  try {
    benchmark(opts);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\nFatal error" << std::endl;
    return 1;
  }

  return 0;
}
