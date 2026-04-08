#include "protocol.h"

#include <chrono>
#include <cstdint>
#include <limits>
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

std::mt19937_64 helperTruncRng(int seed, size_t idx) {
  return std::mt19937_64(static_cast<uint64_t>(seed) * 104729ULL + idx);
}

uint64_t pow2Bound(size_t bits) {
  if (bits >= 64) {
    throw std::runtime_error("pow2Bound supports bit lengths < 64");
  }
  return (1ULL << bits);
}

Field randomFieldBounded(std::mt19937_64& rng, uint64_t bound) {
  if (bound == 0) {
    return Field(0);
  }
  return NTL::conv<Field>(NTL::to_ZZ(static_cast<unsigned long>(rng() % bound)));
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
      constexpr size_t kTripleElements = 3;
      maybeSimulateStep(kTripleElements * common::utils::FIELDSIZE);
      network_->send(nP_ - 1, pack, kTripleElements * common::utils::FIELDSIZE);
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
      constexpr size_t kTripleElements = 3;
      maybeSimulateStep(kTripleElements * common::utils::FIELDSIZE);
      network_->recv(helper_id_, pack, kTripleElements * common::utils::FIELDSIZE);
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

  const size_t peers = static_cast<size_t>(helper_id_ - 1);
  maybeSimulateStep(send_buf.size() * common::utils::FIELDSIZE * peers);
  const bool use_parallel_io = config_.parallel_send && peers >= 3 && gates >= 64;
  if (use_parallel_io) {
    const size_t serialized_bytes = send_buf.size() * common::utils::FIELDSIZE;
    std::vector<uint8_t> send_serialized(serialized_bytes);
    for (size_t i = 0; i < send_buf.size(); ++i) {
      NTL::BytesFromZZ(send_serialized.data() + i * common::utils::FIELDSIZE,
                       NTL::conv<NTL::ZZ>(send_buf[i]), common::utils::FIELDSIZE);
    }

    std::vector<int> peer_ids;
    peer_ids.reserve(peers);
    for (int p = 0; p < helper_id_; ++p) {
      if (p != id_) {
        peer_ids.push_back(p);
      }
    }

    std::vector<std::thread> send_threads;
    send_threads.reserve(peer_ids.size());
    for (int peer : peer_ids) {
      send_threads.emplace_back([&, peer]() {
        auto* channel = network_->getSendChannel(peer);
        channel->send_data(send_serialized.data(), send_serialized.size());
        channel->flush();
      });
    }
    for (auto& th : send_threads) {
      th.join();
    }

    std::vector<OpenPair> sums = local_pairs;
    maybeSimulateStep(send_buf.size() * common::utils::FIELDSIZE * peers);

    std::vector<std::vector<uint8_t>> recv_serialized(
        peer_ids.size(), std::vector<uint8_t>(serialized_bytes));
    std::vector<std::thread> recv_threads;
    recv_threads.reserve(peer_ids.size());
    for (size_t idx = 0; idx < peer_ids.size(); ++idx) {
      recv_threads.emplace_back([&, idx]() {
        auto* channel = network_->getRecvChannel(peer_ids[idx]);
        channel->recv_data(recv_serialized[idx].data(), recv_serialized[idx].size());
      });
    }
    for (auto& th : recv_threads) {
      th.join();
    }

    for (const auto& peer_buf : recv_serialized) {
      for (size_t i = 0; i < gates; ++i) {
        const auto d = NTL::ZZFromBytes(peer_buf.data() + (2 * i) * common::utils::FIELDSIZE,
                                        common::utils::FIELDSIZE);
        const auto e = NTL::ZZFromBytes(peer_buf.data() + (2 * i + 1) * common::utils::FIELDSIZE,
                                        common::utils::FIELDSIZE);
        sums[i].d += NTL::conv<Field>(d);
        sums[i].e += NTL::conv<Field>(e);
      }
    }

    return sums;
  } else {
    for (int p = 0; p < helper_id_; ++p) {
      if (p != id_) {
        network_->send(p, send_buf.data(), send_buf.size() * common::utils::FIELDSIZE);
      }
    }
    network_->flush();

    std::vector<OpenPair> sums = local_pairs;
    std::vector<Field> recv_buf(gates * 2);
    maybeSimulateStep(recv_buf.size() * common::utils::FIELDSIZE * peers);
    for (int p = 0; p < helper_id_; ++p) {
      if (p == id_) {
        continue;
      }
      network_->recv(p, recv_buf.data(), recv_buf.size() * common::utils::FIELDSIZE);
      for (size_t i = 0; i < gates; ++i) {
        sums[i].d += recv_buf[2 * i];
        sums[i].e += recv_buf[2 * i + 1];
      }
    }

    return sums;
  }
}

