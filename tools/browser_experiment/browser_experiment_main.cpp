// AAFR V0.7.3 controlled browser forwarding experiments (section 18).
//
// Real Qt WebEngine page loads against a local origin server, measured by the
// browser itself (wall clock to loadFinished, and the page's Navigation
// Timing entry for time to first byte). Qt WebEngine's proxy is fixed per
// process, so each role runs in its own process:
//
//   --role=direct     Experiment A: no AAFR forwarding (baseline)
//   --role=forwarded  Experiments B-F through the gateway and the relays
//   --all             runs both roles as child processes and writes
//                     <out-dir>/summary.md from their JSON Lines
//
// Every injected delay is a configured edge latency that the relays sleep at
// tunnel set-up (labelled "simulated" in the output); the traffic, the
// relays, the TCP connections and the browser timings are real. Nothing here
// contacts a public website. The expert model (F) runs in shadow mode only;
// with no measured source for its 23 telemetry inputs it is expected to
// abstain, and the configured-profile run is labelled as configured input.

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>
#include <QTextStream>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <algorithm>
#include <cmath>
#include <future>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "browser_forwarding.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "runtime/model/model_events.h"
#include "runtime/model/model_worker_client.h"
#include "tests/support/http_test_server.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"

namespace fwd = aafr::forwarding;
namespace gw = aafr::gateway;
namespace rt = aafr::runtime;

namespace {

QTextStream *g_out = nullptr;

void emitJson(const QJsonObject &o)
{
    *g_out << QJsonDocument(o).toJson(QJsonDocument::Compact) << '\n';
    g_out->flush();
}

struct ProcessUsage {
    double cpu_ms = 0;
    double working_set_mb = 0;
};

ProcessUsage usage()
{
    ProcessUsage u;
#ifdef _WIN32
    FILETIME c, e, k, us;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
        const auto ms = [](FILETIME f) {
            ULARGE_INTEGER v;
            v.LowPart = f.dwLowDateTime;
            v.HighPart = f.dwHighDateTime;
            return v.QuadPart / 10000.0;
        };
        u.cpu_ms = ms(k) + ms(us);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) u.working_set_mb = pmc.WorkingSetSize / 1048576.0;
#endif
    return u;
}

struct Stats {
    int n = 0;
    double mean = 0, median = 0, p90 = 0, stdev = 0, min = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
    Stats s;
    s.n = static_cast<int>(v.size());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / v.size();
    s.median = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
    s.p90 = v[std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(0.9 * v.size())) - 1)];
    double ss = 0;
    for (double x : v) ss += (x - s.mean) * (x - s.mean);
    s.stdev = v.size() > 1 ? std::sqrt(ss / (v.size() - 1)) : 0;
    s.min = v.front();
    s.max = v.back();
    return s;
}

QJsonObject toJson(const Stats &s)
{
    return {{"n", s.n}, {"mean_ms", s.mean}, {"median_ms", s.median}, {"p90_ms", s.p90}, {"stdev_ms", s.stdev},
            {"min_ms", s.min}, {"max_ms", s.max}};
}

struct LoadResult {
    bool ok = false;
    double wall_ms = 0;  // load() to loadFinished, browser process wall clock
    std::optional<double> ttfb_ms;  // Navigation Timing responseStart - requestStart
    std::optional<double> nav_duration_ms;
};

// One navigation of a long-lived view (the view's own set-up is not timed).
LoadResult loadOnce(QWebEngineView *view, const QUrl &url, int timeoutMs = 30000)
{
    LoadResult r;
    bool finished = false;
    const auto c = QObject::connect(view, &QWebEngineView::loadFinished, [&](bool ok) {
        finished = true;
        r.ok = ok;
    });
    QElapsedTimer timer;
    timer.start();
    view->load(url);
    while (!finished && timer.elapsed() < timeoutMs) QTest::qWait(1);
    r.wall_ms = timer.nsecsElapsed() / 1e6;
    QObject::disconnect(c);
    if (!r.ok) return r;
    bool done = false;
    view->page()->runJavaScript(QStringLiteral(
                                    "(() => { const n = performance.getEntriesByType('navigation')[0];"
                                    " return n ? [n.responseStart - n.requestStart, n.duration] : null; })()"),
                                [&](const QVariant &v) {
                                    const QVariantList l = v.toList();
                                    if (l.size() == 2) {
                                        r.ttfb_ms = l[0].toDouble();
                                        r.nav_duration_ms = l[1].toDouble();
                                    }
                                    done = true;
                                });
    QElapsedTimer js;
    js.start();
    while (!done && js.elapsed() < 5000) QTest::qWait(1);
    return r;
}

