#include "protocol.h"

#include <chrono>
#include <random>
#include <stdexcept>
#include <thread>

namespace asterisk2 {

namespace {
Field randomField(std::mt19937_64& rng) {
  return NTL::conv<Field>(NTL::to_ZZ(static_cast<unsigned long>(rng())));
}

std::mt19937_64 partyHelperRng(int seed, int party_id, size_t gate_idx) {
  return std::mt19937_64(static_cast<uint64_t>(seed) * 1000003ULL +
                         static_cast<uint64_t>(party_id) * 1315423911ULL +
                         gate_idx);
}

std::mt19937_64 helperTripleRng(int seed, size_t gate_idx) {
  return std::mt19937_64(static_cast<uint64_t>(seed) * 7919ULL + gate_idx);
}
}  // namespace

Protocol::Protocol(int nP, int id, std::shared_ptr<io::NetIOMP> network,
                   LevelOrderedCircuit circ, int seed, ProtocolConfig config)
    : nP_(nP),
      id_(id),
      helper_id_(nP),
      seed_(seed),
      config_(config),
      network_(std::move(network)),
      circ_(std::move(circ)),
      wire_share_(circ_.num_gates, Field(0)) {}

std::vector<TripleShare> Protocol::offline() {
  if (config_.security_model == SecurityModel::kMalicious) {
    throw std::runtime_error(
        "Asterisk2.0 malicious model is not implemented yet; use semi-honest mode");
  }

  if (nP_ < 2) {
    throw std::runtime_error("Asterisk2.0 requires at least 2 computing parties");
  }

  std::vector<FIn2Gate> mul_gates;
  for (const auto& level : circ_.gates_by_level) {
    for (const auto& gate : level) {
      if (gate->type == common::utils::GateType::kMul) {
        mul_gates.push_back(*std::dynamic_pointer_cast<FIn2Gate>(gate));
      }
    }
  }

  std::vector<TripleShare> triples(mul_gates.size());

  for (size_t g = 0; g < mul_gates.size(); ++g) {
    if (id_ <= nP_ - 2) {
      auto rng = partyHelperRng(seed_, id_, g);
      triples[g].a = randomField(rng);
      triples[g].b = randomField(rng);
      triples[g].c = randomField(rng);
    }
  }

  if (id_ == helper_id_) {
    for (size_t g = 0; g < mul_gates.size(); ++g) {
      auto rg = helperTripleRng(seed_, g);
      Field a = randomField(rg);
      Field b = randomField(rg);
      Field c = a * b;

      Field sum_a = Field(0);
      Field sum_b = Field(0);
      Field sum_c = Field(0);
      for (int i = 0; i <= nP_ - 2; ++i) {
        auto rng = partyHelperRng(seed_, i, g);
        auto ai = randomField(rng);
        auto bi = randomField(rng);
        auto ci = randomField(rng);
        sum_a += ai;
        sum_b += bi;
        sum_c += ci;
      }

      Field an = a - sum_a;
      Field bn = b - sum_b;
      Field cn = c - sum_c;
      Field pack[3] = {an, bn, cn};
      maybeSimulateLatency();
      maybeSimulateBandwidth(3 * common::utils::FIELDSIZE);
      network_->send(nP_ - 1, pack, 3);
    }
    network_->flush();
    return triples;
  }

  if (id_ >= helper_id_) {
    return triples;
  }

  if (id_ == nP_ - 1) {
    for (size_t g = 0; g < mul_gates.size(); ++g) {
      Field pack[3];
      maybeSimulateLatency();
      network_->recv(helper_id_, pack, 3);
      triples[g].a = pack[0];
      triples[g].b = pack[1];
      triples[g].c = pack[2];
    }
  }

  return triples;
}

std::vector<Protocol::OpenPair> Protocol::openPairsToComputingParties(
    const std::vector<OpenPair>& local_pairs) const {
  if (id_ >= helper_id_) {
    return {};
  }

  const size_t gates = local_pairs.size();
  std::vector<Field> send_buf(gates * 2);
  for (size_t i = 0; i < gates; ++i) {
    send_buf[2 * i] = local_pairs[i].d;
    send_buf[2 * i + 1] = local_pairs[i].e;
  }

  for (int p = 0; p < helper_id_; ++p) {
    if (p != id_) {
      maybeSimulateLatency();
      maybeSimulateBandwidth(send_buf.size() * common::utils::FIELDSIZE);
      network_->send(p, send_buf.data(), send_buf.size());
    }
  }
  network_->flush();

  std::vector<OpenPair> sums = local_pairs;
  std::vector<Field> recv_buf(gates * 2);
  for (int p = 0; p < helper_id_; ++p) {
    if (p == id_) {
      continue;
    }
    maybeSimulateLatency();
    network_->recv(p, recv_buf.data(), recv_buf.size());
    for (size_t i = 0; i < gates; ++i) {
      sums[i].d += recv_buf[2 * i];
      sums[i].e += recv_buf[2 * i + 1];
    }
  }

  return sums;
}

void Protocol::maybeSimulateLatency() const {
  if (config_.sim_latency_ms > 0) {
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(
        config_.sim_latency_ms));
  }
}

