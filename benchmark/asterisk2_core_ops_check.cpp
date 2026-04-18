#include <io/netmp.h>

#include <boost/program_options.hpp>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

#include "utils.h"
#include "Asterisk2.0/protocol.h"
#include "utils/types.h"

using common::utils::Field;
using common::utils::GateType;
using common::utils::wire_t;
using json = nlohmann::json;
namespace bpo = boost::program_options;

namespace {

common::utils::LevelOrderedCircuit buildMulCircuit() {
  common::utils::Circuit<Field> circ;
  auto w0 = circ.newInputWire();
  auto w1 = circ.newInputWire();
  auto out = circ.addGate(GateType::kMul, w0, w1);
  circ.setAsOutput(out);
  return circ.orderGatesByLevel();
}

struct InputCircuitData {
  common::utils::LevelOrderedCircuit circ;
  std::vector<wire_t> inputs;
};

InputCircuitData buildInputCircuit(size_t num_inputs) {
  common::utils::Circuit<Field> circ;
  std::vector<wire_t> inputs;
  inputs.reserve(num_inputs);
  for (size_t i = 0; i < num_inputs; ++i) {
    auto w = circ.newInputWire();
    inputs.push_back(w);
    circ.setAsOutput(w);
  }
  return {circ.orderGatesByLevel(), inputs};
}

std::vector<Field> openSharesToParty0(const std::shared_ptr<io::NetIOMP>& network, size_t nP,
                                      size_t pid, const std::vector<Field>& shares) {
  if (pid >= nP) {
    return {};
  }
  if (pid == 0) {
    std::vector<Field> opened = shares;
    for (size_t peer = 1; peer < nP; ++peer) {
      std::vector<Field> recv(shares.size(), Field(0));
      network->recv(static_cast<int>(peer), recv.data(),
                    recv.size() * common::utils::FIELDSIZE);
      for (size_t i = 0; i < recv.size(); ++i) {
        opened[i] += recv[i];
      }
    }
    return opened;
  }

  network->send(0, shares.data(), shares.size() * common::utils::FIELDSIZE);
  network->flush();
  return {};
}

void checkOpenedSingle(const std::vector<Field>& opened, const Field& expected,
                       const std::string& what) {
  if (opened.size() != 1) {
    throw std::runtime_error(what + " opened output size mismatch");
  }
  if (opened[0] != expected) {
    throw std::runtime_error(what + " opened output mismatch");
  }
}

}  // namespace