// A view of the no-cache profile, created once per phase.
std::unique_ptr<QWebEngineView> makeView(QWebEngineProfile *profile)
{
    auto view = std::make_unique<QWebEngineView>();
    view->setPage(new QWebEnginePage(profile, view.get()));
    view->resize(1024, 768);
    view->show();
    return view;
}

int g_sequence = 0;

QJsonObject runPhase(const QString &experiment, const QString &phase, QWebEngineProfile *profile,
                     const aafr::test::HttpTestServer &origin, const QString &path, int trials,
                     gw::AAFRBrowserForwardingGateway *gateway, const QJsonObject &conditions)
{
    std::vector<double> wall, ttfb, nav;
    const auto view = makeView(profile);
    loadOnce(view.get(), QUrl(QStringLiteral("about:blank")));  // warm the view, untimed
    int failures = 0;
    std::map<std::string, int> observed;
    const ProcessUsage before = usage();
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    for (int i = 0; i < trials; ++i) {
        const QString sep = path.contains('?') ? QStringLiteral("&") : QStringLiteral("?");
        const QUrl url(QStringLiteral("http://127.0.0.1:%1%2%3trial=%4").arg(origin.port()).arg(path).arg(sep).arg(++g_sequence));
        const std::int64_t mark = gateway ? [&] {
            const auto all = gateway->Connections(1);
            return all.empty() ? std::int64_t(0) : all.back().accepted_ns;
        }() : 0;
        const LoadResult r = loadOnce(view.get(), url);
        QJsonObject trial{{"type", "trial"}, {"experiment", experiment}, {"phase", phase}, {"trial", i},
                          {"ok", r.ok}, {"browser_wall_ms", r.wall_ms}, {"path", path}};
        if (r.ttfb_ms) trial["browser_ttfb_ms"] = *r.ttfb_ms;
        if (r.nav_duration_ms) trial["browser_navigation_ms"] = *r.nav_duration_ms;
        if (gateway) {
            // Count only connections that carried this trial's requests and
            // have finished (in-flight ones have no observed path yet).
            QElapsedTimer settle;
            settle.start();
            const auto pending = [&] {
                for (const auto &c : gateway->Connections(64))
                    if (c.accepted_ns > mark && c.generation != 0 && c.finished_ns == 0 && c.kind != gw::ConnectionKind::Unknown)
                        return true;
                return false;
            };
            while (pending() && settle.elapsed() < 2000) QTest::qWait(5);
            QJsonArray relays;
            for (const auto &c : gateway->Connections(64)) {
                if (c.accepted_ns <= mark || c.generation == 0 || c.finished_ns == 0) continue;
                relays.append(QString::fromStdString(fwd::PathText(c.observed_path)));
                ++observed[fwd::PathText(c.observed_path)];
                if (c.establish_ms) trial["gateway_establish_ms"] = *c.establish_ms;
                if (c.first_byte_ms) trial["gateway_first_byte_ms"] = *c.first_byte_ms;
                if (c.duration_ms) trial["gateway_duration_ms"] = *c.duration_ms;
            }
            trial["observed_relay_paths"] = relays;
        }
        emitJson(trial);
        if (r.ok) {
            wall.push_back(r.wall_ms);
            if (r.ttfb_ms) ttfb.push_back(*r.ttfb_ms);
            if (r.nav_duration_ms) nav.push_back(*r.nav_duration_ms);
        } else {
            ++failures;
        }
    }
    const ProcessUsage after = usage();
    QJsonObject paths;
    for (const auto &p : observed) paths[QString::fromStdString(p.first)] = p.second;
    QJsonObject summary{{"type", "summary"}, {"experiment", experiment}, {"phase", phase}, {"page", path},
                        {"trials", trials}, {"failures", failures}, {"browser_wall", toJson(stats(wall))},
                        {"browser_ttfb", toJson(stats(ttfb))}, {"browser_navigation", toJson(stats(nav))}, {"observed_relay_paths", paths},
                        {"cpu_ms", after.cpu_ms - before.cpu_ms}, {"working_set_mb", after.working_set_mb},
                        {"phase_wall_ms", static_cast<double>(phaseTimer.elapsed())}, {"conditions", conditions}};
    emitJson(summary);
    return summary;
}

