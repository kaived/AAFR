#pragma once

// Reference values of the research method: the six-switch testbed, the path
// scores, the algorithm parameters, the probe tournament, the decision rule
// and the recorded tournaments of the two testbed scenarios. Test-only data:
// no product code may include this header. It freezes the specified values so
// later phases can be checked against them; it implements nothing.
//
// Where the method leaves a value open it is absent here rather than guessed
// (see the "Research Alignment" section of the root README).

#include <array>
#include <cstddef>

namespace aafr::research_fixture {

// ---- Six-switch testbed topology (provisioned, bidirectional) ----
struct TestbedLink {
  const char* a;
  const char* b;
  double capacity_mbps;
  double delay_ms;  // One-way, as configured with netem.
};

inline constexpr std::array<TestbedLink, 9> kTestbedLinks = {{
    {"s1", "s2", 10.0, 2.0},
    {"s2", "s6", 10.0, 2.0},
    {"s1", "s3", 100.0, 5.0},
    {"s3", "s6", 100.0, 5.0},
    {"s1", "s4", 100.0, 7.0},
    {"s4", "s5", 100.0, 7.0},
    {"s5", "s6", 100.0, 7.0},
    {"s2", "s3", 50.0, 3.0},
    {"s3", "s5", 50.0, 4.0},
}};
// Sender h1 hangs off s1, receiver h2 off s6, h3 off s3.
inline constexpr const char* kSenderSwitch = "s1";
inline constexpr const char* kReceiverSwitch = "s6";
// Dijkstra on start-up delays picks s1-s2-s6 (4 ms one-way); the detour
// s1-s3-s6 is 10 ms one-way and 100 Mb/s wide.
inline constexpr double kDefaultPathDelayMs = 4.0;
inline constexpr double kDetourPathDelayMs = 10.0;

// ---- Independent quality score J (lower is better) ----
// J(P) = w_h H + w_b / B + w_d D + w_l L, (w_h, w_b, w_d, w_l) = (0.5, 100, 1, 100).
// Used only as an independent check of tested paths, never by the engine.
inline constexpr double kJHopWeight = 0.5;
inline constexpr double kJBandwidthWeight = 100.0;  // Multiplies 1 / B.
inline constexpr double kJDelayWeight = 1.0;
inline constexpr double kJLossWeight = 100.0;
// Worked check: 5 hops, bottleneck 598, delay 14, no loss.
inline constexpr double kJCheckHops = 5.0;
inline constexpr double kJCheckBottleneck = 598.0;
inline constexpr double kJCheckDelay = 14.0;
inline constexpr double kJCheckLoss = 0.0;
inline constexpr double kJCheckValue = 16.667;  // Recorded to 3 decimals.

// ---- Engine cost J_eng minimized by ACO and GA ----
// J_eng(P) = W_d z(D) + W_l z(L) + W_b z(1/B) + W_h z(H), z with training-set
// statistics.
inline constexpr double kJengDelayWeight = 1.0;
inline constexpr double kJengLossWeight = 1.0;
inline constexpr double kJengInverseBandwidthWeight = 1.0;
inline constexpr double kJengHopWeight = 0.3;

// ---- Goodness G of an arm from measured probe results ----
// G = W_hops z(H) + W_bw z(B) + W_lat z(RTT) + W_tp z(T); T = B * delivery ratio.
inline constexpr double kGHopWeight = -0.5;
inline constexpr double kGBandwidthWeight = 1.0;
inline constexpr double kGLatencyWeight = -1.0;
inline constexpr double kGThroughputWeight = 1.0;

// ---- Algorithm pool parameters ----
inline constexpr std::size_t kAcoAnts = 24;
inline constexpr std::size_t kAcoIterations = 30;
inline constexpr std::size_t kGaPopulation = 30;
inline constexpr std::size_t kGaGenerations = 40;

// ---- Triggers and engine timing ----
inline constexpr double kTriggerLinkLoss = 0.03;          // Above 3 %.
inline constexpr double kTriggerLinkUtilization = 0.85;   // Above 85 %.
inline constexpr double kTriggerRttFactor = 2.0;          // Above twice its reference.
inline constexpr double kTriggerPeriodicS = 90.0;
inline constexpr double kTriggerCooldownS = 15.0;
inline constexpr double kTelemetryIntervalS = 2.0;
inline constexpr std::size_t kTelemetryWarmupSweeps = 5;

// ---- Probe tournament ----
inline constexpr std::size_t kFramesPerArm = 10;
inline constexpr std::size_t kBlocksPerFrame = 5;
inline constexpr std::size_t kEchoesPerBlock = 8;
inline constexpr std::size_t kMinRepliesPerBlock = 3;
inline constexpr std::size_t kProbesPerArm = kFramesPerArm * kBlocksPerFrame * kEchoesPerBlock;  // 400

// ---- Decision rule (reference version) ----
inline constexpr double kWeightEta = 0.40;
inline constexpr double kWeightCi = 0.35;
inline constexpr double kWeightGoodness = 0.25;
inline constexpr double kCiScale = 0.05;        // s_ci = min(1, |CI bound| / 0.05).
inline constexpr double kGoodnessScale = 1.0;   // g_scale: s_g = clip(dG / g_scale, -1, 1).
inline constexpr double kConfidenceThreshold = 0.70;  // C must exceed it.
inline constexpr double kHolmAlpha = 0.05;            // Holm-adjusted p must be below it.
inline constexpr double kDwellS = 30.0;
inline constexpr double kLossBudget = 0.03;       // Round-trip loss.
inline constexpr double kMeanRttBudgetMs = 150.0;
inline constexpr double kWilsonLevel = 0.95;
// A challenger can reach the threshold only if dG / g_scale > -0.2.
inline constexpr double kOneSidedGoodnessLimit = -0.2;

// ---- Link-weight regressor training ----
inline constexpr std::size_t kTrainingRuns = 3500;
inline constexpr std::size_t kTrainRuns = 2800;
inline constexpr std::size_t kTestRuns = 700;
inline constexpr std::size_t kXgbTrees = 400;
inline constexpr std::size_t kXgbDepth = 5;
inline constexpr double kXgbLearningRate = 0.05;
inline constexpr double kRecordedTestR2 = 0.997;
inline constexpr double kRecordedTestMae = 1.04;
inline constexpr double kTargetOffset = 100.0;  // Target W_link = 100 - score.

// ---- Recorded tournaments of both testbed scenarios ----
enum class Decision { Swap, Keep };

struct Tournament {
  const char* run;
  int number;
  const char* arms;  // "incumbent / challenger"; an arm lists the algorithms that returned its path.
  int incumbent_delivered;  // Of kProbesPerArm.
  double incumbent_mean_rtt_ms;
  int challenger_delivered;
  double challenger_mean_rtt_ms;
  double rtt_ratio;     // Challenger over incumbent.
  double ratio_ci_low;  // 95 % CI.
  double ratio_ci_high;
  double partial_eta_squared;
  double delta_goodness;  // G_challenger - G_incumbent.
  double confidence;      // C as recorded (3 decimals).
  Decision decision;
};

inline constexpr std::array<Tournament, 7> kRecordedTournaments = {{
    {"Congestion", 1, "Dijkstra / A*+ACO+GA", 150, 81.5, 400, 20.4, 0.251, 0.245, 0.256, 1.000, 8.99, 1.000, Decision::Swap},
    {"Congestion", 2, "A* / Dijkstra", 400, 20.4, 400, 8.4, 0.410, 0.409, 0.411, 1.000, -4.46, 0.500, Decision::Keep},
    {"Congestion", 3, "A* / Dijkstra+A*", 400, 20.4, 400, 8.4, 0.412, 0.410, 0.413, 1.000, -4.48, 0.500, Decision::Keep},
    {"Congestion", 4, "A* / Dijkstra+A*", 400, 20.5, 400, 8.5, 0.414, 0.409, 0.419, 1.000, -5.51, 0.500, Decision::Keep},
    {"Loss", 1, "Dijkstra / A*+ACO+GA", 294, 9.2, 400, 20.5, 2.294, 2.161, 2.435, 0.991, 6.03, 0.996, Decision::Swap},
    {"Loss", 2, "A* / Dijkstra", 400, 20.5, 281, 8.5, 0.412, 0.410, 0.414, 1.000, -5.14, -0.200, Decision::Keep},
    {"Loss", 3, "A* / Dijkstra", 400, 20.5, 291, 8.5, 0.414, 0.412, 0.415, 1.000, -5.13, -0.200, Decision::Keep},
}};

}  // namespace aafr::research_fixture
