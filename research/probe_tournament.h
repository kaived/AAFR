#pragma once

// Probe tournament data model and its per-arm summaries. Plain C++17, pure
// functions, no probing and no I/O. Not used by the live runtime.
//
// Every arm (the incumbent path and each distinct challenger path) receives
// kFrames frames; each frame holds kBlocks blocks of kEchoes ICMP echoes, so
// kProbesPerArm probes. Frames are sent in interleaved rounds: round r holds
// frame r of every arm, in a rotated arm order, so frame r of the incumbent
// and frame r of a challenger form a pair. A lost echo is represented
// explicitly (no RTT) and is never imputed. A block is valid only with at
// least kMinReplies returned echoes.
//
// All analysis uses x = log10(RTT in ms).
//   block:  mean and variance (micro-variance, ddof 1) of x over returned echoes
//   frame:  value = mean of its valid block means; variance (ddof 1) of the
//           valid block means (defined with at least 2 valid blocks)
//
// Invalid blocks and frames follow the research engine (source commit
// a0ce0ed, anova.cpp frameStats): invalid blocks are discarded, and a frame
// is valid when at least kMinValidBlocksPerFrame of its blocks are valid. A
// frame without a valid block has no value; the arbitration then drops that
// round from the paired test (arbitration_reference.h).

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aafr::research {

inline constexpr std::size_t kFrames = 10;
inline constexpr std::size_t kBlocks = 5;
inline constexpr std::size_t kEchoes = 8;
inline constexpr std::size_t kMinReplies = 3;
inline constexpr std::size_t kProbesPerArm = kFrames * kBlocks * kEchoes;
inline constexpr std::size_t kMinValidBlocksPerFrame = 1;  // Research engine: a frame is usable with >= 1 valid block.

// One ICMP echo: the RTT when a reply returned; nullopt when it was lost.
using Echo = std::optional<double>;

struct ProbeBlock {
  std::vector<Echo> echoes;  // Exactly kEchoes.
};

struct ProbeFrame {
  std::size_t round{0};              // Interleaving round, 0 .. kFrames - 1.
  std::size_t position_in_round{0};  // Order of this arm within its round.
  std::vector<ProbeBlock> blocks;    // Exactly kBlocks.
};

struct ProbeArm {
  std::string arm_id;
  bool incumbent{false};
  std::vector<std::string> path;        // Nodes of the arm's path.
  std::vector<std::string> algorithms;  // Algorithms that returned this path.
  std::vector<ProbeFrame> frames;       // Exactly kFrames, frame r in round r.
};

struct ProbeTournament {
  std::vector<ProbeArm> arms;  // Exactly one incumbent and at least one challenger.
};

// Problems with the tournament's structure; empty when it can be analysed.
// Checks dimensions, one incumbent, at least one challenger, distinct arm
// ids, distinct paths (identical paths are one arm), round numbering, that
// every round orders the arms as a permutation, and that every returned RTT
// is finite and positive.
std::vector<std::string> ValidateTournament(const ProbeTournament& tournament);

struct BlockSummary {
  std::size_t replies{0};
  bool valid{false};
  std::optional<double> mean_log10;
  std::optional<double> variance_log10;  // Micro-variance, ddof 1.
};

struct FrameSummary {
  std::size_t round{0};
  std::vector<BlockSummary> blocks;
  std::size_t valid_blocks{0};
  bool complete{false};                         // All kBlocks blocks valid.
  bool valid{false};                            // At least kMinValidBlocksPerFrame valid blocks.
  std::optional<double> value_log10;            // Mean of valid block means; valid frames only.
  std::optional<double> block_mean_variance;    // Variance of valid block means, ddof 1; >= 2 valid blocks.
};

struct ArmSummary {
  std::string arm_id;
  bool incumbent{false};
  std::vector<FrameSummary> frames;
  std::size_t sent{0};
  std::size_t returned{0};
  std::size_t lost{0};
  double delivery_ratio{0.0};
  double loss_ratio{1.0};
  std::optional<double> mean_rtt_ms;  // Arithmetic mean of returned RTTs.
  std::size_t valid_frames{0};
  bool all_frames_complete{false};
};

// Summary of one arm; call only on a tournament that passed validation.
ArmSummary SummarizeArm(const ProbeArm& arm);

}  // namespace aafr::research