// The forwarding stack of one experiment: relays + verified runtime.
struct Stack {
    aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
    std::unique_ptr<fwd::LoopbackForwardingNetwork> network;
    std::unique_ptr<fwd::LoopbackForwardingExecutor> executor;
    std::unique_ptr<aafr::algorithms::AlgorithmExecutionEngine> engine;
    std::unique_ptr<rt::AAFRRuntime> runtime;

    bool start(bool injectDelay, double a, double b, const rt::IRecommendationProvider *provider = nullptr)
    {
        graph.SetLatency("entry", "relay_a", a);
        graph.SetLatency("relay_a", "dest", a);
        graph.SetLatency("entry", "relay_b", b);
        graph.SetLatency("relay_b", "dest", b);
        fwd::LoopbackOptions o;
        o.inject_edge_latency = injectDelay;
        network = std::make_unique<fwd::LoopbackForwardingNetwork>(*graph.Get(), o);
        network->SetEgressPolicy(fwd::EgressPolicy::LocalTesting());
        executor = std::make_unique<fwd::LoopbackForwardingExecutor>(*network);
        engine = std::make_unique<aafr::algorithms::AlgorithmExecutionEngine>(nullptr, aafr::test::VerifiedRegistry(),
                                                                              executor.get());
        rt::RuntimeConfig c;
        c.session_id = "exp";
        c.scheduler.min_interval_ms = 100;
        c.stability.min_dwell_ms = 200;
        c.stability.cooldown_ms = 300;
        c.stability.sustain_evaluations = 2;
        c.probe.rounds = 12;
        c.health_check_interval_ms = 0;
        c.review_interval_ms = 0;
        c.failures_for_safety_override = 2;
        rt::RuntimeDependencies d;
        d.graph_source = [this] { return graph.Get(); };
        d.problem_for = [](const fwd::ForwardingGraph &g) { return aafr::test::ProblemFor(g); };
        d.executor = executor.get();
        d.eligibility = engine.get();
        d.executor_injects_delay = injectDelay;
        d.provider = provider;
        runtime = std::make_unique<rt::AAFRRuntime>(std::move(d), c);
        std::string error;
        return network->Start(&error) && runtime->Initialize();
    }
    gw::GatewayBindings bindings()
    {
        gw::RuntimeBindingSources s;
        s.runtime = runtime.get();
        s.graph_source = [this] { return graph.Get(); };
        s.problem_for = [](const fwd::ForwardingGraph &g) { return aafr::test::ProblemFor(g); };
        s.executor = executor.get();
        s.network = network.get();
        return gw::MakeRuntimeBindings(s);
    }
    void mutate(const char *from, const char *to, double ms)
    {
        graph.SetLatency(from, to, ms);
        std::string error;
        network->SyncWith(*graph.Get(), &error);
    }
    void stop()
    {
        runtime->Stop();
        network->Stop();
    }
};

void async(const std::function<void()> &f)
{
    auto future = std::async(std::launch::async, f);
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(5);
}

// Evaluations until the runtime commits a switch (at most `max`); wall ms.
double untilSwitch(rt::AAFRRuntime &runtime, int max, rt::DecisionRecord *last)
{
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < max; ++i) {
        async([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            runtime.RequestEvaluation(rt::EvaluationTrigger::Manual, "experiment");
            runtime.Step();
        });
        *last = runtime.Decisions(1).back();
        if (last->type == rt::DecisionType::Switch && last->activation == rt::ActivationStatus::Confirmed) break;
    }
    return static_cast<double>(t.elapsed());
}

QJsonObject decisionJson(const rt::DecisionRecord &r)
{
    QJsonObject o{{"decision_id", QString::fromStdString(r.decision_id)},
                  {"type", rt::ToString(r.type)},
                  {"confirmed_path", QString::fromStdString(fwd::PathText(r.confirmed_path))},
                  {"evidence_validity", QString::fromStdString(r.evidence_validity)},
                  {"probe_overhead_ms", r.probe_overhead_ms},
                  {"probe_requests", static_cast<double>(r.probe_requests)},
                  {"decision_ms", r.decision_ms}};
    if (r.latency_ratio) o["latency_ratio"] = *r.latency_ratio;
    if (r.activation_latency_ms) o["activation_latency_ms"] = *r.activation_latency_ms;
    return o;
}

int runDirect(int trials)
{
    aafr::test::HttpTestServer origin;
    if (!origin.Start()) return 1;
    QWebEngineProfile profile;  // off the record
    profile.setHttpCacheType(QWebEngineProfile::NoCache);
    const QJsonObject cond{{"forwarding", "direct"}, {"injected_delay", "none"}};
    runPhase("A", "A_direct_small_page", &profile, origin, "/page", trials, nullptr, cond);
    runPhase("A", "A_direct_1MiB_page", &profile, origin, "/lines?bytes=1048576", trials, nullptr, cond);
    origin.Stop();
    return 0;
}

