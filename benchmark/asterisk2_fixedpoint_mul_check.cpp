#include <io/netmp.h>
#include <utils/circuit.h>

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

json zeroBench(size_t party_count) {
  json comm = json::array();
  for (size_t i = 0; i < party_count; ++i) {
    comm.push_back(0);
  }
  return {{"time", 0.0}, {"communication", comm}};
}

void addBench(json& acc, const json& delta) {
  acc["time"] = acc["time"].get<double>() + delta["time"].get<double>();
  for (size_t i = 0; i < delta["communication"].size(); ++i) {
    acc["communication"][i] =
        acc["communication"][i].get<uint64_t>() + delta["communication"][i].get<uint64_t>();
  }
}

common::utils::LevelOrderedCircuit buildSingleMulCircuit() {
  common::utils::Circuit<Field> circ;
  auto in1 = circ.newInputWire();
  auto in2 = circ.newInputWire();
  auto out = circ.addGate(GateType::kMul, in1, in2);
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

std::vector<Field> buildEncodedInputs(size_t batch_size, int64_t scale, int64_t base,
                                      int64_t stride) {
  std::vector<Field> values;
  values.reserve(batch_size);
  for (size_t i = 0; i < batch_size; ++i) {
    const int64_t sign = (i % 2 == 0) ? 1 : -1;
    values.emplace_back(sign * (base + static_cast<int64_t>(i) * stride) * scale);
  }
  return values;
}

std::vector<Field> expectedProducts(const std::vector<Field>& x, const std::vector<Field>& y,
                                    int64_t scale) {
  std::vector<Field> expected;
  expected.reserve(x.size());
  const Field scale_field(scale);
  const Field inv_scale = inv(scale_field);
  for (size_t i = 0; i < x.size(); ++i) {
    expected.push_back(x[i] * y[i] * inv_scale);
  }
  return expected;
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

void checkOpenedOutputs(const std::vector<Field>& opened, const std::vector<Field>& expected) {
  if (opened.size() != expected.size()) {
    throw std::runtime_error("opened output size mismatch");
  }
  for (size_t i = 0; i < opened.size(); ++i) {
    if (opened[i] != expected[i]) {
      throw std::runtime_error("fixed-point output mismatch at index " + std::to_string(i));
    }
  }
}

}  // namespace

void benchmark(const bpo::variables_map& opts) {
  const auto nP = opts["num-parties"].as<size_t>();
  const auto pid = opts["pid"].as<size_t>();
  const auto seed = opts["seed"].as<size_t>();
  const auto repeat = opts["repeat"].as<size_t>();
  const auto port = opts["port"].as<int>();
  const auto batch_size = opts["batch-size"].as<size_t>();
  const auto scalar_call_count = opts["scalar-call-count"].as<size_t>();
  const auto frac_bits = opts["frac-bits"].as<size_t>();
  const auto ell_x = opts["ell-x"].as<size_t>();
  const auto slack = opts["slack"].as<size_t>();
  const bool scalar_only = opts["scalar-only"].as<bool>();
  const auto security_model_str = opts["security-model"].as<std::string>();

  asterisk2::SecurityModel security_model = asterisk2::SecurityModel::kSemiHonest;
  if (security_model_str == "malicious") {
    security_model = asterisk2::SecurityModel::kMalicious;
  } else if (security_model_str != "semi-honest") {
    throw std::runtime_error("Unsupported security-model, expected semi-honest or malicious");
  }

  if (!opts["localhost"].as<bool>()) {
    throw std::runtime_error("Asterisk2.0 fixed-point check currently supports localhost only");
  }

  auto network = std::make_shared<io::NetIOMP>(pid, nP + 1, port, nullptr, true);
  auto mul_circ = buildSingleMulCircuit();

  asterisk2::ProtocolConfig cfg;
  cfg.security_model = security_model;

  const int64_t scale = (1LL << frac_bits);
  const auto clear_x = buildEncodedInputs(batch_size, scale, 2, 1);
  const auto clear_y = buildEncodedInputs(batch_size, scale, 5, 2);
  const auto expected = expectedProducts(clear_x, clear_y, scale);

  json output_data;
  output_data["details"] = {{"num-parties", nP},
                            {"pid", pid},
                            {"seed", seed},
                            {"repeat", repeat},
                            {"batch_size", batch_size},
                            {"scalar_call_count", scalar_call_count},
                            {"scalar_total_calls_per_benchmark", scalar_call_count * batch_size},
                            {"scalar_only", scalar_only},
                            {"frac_bits", frac_bits},
                            {"ell_x", ell_x},
                            {"slack", slack},
                            {"security_model", security_model_str}};
  output_data["benchmarks"] = json::array();

  for (size_t r = 0; r < repeat; ++r) {
    asterisk2::Protocol proto(static_cast<int>(nP), static_cast<int>(pid), network, mul_circ,
                              static_cast<int>(seed + 17 * r), cfg);

    std::vector<Field> x_shares(batch_size, Field(0));
    std::vector<Field> y_shares(batch_size, Field(0));
    std::vector<Field> delta_x_shares(batch_size, Field(0));
    std::vector<Field> delta_y_shares(batch_size, Field(0));

    if (security_model == asterisk2::SecurityModel::kSemiHonest) {
      if (pid == 0) {
        x_shares = clear_x;
        y_shares = clear_y;
      }
    } else {
      auto input_circ = buildInputCircuit(2 * batch_size);
      asterisk2::Protocol input_proto(static_cast<int>(nP), static_cast<int>(pid), network,
                                      input_circ.circ, static_cast<int>(seed + 17 * r), cfg);
      auto input_auth = input_proto.mul_offline();
      std::unordered_map<wire_t, Field> clear_inputs;
      for (size_t i = 0; i < batch_size; ++i) {
        clear_inputs[input_circ.inputs[i]] = clear_x[i];
        clear_inputs[input_circ.inputs[batch_size + i]] = clear_y[i];
      }
      auto auth_inputs = input_proto.maliciousInputShareForTesting(clear_inputs, input_auth);
      if (pid < nP) {
        for (size_t i = 0; i < batch_size; ++i) {
          x_shares[i] = auth_inputs.x_shares.at(input_circ.inputs[i]);
          delta_x_shares[i] = auth_inputs.delta_x_shares.at(input_circ.inputs[i]);
          y_shares[i] = auth_inputs.x_shares.at(input_circ.inputs[batch_size + i]);
          delta_y_shares[i] = auth_inputs.delta_x_shares.at(input_circ.inputs[batch_size + i]);
        }
      }
    }

    json offline_bench = zeroBench(nP + 1);
    json online_bench = zeroBench(nP + 1);
    std::vector<Field> opened;
    if (!scalar_only) {
      network->sync();
      StatsPoint offline_start(*network);
      asterisk2::FixedPointBatchMulOfflineData off_data;
      if (security_model == asterisk2::SecurityModel::kSemiHonest) {
        off_data =
            proto.fixed_point_batch_mul_offline_semi_honest(batch_size, ell_x, frac_bits, slack);
      } else {
        off_data =
            proto.fixed_point_batch_mul_offline_malicious(batch_size, ell_x, frac_bits, slack);
      }
      StatsPoint offline_end(*network);
      offline_bench = offline_end - offline_start;

      network->sync();
      StatsPoint online_start(*network);
      asterisk2::FixedPointBatchMulResult fp_out;
      if (security_model == asterisk2::SecurityModel::kSemiHonest) {
        fp_out = proto.fixed_point_batch_mul_online_semi_honest(x_shares, y_shares, off_data);
      } else {
        fp_out = proto.fixed_point_batch_mul_online_malicious(
            x_shares, delta_x_shares, y_shares, delta_y_shares, off_data);
      }
      StatsPoint online_end(*network);
      online_bench = online_end - online_start;

      network->sync();
      opened = openSharesToParty0(network, nP, pid, fp_out.shares);
      if (pid == 0) {
        checkOpenedOutputs(opened, expected);
      }
      network->sync();
    }

    json scalar_offline_bench = zeroBench(nP + 1);
    json scalar_online_bench = zeroBench(nP + 1);
    const size_t total_scalar_calls = scalar_call_count * batch_size;
    std::vector<Field> scalar_output_shares;
    std::vector<Field> scalar_opened_outputs;
    std::vector<Field> scalar_expected_outputs;
    if (pid < nP) {
      scalar_output_shares.reserve(total_scalar_calls);
    }
    if (pid == 0) {
      scalar_opened_outputs.reserve(total_scalar_calls);
      scalar_expected_outputs.reserve(total_scalar_calls);
    }
    asterisk2::Protocol scalar_proto(static_cast<int>(nP), static_cast<int>(pid), network, mul_circ,
                                     static_cast<int>(seed + 17 * r), cfg);
    std::vector<asterisk2::FixedPointMulOfflineData> scalar_offline_data;
    scalar_offline_data.reserve(total_scalar_calls);

    network->sync();
    StatsPoint scalar_offline_start(*network);
    for (size_t call_idx = 0; call_idx < scalar_call_count; ++call_idx) {
      for (size_t i = 0; i < batch_size; ++i) {
        if (security_model == asterisk2::SecurityModel::kSemiHonest) {
          scalar_offline_data.push_back(
              scalar_proto.fixed_point_mul_offline_semi_honest(ell_x, frac_bits, slack));
        } else {
          scalar_offline_data.push_back(
              scalar_proto.fixed_point_mul_offline_malicious(ell_x, frac_bits, slack));
        }
      }
    }
    StatsPoint scalar_offline_end(*network);
    addBench(scalar_offline_bench, scalar_offline_end - scalar_offline_start);

    network->sync();
    StatsPoint scalar_online_start(*network);
    size_t scalar_flat_idx = 0;
    for (size_t call_idx = 0; call_idx < scalar_call_count; ++call_idx) {
      for (size_t i = 0; i < batch_size; ++i, ++scalar_flat_idx) {
        asterisk2::FixedPointMulResult scalar_out;
        if (security_model == asterisk2::SecurityModel::kSemiHonest) {
          scalar_out = scalar_proto.fixed_point_mul_online_semi_honest(
              x_shares[i], y_shares[i], scalar_offline_data[scalar_flat_idx]);
        } else {
          scalar_out = scalar_proto.fixed_point_mul_online_malicious(
              x_shares[i], delta_x_shares[i], y_shares[i], delta_y_shares[i],
              scalar_offline_data[scalar_flat_idx]);
        }
        if (pid < nP) {
          scalar_output_shares.push_back(scalar_out.share);
        }
      }
    }
    StatsPoint scalar_online_end(*network);
    addBench(scalar_online_bench, scalar_online_end - scalar_online_start);

    network->sync();
    const auto opened_scalar = openSharesToParty0(network, nP, pid, scalar_output_shares);
    if (pid == 0) {
      if (opened_scalar.size() != total_scalar_calls) {
        throw std::runtime_error("scalar fixed-point opened output size mismatch");
      }
      for (size_t call_idx = 0; call_idx < scalar_call_count; ++call_idx) {
        for (size_t i = 0; i < batch_size; ++i) {
          const size_t flat_idx = call_idx * batch_size + i;
          if (opened_scalar[flat_idx] != expected[i]) {
            throw std::runtime_error("scalar fixed-point output mismatch at flat index " +
                                     std::to_string(flat_idx));
          }
          scalar_opened_outputs.push_back(opened_scalar[flat_idx]);
          scalar_expected_outputs.push_back(expected[i]);
        }
      }
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
                {"scalar_offline", scalar_offline_bench},
                {"scalar_online", scalar_online_bench},
                {"passed", true}};
    if (pid == 0) {
      json opened_json = json::array();
      json expected_json = json::array();
      json scalar_opened_json = json::array();
      for (size_t i = 0; i < opened.size(); ++i) {
        opened_json.push_back(NTL::conv<uint64_t>(NTL::rep(opened[i])));
        expected_json.push_back(NTL::conv<uint64_t>(NTL::rep(expected[i])));
      }
      for (size_t i = 0; i < scalar_opened_outputs.size(); ++i) {
        scalar_opened_json.push_back(NTL::conv<uint64_t>(NTL::rep(scalar_opened_outputs[i])));
      }
      row["opened_outputs"] = std::move(opened_json);
      row["expected_outputs"] = std::move(expected_json);
      row["scalar_opened_outputs"] = std::move(scalar_opened_json);
      json scalar_expected_json = json::array();
      for (const auto& val : scalar_expected_outputs) {
        scalar_expected_json.push_back(NTL::conv<uint64_t>(NTL::rep(val)));
      }
      row["scalar_expected_outputs"] = std::move(scalar_expected_json);
    }
    output_data["benchmarks"].push_back(std::move(row));
  }

  if (opts.count("output") != 0) {
    saveJson(output_data, opts["output"].as<std::string>());
  }
}

bpo::options_description programOptions() {
  bpo::options_description desc("Asterisk2.0 fixed-point multiplication check options");
  desc.add_options()
      ("num-parties,n", bpo::value<size_t>()->required(), "Number of computing parties.")
      ("pid,p", bpo::value<size_t>()->required(), "Party ID.")
      ("seed", bpo::value<size_t>()->default_value(200), "Value of the random seed.")
      ("repeat,r", bpo::value<size_t>()->default_value(1), "Number of repetitions.")
      ("batch-size", bpo::value<size_t>()->default_value(4),
       "Number of fixed-point multiplications in one batch.")
      ("scalar-call-count", bpo::value<size_t>()->default_value(1),
       "Number of sequential scalar fixed-point calls per benchmark repetition.")
      ("scalar-only", bpo::bool_switch(), "Measure only the scalar fixed-point APIs.")
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
  bpo::options_description cmdline("Benchmark Asterisk2.0 fixed-point multiplication check.");
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