void Protocol::maybeSimulateBandwidth(size_t bytes) const {
  if (config_.sim_bandwidth_mbps <= 0 || bytes == 0) {
    return;
  }
  // seconds = bits / (megabits_per_sec * 1e6)
  const double bits = static_cast<double>(bytes) * 8.0;
  const double seconds = bits / (config_.sim_bandwidth_mbps * 1e6);
  if (seconds > 0) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  }
}

std::vector<Field> Protocol::online(
    const std::unordered_map<wire_t, Field>& inputs,
    const std::vector<TripleShare>& triples) {
  if (config_.security_model == SecurityModel::kMalicious) {
    throw std::runtime_error(
        "Asterisk2.0 malicious model is not implemented yet; use semi-honest mode");
  }

  if (id_ >= helper_id_) {
    return {};
  }

  size_t mul_idx = 0;
  for (const auto& level : circ_.gates_by_level) {
    std::vector<const FIn2Gate*> mul_gates;
    mul_gates.reserve(level.size());

    for (const auto& gate : level) {
      switch (gate->type) {
        case common::utils::GateType::kInp: {
          auto it = inputs.find(gate->out);
          wire_share_[gate->out] = (it == inputs.end()) ? Field(0) : it->second;
          break;
        }
        case common::utils::GateType::kAdd: {
          auto* g = static_cast<FIn2Gate*>(gate.get());
          wire_share_[g->out] = wire_share_[g->in1] + wire_share_[g->in2];
          break;
        }
        case common::utils::GateType::kSub: {
          auto* g = static_cast<FIn2Gate*>(gate.get());
          wire_share_[g->out] = wire_share_[g->in1] - wire_share_[g->in2];
          break;
        }
        case common::utils::GateType::kMul: {
          auto* g = static_cast<FIn2Gate*>(gate.get());
          mul_gates.push_back(g);
          break;
        }
        default:
          throw std::runtime_error("Asterisk2.0 benchmark currently supports Inp/Add/Sub/Mul only");
      }
    }

    if (!mul_gates.empty()) {
      std::vector<OpenPair> local_pairs(mul_gates.size());
      for (size_t i = 0; i < mul_gates.size(); ++i) {
        if (mul_idx + i >= triples.size()) {
          throw std::runtime_error("Insufficient Beaver triples in online phase");
        }
        const auto* g = mul_gates[i];
        const auto& t = triples[mul_idx + i];
        local_pairs[i].d = wire_share_[g->in1] - t.a;
        local_pairs[i].e = wire_share_[g->in2] - t.b;
      }

      auto opened = openPairsToComputingParties(local_pairs);
      for (size_t i = 0; i < mul_gates.size(); ++i) {
        const auto* g = mul_gates[i];
        const auto& t = triples[mul_idx + i];
        Field out = opened[i].e * t.a + opened[i].d * t.b + t.c;
        if (id_ == 0) {
          out += opened[i].d * opened[i].e;
        }
        wire_share_[g->out] = out;
      }
      mul_idx += mul_gates.size();
    }
  }

  std::vector<Field> outputs;
  outputs.reserve(circ_.outputs.size());
  for (auto wid : circ_.outputs) {
    outputs.push_back(wire_share_[wid]);
  }
  return outputs;
}

}  // namespace asterisk2