int runForwarded(BrowserForwardingSetup *setup, int trials)
{
    auto *gateway = setup->gateway();
    std::string error;
    if (!gateway->Start(&error)) return 1;
    aafr::test::HttpTestServer origin;
    if (!origin.Start()) return 1;
    QWebEngineProfile profile;
    profile.setHttpCacheType(QWebEngineProfile::NoCache);

    // B0: forwarding overhead alone (no injected delay).
    {
        Stack s;
        if (!s.start(false, 0.0, 0.0)) return 1;
        gateway->SetBindings(s.bindings());
        const QJsonObject cond{{"forwarding", "aafr"}, {"path", "entry -> relay_a -> dest"},
                               {"injected_delay", "none"}, {"relays", 3}};
        runPhase("B", "B_fixed_A_no_delay_small_page", &profile, origin, "/page", trials, gateway, cond);
        runPhase("B", "B_fixed_A_no_delay_1MiB_page", &profile, origin, "/lines?bytes=1048576", trials, gateway, cond);
        gateway->Detach();
        s.stop();
    }
    // B, C, D, E on configured (simulated) delays: A 5+5 ms, B 25+25 ms at set-up.
    {
        Stack s;
        if (!s.start(true, 5.0, 25.0)) return 1;
        gateway->SetBindings(s.bindings());
        QJsonObject cond{{"forwarding", "aafr"}, {"injected_delay", "simulated: A 5+5 ms, B 25+25 ms at tunnel set-up"}};
        runPhase("B", "B_fixed_A_configured_delay", &profile, origin, "/page", trials, gateway, cond);

        // C: A degrades by 150 ms. First what staying on A costs, then the switch.
        s.mutate("entry", "relay_a", 150.0);
        cond["injected_delay"] = "simulated: A 150+5 ms (degraded), B 25+25 ms";
        runPhase("C", "C_degraded_A_before_switch", &profile, origin, "/page", trials, gateway, cond);
        rt::DecisionRecord d;
        const double switchMs = untilSwitch(*s.runtime, 6, &d);
        emitJson({{"type", "event"}, {"experiment", "C"}, {"event", "switch"}, {"runtime_switch_wall_ms", switchMs},
                  {"decision", decisionJson(d)}});
        runPhase("C", "C_after_switch", &profile, origin, "/page", trials, gateway, cond);

        // D: the degradation clears; the runtime may return when evidence says so.
        s.mutate("entry", "relay_a", 5.0);
        QTest::qWait(400);  // dwell and cooldown
        const double recoverMs = untilSwitch(*s.runtime, 6, &d);
        emitJson({{"type", "event"}, {"experiment", "D"}, {"event", "recovery_switch"},
                  {"runtime_switch_wall_ms", recoverMs}, {"decision", decisionJson(d)}});
        cond["injected_delay"] = "simulated: A 5+5 ms (restored), B 25+25 ms";
        runPhase("D", "D_after_recovery", &profile, origin, "/page", trials, gateway, cond);

        // E: relay_a fails. Loads fail until the gateway's relay-failure
        // reports trigger the runtime's recovery.
        const auto active = s.runtime->ConfirmedConfiguration();
        const bool onA = active && active->path == aafr::test::kPathA;
        s.network->SetNodeOnline(active ? active->path[1] : std::string("relay_a"), false);
        QElapsedTimer failure;
        failure.start();
        int failed = 0;
        int attempts = 0;
        bool recovered = false;
        const auto failView = makeView(&profile);
        while (attempts < 12 && !recovered) {
            ++attempts;
            const LoadResult r = loadOnce(failView.get(), QUrl(QStringLiteral("http://127.0.0.1:%1/page?failure=%2")
                                                             .arg(origin.port()).arg(attempts)));
            const auto last = gateway->Connections(1);
            const bool viaRelays = r.ok && !last.empty() && last.back().path_confirmed && last.back().http_status == 200;
            if (viaRelays) recovered = true;
            else {
                ++failed;
                async([&] { s.runtime->Step(); });
            }
        }
        const auto after = s.runtime->ConfirmedConfiguration();
        emitJson({{"type", "event"}, {"experiment", "E"}, {"event", "relay_failure"},
                  {"failed_relay", QString::fromStdString(active ? active->path[1] : "relay_a")},
                  {"was_on_path_a", onA}, {"failed_loads", failed}, {"attempts", attempts}, {"recovered", recovered},
                  {"time_to_recovery_ms", static_cast<double>(failure.elapsed())},
                  {"path_after", QString::fromStdString(after ? fwd::PathText(after->path) : "(none)")},
                  {"relay_failures_reported", static_cast<double>(gateway->Status().counters.relay_failures)}});
        cond["injected_delay"] = "simulated; one relay offline";
        runPhase("E", "E_after_failure_recovery", &profile, origin, "/page", trials, gateway, cond);
        gateway->Detach();
        s.stop();
    }

    // F: the expert model in shadow mode (never controls forwarding).
    for (const bool configured : {false, true}) {
        aafr::model::ModelEventLog events;
        auto worker = std::make_unique<aafr::model::ModelWorkerClient>(aafr::model::WorkerConfigFromEnvironment(), &events);
        const bool started = worker->Start() && worker->WaitUntilReady(60000);
        aafr::model::ConfiguredTelemetrySource telemetry(configured ? "configured_profile" : "none");
        if (configured) telemetry.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "configured (not measured)");
        aafr::model::AIProviderConfig pc;
        pc.mode = rt::RecommendationMode::Shadow;
        aafr::model::AIRecommendationProvider provider(worker.get(), &telemetry, pc, &events);
        Stack s;
        if (!s.start(true, 5.0, 25.0, &provider)) return 1;
        gateway->SetBindings(s.bindings());
        s.mutate("entry", "relay_a", 150.0);
        rt::DecisionRecord d;
        untilSwitch(*s.runtime, 4, &d);
        QJsonArray decisions;
        int agree = 0, compared = 0;
        for (const auto &r : s.runtime->Decisions(20)) {
            QJsonObject o{{"decision_id", QString::fromStdString(r.decision_id)},
                          {"type", rt::ToString(r.type)},
                          {"confirmed_algorithm", QString::fromStdString(r.confirmed_algorithm ? aafr::algorithms::ToString(*r.confirmed_algorithm) : "")},
                          {"recommendation_source", QString::fromStdString(r.recommendation_source)}};
            if (r.model) {
                o["inference_status"] = QString::fromStdString(r.model->inference_status);
                o["model_label"] = QString::fromStdString(r.model->model_label);
                if (r.model->deterministic_algorithm)
                    o["deterministic_algorithm"] = aafr::algorithms::ToString(*r.model->deterministic_algorithm);
                if (r.model->agrees_with_deterministic) {
                    ++compared;
                    agree += *r.model->agrees_with_deterministic ? 1 : 0;
                    o["agrees"] = *r.model->agrees_with_deterministic;
                }
                if (r.model->inference_ms) o["inference_ms"] = *r.model->inference_ms;
                if (!r.model->error.empty()) o["model_error"] = QString::fromStdString(r.model->error);
            }
            decisions.append(o);
        }
        const auto active = s.runtime->ConfirmedConfiguration();
        const QJsonObject cond{{"model_inputs", configured ? "23 telemetry inputs from a configured profile (not measured)"
                                                           : "no source for the 23 telemetry inputs"},
                               {"mode", "shadow"}};
        const QString phase = configured ? "F_shadow_configured_profile" : "F_shadow_no_profile";
        runPhase("F", phase, &profile, origin, "/page", std::max(3, trials / 4), gateway, cond);
        emitJson({{"type", "event"}, {"experiment", "F"}, {"event", phase}, {"model_worker_ready", started},
                  {"model_status", QString::fromStdString(aafr::model::ToString(worker->Status().state))},
                  {"compared", compared}, {"agreed", agree},
                  {"confirmed_path", QString::fromStdString(active ? fwd::PathText(active->path) : "")},
                  {"decisions", decisions}});
        gateway->Detach();
        s.stop();
        worker->Stop();
    }
    origin.Stop();
    gateway->Stop();
    return 0;
}