void benchmark(const bpo::variables_map& opts) {
  const auto nP = opts["num-parties"].as<size_t>();
  const auto pid = opts["pid"].as<size_t>();
  const auto seed = opts["seed"].as<size_t>();
  const auto repeat = opts["repeat"].as<size_t>();
  const auto port = opts["port"].as<int>();
  const auto op = opts["op"].as<std::string>();
  const auto x_clear = opts["x-clear"].as<int64_t>();
  const auto y_clear = opts["y-clear"].as<int64_t>();
  const auto frac_bits = opts["frac-bits"].as<size_t>();
  const auto ell_x = opts["ell-x"].as<size_t>();
  const auto slack = opts["slack"].as<size_t>();
  const auto security_model_str = opts["security-model"].as<std::string>();

  asterisk2::SecurityModel security_model = asterisk2::SecurityModel::kSemiHonest;
  if (security_model_str == "malicious") {
    security_model = asterisk2::SecurityModel::kMalicious;
  } else if (security_model_str != "semi-honest") {
    throw std::runtime_error("Unsupported security-model, expected semi-honest or malicious");
  }

  if (op != "mul" && op != "trunc") {
    throw std::runtime_error("Unsupported op, expected mul or trunc");
  }
  if (!opts["localhost"].as<bool>()) {
    throw std::runtime_error("Asterisk2.0 core-op check currently supports localhost only");
  }

  auto network = std::make_shared<io::NetIOMP>(pid, nP + 1, port, nullptr, true);
  auto mul_circ = buildMulCircuit();

  asterisk2::ProtocolConfig cfg;
  cfg.security_model = security_model;

  const Field x_field(x_clear);
  const Field y_field(y_clear);
  const auto scale_u64 = static_cast<unsigned long>(1ULL << frac_bits);
  const Field trunc_expected = x_field * inv(NTL::conv<Field>(NTL::to_ZZ(scale_u64)));
  const Field mul_expected = x_field * y_field;

  json output_data;
  output_data["details"] = {{"num-parties", nP},
                            {"pid", pid},
                            {"seed", seed},
                            {"repeat", repeat},
                            {"op", op},
                            {"security_model", security_model_str},
                            {"x_clear", x_clear},
                            {"y_clear", y_clear},
                            {"frac_bits", frac_bits},
                            {"ell_x", ell_x},
                            {"slack", slack}};
  output_data["benchmarks"] = json::array();

  for (size_t r = 0; r < repeat; ++r) {
    asterisk2::Protocol proto(static_cast<int>(nP), static_cast<int>(pid), network, mul_circ,
                              static_cast<int>(seed + 17 * r), cfg);

    Field x_share = Field(0);
    Field y_share = Field(0);
    Field delta_x_share = Field(0);
    Field delta_y_share = Field(0);

    if (security_model == asterisk2::SecurityModel::kSemiHonest) {
      if (pid == 0) {
        x_share = x_field;
        y_share = y_field;
      }
    } else {
      auto input_circ = buildInputCircuit(op == "mul" ? 2 : 1);
      asterisk2::Protocol input_proto(static_cast<int>(nP), static_cast<int>(pid), network,
                                      input_circ.circ, static_cast<int>(seed + 17 * r), cfg);
      auto input_auth = input_proto.mul_offline();
      std::unordered_map<wire_t, Field> clear_inputs;
      clear_inputs[input_circ.inputs[0]] = x_field;
      if (op == "mul") {
        clear_inputs[input_circ.inputs[1]] = y_field;
      }
      auto auth_inputs = input_proto.maliciousInputShareForTesting(clear_inputs, input_auth);
      if (pid < nP) {
        x_share = auth_inputs.x_shares.at(input_circ.inputs[0]);
        delta_x_share = auth_inputs.delta_x_shares.at(input_circ.inputs[0]);
        if (op == "mul") {
          y_share = auth_inputs.x_shares.at(input_circ.inputs[1]);
          delta_y_share = auth_inputs.delta_x_shares.at(input_circ.inputs[1]);
        }
      }
    }

    network->sync();
    StatsPoint offline_start(*network);
    json offline_bench;
    json online_bench;
    Field out_share = Field(0);

    if (op == "mul") {
      auto off_data = proto.mul_offline();
      StatsPoint offline_end(*network);
      offline_bench = offline_end - offline_start;

      network->sync();
      StatsPoint online_start(*network);
      if (security_model == asterisk2::SecurityModel::kSemiHonest) {
        std::unordered_map<wire_t, Field> inputs;
        if (pid < nP) {
          inputs[0] = x_share;
          inputs[1] = y_share;
          auto out = proto.onlineSemiHonestForBenchmark(inputs, off_data.triples);
          out_share = out.at(0);
        }
      } else if (pid < nP) {
        auto auth_out = proto.mul_online_malicious_single(x_share, delta_x_share, y_share,
                                                          delta_y_share, off_data);
        out_share = auth_out.share;
      } else {
        (void)proto.mul_online_malicious_single(x_share, delta_x_share, y_share, delta_y_share,
                                                off_data);
      }
      StatsPoint online_end(*network);
      online_bench = online_end - online_start;
    } else {
      asterisk2::TruncOfflineData off_data;
      if (security_model == asterisk2::SecurityModel::kSemiHonest) {
        off_data = proto.trunc_offline(1, ell_x, frac_bits, slack);
      } else {
        off_data = proto.trunc_offline_malicious(1, ell_x, frac_bits, slack);
      }
      StatsPoint offline_end(*network);
      offline_bench = offline_end - offline_start;

      network->sync();
      StatsPoint online_start(*network);
      if (security_model == asterisk2::SecurityModel::kSemiHonest) {
        if (pid < nP) {
          auto out = proto.trunc_online(std::vector<Field>{x_share}, off_data);
          out_share = out.at(0);
        } else {
          (void)proto.trunc_online(std::vector<Field>{Field(0)}, off_data);
        }
      } else if (pid < nP) {
        auto out =
            proto.trunc_online_malicious(std::vector<Field>{x_share},
                                         std::vector<Field>{delta_x_share}, off_data);
        out_share = out.trunc_x_shares.at(0);
      } else {
        (void)proto.trunc_online_malicious(std::vector<Field>{x_share},
                                           std::vector<Field>{delta_x_share}, off_data);
      }
      StatsPoint online_end(*network);
      online_bench = online_end - online_start;
    }

    network->sync();
    const auto opened =
        openSharesToParty0(network, nP, pid, std::vector<Field>{out_share});
    if (pid == 0) {
      checkOpenedSingle(opened, op == "mul" ? mul_expected : trunc_expected, op);
    }
    network->sync();

    size_t offline_bytes = 0;
    for (const auto& val : offline_bench["communication"]) {
      offline_bytes += val.get<uint64_t>();
    }
    size_t online_bytes = 0;
    for (const auto& val : online_bench["communication"]) {
      online_bytes += val.get<uint64_t>();
    }

    json row = {{"offline", offline_bench},
                {"online", online_bench},
                {"offline_bytes", offline_bytes},
                {"online_bytes", online_bytes},
                {"passed", true}};
    if (pid == 0) {
      row["opened_output"] = NTL::conv<uint64_t>(NTL::rep(opened[0]));
      row["expected_output"] =
          NTL::conv<uint64_t>(NTL::rep(op == "mul" ? mul_expected : trunc_expected));
    }
    output_data["benchmarks"].push_back(std::move(row));
  }

  if (opts.count("output") != 0) {
    saveJson(output_data, opts["output"].as<std::string>());
  }
}