Field Protocol::openToComputingParties(const Field& local_share) const {
  if (id_ >= helper_id_) {
    return Field(0);
  }

  maybeSimulateStep(common::utils::FIELDSIZE * static_cast<size_t>(helper_id_ - 1));
  for (int p = 0; p < helper_id_; ++p) {
    if (p != id_) {
      network_->send(p, &local_share, common::utils::FIELDSIZE);
    }
  }
  network_->flush();

  Field opened = local_share;
  Field recv_val = Field(0);
  maybeSimulateStep(common::utils::FIELDSIZE * static_cast<size_t>(helper_id_ - 1));
  for (int p = 0; p < helper_id_; ++p) {
    if (p == id_) {
      continue;
    }
    network_->recv(p, &recv_val, common::utils::FIELDSIZE);
    opened += recv_val;
  }
  return opened;
}

void Protocol::maybeSimulateStep(size_t aggregate_bytes) const {
  maybeSimulateLatency();
  maybeSimulateBandwidth(aggregate_bytes);
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

std::vector<Field> Protocol::probabilisticTruncate(
    const std::vector<Field>& x_shares, size_t ell_x, size_t m, size_t s) {
  if (id_ > helper_id_) {
    return {};
  }
  if (m == 0 || m >= ell_x) {
    throw std::runtime_error("probabilisticTruncate requires 0 < m < ell_x");
  }
  if (ell_x + s + 1 >= 64) {
    throw std::runtime_error("probabilisticTruncate requires ell_x + s + 1 < 64");
  }
  const uint64_t bound_r = pow2Bound(ell_x - m + s);
  const uint64_t bound_r0 = pow2Bound(m);
  const uint64_t two_pow_m = pow2Bound(m);
  const uint64_t two_pow_lx_minus_1 = pow2Bound(ell_x - 1);
  const Field lambda_m = inv(NTL::conv<Field>(NTL::to_ZZ(two_pow_m)));

  std::vector<Field> out(x_shares.size(), Field(0));
  if (id_ == helper_id_) {
    for (size_t idx = 0; idx < x_shares.size(); ++idx) {
      auto hrg = helperTruncRng(seed_, idx);
      Field r = randomFieldBounded(hrg, bound_r);
      Field r0 = randomFieldBounded(hrg, bound_r0);
      Field sum_r = Field(0);
      Field sum_r0 = Field(0);
      for (int i = 0; i <= nP_ - 2; ++i) {
        auto prg = partyHelperRng(seed_ + 17, i, idx);
        auto ri = randomFieldBounded(prg, bound_r);
        auto r0i = randomFieldBounded(prg, bound_r0);
        sum_r += ri;
        sum_r0 += r0i;
      }
      Field pack[2] = {r - sum_r, r0 - sum_r0};
      maybeSimulateStep(2 * common::utils::FIELDSIZE);
      network_->send(nP_ - 1, pack, 2 * common::utils::FIELDSIZE);
    }
    network_->flush();
    return out;
  }

  std::vector<Field> r_share(x_shares.size(), Field(0));
  std::vector<Field> r0_share(x_shares.size(), Field(0));
  if (id_ <= nP_ - 2) {
    for (size_t idx = 0; idx < x_shares.size(); ++idx) {
      auto prg = partyHelperRng(seed_ + 17, id_, idx);
      r_share[idx] = randomFieldBounded(prg, bound_r);
      r0_share[idx] = randomFieldBounded(prg, bound_r0);
    }
  } else if (id_ == nP_ - 1) {
    for (size_t idx = 0; idx < x_shares.size(); ++idx) {
      Field pack[2];
      maybeSimulateStep(2 * common::utils::FIELDSIZE);
      network_->recv(helper_id_, pack, 2 * common::utils::FIELDSIZE);
      r_share[idx] = pack[0];
      r0_share[idx] = pack[1];
    }
  }

  for (size_t idx = 0; idx < x_shares.size(); ++idx) {
    Field z_i = x_shares[idx] + ((id_ == 0) ? NTL::conv<Field>(NTL::to_ZZ(two_pow_lx_minus_1))
                                            : Field(0));
    Field c_i = z_i + NTL::conv<Field>(NTL::to_ZZ(two_pow_m)) * r_share[idx] + r0_share[idx];
    Field c = openToComputingParties(c_i);
    uint64_t c_u64 = NTL::conv<uint64_t>(NTL::rep(c));
    uint64_t c0 = c_u64 & (two_pow_m - 1);
    Field d_i = ((id_ == 0) ? NTL::conv<Field>(NTL::to_ZZ(c0)) : Field(0)) - r0_share[idx];
    out[idx] = lambda_m * (x_shares[idx] - d_i);
  }
  return out;
}

}  // namespace asterisk2