QString fmt(double v) { return QString::number(v, 'f', 1); }

int runAll(const QString &self, const QString &outDir, int trials)
{
    QDir().mkpath(outDir);
    for (const QString role : {QStringLiteral("direct"), QStringLiteral("forwarded")}) {
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        p.start(self, {QStringLiteral("--role=") + role, QStringLiteral("--trials=%1").arg(trials),
                       QStringLiteral("--out=") + outDir + "/" + role + ".jsonl"});
        if (!p.waitForFinished(30 * 60 * 1000) || p.exitCode() != 0) {
            qWarning("role %s failed (exit %d)", qPrintable(role), p.exitCode());
            return 1;
        }
    }
    QFile md(outDir + "/summary.md");
    if (!md.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
    QTextStream o(&md);
    o << "# AAFR V0.7.3 browser forwarding experiments\n\n"
      << "Trials per phase: " << trials << ". Browser-observed times (Qt WebEngine, one long-lived view per phase): "
      << "wall = load() to loadFinished; navigation = Navigation Timing duration; "
      << "TTFB = Navigation Timing responseStart - requestStart. Injected delays are configured (simulated) "
      << "edge latencies slept by the relays at tunnel set-up.\n\n"
      << "| Phase | Page | OK/trials | Wall median (ms) | Wall p90 | Navigation median (ms) | TTFB median | Observed relay paths | Conditions |\n"
      << "|---|---|---|---|---|---|---|---|---|\n";
    QStringList events;
    for (const QString role : {QStringLiteral("direct"), QStringLiteral("forwarded")}) {
        QFile f(outDir + "/" + role + ".jsonl");
        if (!f.open(QIODevice::ReadOnly)) continue;
        while (!f.atEnd()) {
            const QJsonObject j = QJsonDocument::fromJson(f.readLine()).object();
            if (j["type"] == "summary") {
                const QJsonObject w = j["browser_wall"].toObject();
                const QJsonObject t = j["browser_ttfb"].toObject();
                const QJsonObject nv = j["browser_navigation"].toObject();
                QStringList paths;
                const QJsonObject op = j["observed_relay_paths"].toObject();
                for (auto it = op.begin(); it != op.end(); ++it) paths << it.key() + " ×" + QString::number(it.value().toInt());
                QStringList cond;
                const QJsonObject c = j["conditions"].toObject();
                for (auto it = c.begin(); it != c.end(); ++it) cond << it.key() + ": " + it.value().toVariant().toString();
                const int trialsN = j["trials"].toInt();
                o << "| " << j["phase"].toString() << " | " << j["page"].toString() << " | "
                  << (trialsN - j["failures"].toInt()) << "/" << trialsN << " | " << fmt(w["median_ms"].toDouble())
                  << " | " << fmt(w["p90_ms"].toDouble()) << " | " << fmt(nv["median_ms"].toDouble()) << " | "
                  << fmt(t["median_ms"].toDouble()) << " | "
                  << (paths.isEmpty() ? QStringLiteral("direct") : paths.join(", ")) << " | " << cond.join("; ")
                  << " |\n";
            } else if (j["type"] == "event") {
                QJsonObject copy = j;
                copy.remove("decisions");
                events << QString::fromUtf8(QJsonDocument(copy).toJson(QJsonDocument::Compact));
            }
        }
    }
    o << "\n## Events\n\n";
    for (const QString &e : events) o << "- `" << e << "`\n";
    o << "\nRaw per-trial records: direct.jsonl, forwarded.jsonl (model decisions for F are in forwarded.jsonl).\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    QString role;
    int trials = 20;
    QString out;
    QString outDir = QStringLiteral("experiment-results");
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith("--role=")) role = a.mid(7);
        else if (a.startsWith("--trials=")) trials = std::max(1, a.mid(9).toInt());
        else if (a.startsWith("--out=")) out = a.mid(6);
        else if (a.startsWith("--out-dir=")) outDir = a.mid(10);
        else if (a == "--all") all = true;
    }
    if (all) {
        QCoreApplication app(argc, argv);
        return runAll(QCoreApplication::applicationFilePath(), outDir, trials);
    }
    std::unique_ptr<BrowserForwardingSetup> setup;
    if (role == "forwarded") {
        BrowserForwardingOptions options;
        options.mode = BrowserForwardingMode::Controlled;
        options.forwardLoopback = true;  // the origin is a local server
        QString error;
        setup = BrowserForwardingSetup::prepare(options, &error);
        if (!setup) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
    } else if (role != "direct") {
        std::fprintf(stderr, "usage: aafr-browser-experiment --all [--trials=N] [--out-dir=DIR] | --role=direct|forwarded --out=FILE\n");
        return 2;
    }
    QApplication app(argc, argv);
    QFile file(out.isEmpty() ? QStringLiteral("aafr-experiment-%1.jsonl").arg(role) : out);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
    QTextStream stream(&file);
    g_out = &stream;
    return role == "direct" ? runDirect(trials) : runForwarded(setup.get(), trials);
}