bpo::options_description programOptions() {
  bpo::options_description desc("Asterisk2.0 core operation benchmark options");
  desc.add_options()
      ("num-parties,n", bpo::value<size_t>()->required(), "Number of computing parties.")
      ("pid,p", bpo::value<size_t>()->required(), "Party ID.")
      ("seed", bpo::value<size_t>()->default_value(200), "Value of the random seed.")
      ("repeat,r", bpo::value<size_t>()->default_value(1), "Number of repetitions.")
      ("op", bpo::value<std::string>()->required(), "Operation: mul or trunc.")
      ("x-clear", bpo::value<int64_t>()->default_value(2560),
       "Clear signed input x. For trunc, this is the pre-truncation encoded value.")
      ("y-clear", bpo::value<int64_t>()->default_value(1792),
       "Clear signed input y. Used only for mul.")
      ("frac-bits", bpo::value<size_t>()->default_value(8), "Fixed-point fractional bits.")
      ("ell-x", bpo::value<size_t>()->default_value(40), "Truncation ell_x.")
      ("slack", bpo::value<size_t>()->default_value(8), "Truncation slack.")
      ("security-model", bpo::value<std::string>()->default_value("semi-honest"),
       "Security model: semi-honest or malicious.")
      ("localhost", bpo::bool_switch(), "All parties are on same machine.")
      ("port", bpo::value<int>()->default_value(10000), "Base port for networking.")
      ("output,o", bpo::value<std::string>(), "File to save benchmarks.");
  return desc;
}

int main(int argc, char* argv[]) {
  ZZ_p::init(conv<ZZ>(common::utils::kFieldPrimeDecimal));

  auto prog_opts(programOptions());
  bpo::options_description cmdline("Benchmark Asterisk2.0 core operations.");
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
