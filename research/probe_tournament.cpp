#include "research/probe_tournament.h"

#include <cmath>
#include <set>

namespace aafr::research {
namespace {

double Mean(const std::vector<double>& v) {
  double s = 0.0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

std::optional<double> SampleVariance(const std::vector<double>& v) {
  if (v.size() < 2) return std::nullopt;
  const double m = Mean(v);
  double ss = 0.0;
  for (double x : v) ss += (x - m) * (x - m);
  return ss / static_cast<double>(v.size() - 1);
}

}  // namespace

std::vector<std::string> ValidateTournament(const ProbeTournament& t) {
  std::vector<std::string> issues;
  std::size_t incumbents = 0;
  std::set<std::string> ids;
  std::set<std::vector<std::string>> paths;
  for (const ProbeArm& arm : t.arms) {
    const std::string who = "arm '" + arm.arm_id + "'";
    if (arm.arm_id.empty()) issues.push_back("an arm has no id");
    if (!ids.insert(arm.arm_id).second) issues.push_back(who + ": duplicate arm id");
    if (arm.incumbent) ++incumbents;
    if (arm.path.size() < 2) issues.push_back(who + ": path has fewer than two nodes");
    else if (!paths.insert(arm.path).second) issues.push_back(who + ": path duplicates another arm (identical paths are one arm)");
    if (arm.frames.size() != kFrames) {
      issues.push_back(who + ": " + std::to_string(arm.frames.size()) + " frames, expected " + std::to_string(kFrames));
      continue;
    }
    for (std::size_t f = 0; f < arm.frames.size(); ++f) {
      const ProbeFrame& frame = arm.frames[f];
      if (frame.round != f) issues.push_back(who + ": frame " + std::to_string(f) + " is not in round " + std::to_string(f));
      if (frame.position_in_round >= t.arms.size()) issues.push_back(who + ": position in round out of range");
      if (frame.blocks.size() != kBlocks) {
        issues.push_back(who + ": frame " + std::to_string(f) + " has " + std::to_string(frame.blocks.size()) + " blocks");
        continue;
      }
      for (const ProbeBlock& block : frame.blocks) {
        if (block.echoes.size() != kEchoes) {
          issues.push_back(who + ": a block of frame " + std::to_string(f) + " has " +
                           std::to_string(block.echoes.size()) + " echoes");
          continue;
        }
        for (const Echo& echo : block.echoes) {
          if (echo && !(std::isfinite(*echo) && *echo > 0.0)) {
            issues.push_back(who + ": a returned RTT is not finite and positive");
          }
        }
      }
    }
  }
  if (incumbents != 1) issues.push_back("exactly one incumbent arm is required, found " + std::to_string(incumbents));
  if (t.arms.size() < 2) issues.push_back("at least one challenger arm is required");
  // Each round must order the arms as a permutation 0 .. arms - 1.
  bool shapes_ok = true;
  for (const ProbeArm& arm : t.arms) shapes_ok = shapes_ok && arm.frames.size() == kFrames;
  if (shapes_ok) {
    for (std::size_t r = 0; r < kFrames; ++r) {
      std::set<std::size_t> positions;
      for (const ProbeArm& arm : t.arms) positions.insert(arm.frames[r].position_in_round);
      if (positions.size() != t.arms.size()) issues.push_back("round " + std::to_string(r) + " does not order every arm exactly once");
    }
  }
  return issues;
}

ArmSummary SummarizeArm(const ProbeArm& arm) {
  ArmSummary s;
  s.arm_id = arm.arm_id;
  s.incumbent = arm.incumbent;
  double rtt_sum = 0.0;
  s.all_frames_complete = true;
  for (const ProbeFrame& frame : arm.frames) {
    FrameSummary fs;
    fs.round = frame.round;
    std::vector<double> block_means;
    for (const ProbeBlock& block : frame.blocks) {
      BlockSummary bs;
      std::vector<double> x;
      for (const Echo& echo : block.echoes) {
        ++s.sent;
        if (!echo) continue;
        ++s.returned;
        rtt_sum += *echo;
        x.push_back(std::log10(*echo));
      }
      bs.replies = x.size();
      bs.valid = bs.replies >= kMinReplies;
      if (bs.valid) {
        bs.mean_log10 = Mean(x);
        bs.variance_log10 = SampleVariance(x);
        block_means.push_back(*bs.mean_log10);
        ++fs.valid_blocks;
      }
      fs.blocks.push_back(bs);
    }
    fs.complete = fs.valid_blocks == kBlocks;
    if (fs.complete) {
      fs.value_log10 = Mean(block_means);
      fs.block_mean_variance = SampleVariance(block_means);
    }
    s.all_frames_complete = s.all_frames_complete && fs.complete;
    s.frames.push_back(fs);
  }
  s.lost = s.sent - s.returned;
  if (s.sent > 0) {
    s.delivery_ratio = static_cast<double>(s.returned) / static_cast<double>(s.sent);
    s.loss_ratio = 1.0 - s.delivery_ratio;
  }
  if (s.returned > 0) s.mean_rtt_ms = rtt_sum / static_cast<double>(s.returned);
  return s;
}

}  // namespace aafr::research
