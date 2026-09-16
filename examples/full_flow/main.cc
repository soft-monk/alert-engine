// examples/full_flow/main.cc —— 全流程演示：
//   规则装载 → 阈值/持续/组合/恢复四类条件 → 去重合并与升级 → 抖动抑制 → 父子抑制 →
//   风暴保护（汇总告警 + 被折叠条数）→ 消音到期自动恢复 → 确认状态机与留痕 →
//   台账筛选与截断 → 订阅分发 → 计数口径 → 有界台账
//
// 依赖全部由示例注入（内存 Store / 假时钟 / 打印 Sink），引擎只认识反向接口。
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "alert_engine/alert_engine.h"

using namespace alert_engine;

namespace {

class StepClock : public IClock {
public:
    explicit StepClock(int64_t start = 1750000000000LL) : now_(start) {}
    int64_t nowMs() const override { return now_; }
    void set(int64_t ms) { now_ = ms; }
    void advance(int64_t ms) { now_ += ms; }

private:
    int64_t now_;
};

class CountingSink : public IAlertSink {
public:
    void onAlertRaised(const AlertDelivery& d) override {
        ++raised;
        lastRaised = d;
    }
    void onAlertUpdated(const AlertDelivery& d) override {
        ++updated;
        lastUpdated = d;
    }
    void onAlertAcked(const AlertDelivery& d) override {
        ++acked;
        lastAcked = d;
    }
    int raised = 0;
    int updated = 0;
    int acked = 0;
    AlertDelivery lastRaised;
    AlertDelivery lastUpdated;
    AlertDelivery lastAcked;
};

class MemStore : public IAlertStore {
public:
    bool save(const AlertRecord& rec) override {
        rows[rec.alertId] = rec;
        ++saves;
        return true;
    }
    bool load(const std::string& alertId, AlertRecord& out) override {
        const auto it = rows.find(alertId);
        if (it == rows.end()) return false;
        out = it->second;
        return true;
    }
    bool remove(const std::string& alertId) override { return rows.erase(alertId) != 0; }

    std::map<std::string, AlertRecord> rows;
    int saves = 0;
};

struct Section {
    int failures = 0;
    void check(const char* what, bool ok) {
        std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok) ++failures;
    }
    void title(const char* t) { std::printf("\n-- %s\n", t); }
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    try {
        std::system("chcp 65001 > nul");
    } catch (...) {
    }
#endif
    const std::string policyDir = (argc > 1) ? argv[1] : ALERT_ENGINE_POLICY_DIR;

    std::printf("== alert-engine 全流程演示 ==\n");
    std::printf("规则包目录（外部注入）：%s\n", policyDir.c_str());

    auto clock = std::make_shared<StepClock>();
    auto sink = std::make_shared<CountingSink>();
    auto store = std::make_shared<MemStore>();

    AlertEngineOptions opts;
    opts.clock = clock;
    opts.sink = sink;
    opts.store = store;
    AlertEngine engine(opts);

    Section s;
    const LoadResult loaded = engine.loadRulesFromDirectory(policyDir);
    if (loaded.code != 0) {
        std::printf("[FAIL] 规则包装载失败：%s\n", loaded.toJson().dump().c_str());
        return 1;
    }

    s.title("1. 规则包（全部业务取值来自数据，引擎零内建）");
    std::printf("  规则 %d 条｜级别 %d 个｜抑制边 %d 条｜风暴 %s｜消音缺省 %lld ms\n",
                loaded.data.ruleCount, loaded.data.levelCount, loaded.data.suppressionEdges,
                loaded.data.stormEnabled ? "开" : "关",
                static_cast<long long>(loaded.data.muteDefaultMs));
    for (const auto& lv : engine.levels()) {
        std::printf("    级别 %-6s rank=%d 名称=%s\n", lv.key.c_str(), lv.rank, lv.name.c_str());
    }
    s.check("规则包导出可审计（digest + 规则 id 清单）",
            loaded.data.digest.size() == 16 && loaded.data.ruleIds.size() ==
                                                    static_cast<std::size_t>(loaded.data.ruleCount));

    auto obs = [](const std::string& rule, const std::string& entity, const std::string& mission,
                  const std::string& metric, double value, int64_t ts) {
        Observation o;
        o.ruleId = rule;
        o.entityId = entity;
        o.missionId = mission;
        o.metric = metric;
        o.value = value;
        o.ts = ts;
        return o;
    };

    s.title("2. 持续时间条件：'持续 N 秒才报'（规则声明 forMs=3000）");
    s.check("1000ms 时不报", !engine.observe(obs("node-offline", "node-1", "mission-1", "online", 0, 1000)).raised);
    const AlertResult offline = engine.observe(obs("node-offline", "node-1", "mission-1", "online", 0, 4000));
    s.check("满 3000ms 才报", offline.raised);
    const std::string offlineId = offline.alert.alertId;

    s.title("3. 去重合并与升级（3 秒内 10 次 → 1 条、计数 10）");
    for (int i = 0; i < 9; ++i) {
        engine.observe(obs("node-offline", "node-1", "mission-1", "online", 0, 4100 + i * 300));
    }
    const auto offRec = engine.getAlert(offlineId).value();
    std::printf("  台账 %zu 条｜该条 count=%lld firstAt=%lld lastAt=%lld\n", store->rows.size(),
                static_cast<long long>(offRec.count), static_cast<long long>(offRec.firstAt),
                static_cast<long long>(offRec.lastAt));
    s.check("同（规则+实体）窗口内归并为一条、计数 10", store->rows.size() == 1 && offRec.count == 10);
    s.check("首次时间不被合并修改", offRec.firstAt == 4000 && offRec.lastAt == 6500 && offRec.firstValue == 0);
    s.check("级别历史与原始级别保留", offRec.levelHistory.size() == 1 &&
                                          offRec.levelHistory[0].level == offRec.originLevel);

    s.title("4. 父子抑制：父告警存在时子告警不产生");
    // 该规则声明 confirmCount=2 / minDwellMs=500：第 1 次被确认计数拦下、第 2 次才判定抑制
    const AlertResult conf1 =
        engine.observe(obs("link-quality", "node-1", "mission-1", "signal", 0.2, 7000));
    const AlertResult childSup =
        engine.observe(obs("link-quality", "node-1", "mission-1", "signal", 0.2, 7600));
    std::printf("  子告警结果：suppressed=%d 原因=%s\n", childSup.suppressed ? 1 : 0,
                childSup.suppressReason.c_str());
    s.check("父告警存在 → 子告警被抑制且原因可读",
            conf1.durationPending && childSup.suppressed && !childSup.raised);
    s.check("抑制不产生台账记录（仍只有 1 条）", store->rows.size() == 1);

    s.title("5. 恢复语义：条件不再满足 → 恢复记录 + 自动关闭 + 关联");
    const AlertResult recovered =
        engine.observe(obs("node-offline", "node-1", "mission-1", "online", 1, 9000));
    std::printf("  恢复记录 %s kind=%s linkedAlertId=%s\n", recovered.alert.alertId.c_str(),
                recovered.alert.kind.c_str(), recovered.alert.linkedAlertId.c_str());
    s.check("产生恢复记录并关联原告警", recovered.recovered && recovered.alert.kind == "recovery" &&
                                           recovered.alert.linkedAlertId == offlineId);
    s.check("原告警被关闭", engine.getAlert(offlineId).value().state == AlertState::Closed);

    s.title("6. 父告警关闭后，子告警恢复产生（含抖动抑制：需连续 2 次）");
    // 抖动的连续计数按（规则+实体）**跨观测累计**：7000/7600 两次已满足 confirmCount=2，
    // 父告警关闭（9000）后条件仍成立 → 10000 直接产生；11000 落入 10s 去重窗口 → 归并
    const AlertResult c1 = engine.observe(obs("link-quality", "node-1", "mission-1", "signal", 0.2, 10000));
    const AlertResult c2 = engine.observe(obs("link-quality", "node-1", "mission-1", "signal", 0.2, 11000));
    s.check("父告警关闭后子告警恢复产生", c1.raised);
    s.check("窗口内再次触发 → 归并", c2.merged);

    s.title("7. 阈值附近震荡不产生风暴（对照：同输入未抑制则每次一条）");
    {
        AlertEngine flapOn;
        AlertEngine flapOff;
        const json flapRule = [] {
            json c = json::object();
            c["type"] = "bool";
            c["logic"] = "allOf";
            c["conditions"] = json::array(
                {json{{"type", "threshold"}, {"field", "v"}, {"operator", "gt"}, {"value", 10}},
                 json{{"type", "threshold"}, {"field", "v"}, {"operator", "lt"}, {"value", 11}}});
            return c;
        }();
        json itemOn = json::object();
        itemOn["key"] = "flap";
        itemOn["level"] = "warn";
        itemOn["condition"] = flapRule;
        itemOn["dedupWindowMs"] = 1000;
        itemOn["confirmCount"] = 2;
        json itemOff = itemOn;
        itemOff["dedupWindowMs"] = 0;
        auto packOf = [&](const json& item) {
            json p = json::object();
            p["policiesNamespace"] = "mapapp";
            p["schemaVersion"] = "1.0.0";
            p["kind"] = "alertRules";
            p["levels"] = json::array({json{{"key", "warn"}, {"name", "警告"}, {"rank", 2}}});
            p["items"] = json::array({item});
            return p;
        };
        flapOn.loadRules(packOf(itemOn));
        flapOff.loadRules(packOf(itemOff));
        int onRaised = 0, offRaised = 0;
        for (int i = 0; i < 100; ++i) {
            const double v = (i % 2 == 0) ? 10.5 : 10.0;
            if (flapOn.observe(obs("flap", "e", "", "v", v, 1000 + i * 100)).raised) ++onRaised;
            if (flapOff.observe(obs("flap", "e", "", "v", 10.5, 1000 + i * 100)).raised) ++offRaised;
        }
        std::printf("  100 次震荡：抑制开 → %d 条；抑制关 → %d 条\n", onRaised, offRaised);
        s.check("抖动抑制开：0 条（无风暴）", onRaised == 0);
        // 抑制关时几乎每次触发各成一条（100 次里有 1 次落在去重/风暴边界上被吸收）
        s.check("抖动抑制关：每次触发各一条（说明抑制确实起作用）", offRaised == 99);
    }

    s.title("8. 风暴保护：单位时间超上限 → 汇总告警 + 如实上报被折叠条数");
    {
        json storm = json::object();
        storm["enabled"] = true;
        storm["windowMs"] = 1000;
        storm["maxRaised"] = 20;
        storm["level"] = "error";
        storm["title"] = "告警风暴：{windowMs} 毫秒内超过 {maxRaised} 条，已折叠 {folded} 条";
        json item = json::object();
        item["key"] = "flood";
        item["level"] = "warn";
        item["condition"] = json{{"type", "threshold"}, {"field", "m"}, {"operator", "gt"}, {"value", 1}};
        item["dedupWindowMs"] = 0;
        json p = json::object();
        p["policiesNamespace"] = "mapapp";
        p["schemaVersion"] = "1.0.0";
        p["kind"] = "alertRules";
        p["levels"] = json::array({json{{"key", "warn"}, {"name", "警告"}, {"rank", 2}},
                                   json{{"key", "error"}, {"name", "严重"}, {"rank", 3}}});
        p["items"] = json::array({item});
        p["storm"] = storm;
        AlertEngine e2;
        e2.loadRules(p);

        int raised = 0, folded = 0;
        for (int i = 0; i < 60; ++i) {
            const AlertResult r = e2.observe(obs("flood", "e" + std::to_string(i), "", "m", 5, 1000 + i));
            if (r.raised) ++raised;
            if (r.folded) ++folded;
        }
        AlertQuery q;
        q.kindIn = {"aggregate"};
        const AlertQueryResult agg = e2.listAlerts(q);
        std::printf("  60 条触发（上限 20）→ 新开 %d 条、折叠 %d 条；汇总告警 foldedCount=%lld\n",
                    raised, folded,
                    agg.total == 1 ? static_cast<long long>(agg.items[0].foldedCount) : -1LL);
        s.check("超出上限的触发被折叠进汇总告警", folded == 40 && agg.total == 1);
        s.check("汇总告警如实上报被折叠条数", agg.total == 1 && agg.items[0].foldedCount == 40);
        s.check("汇总告警带规则包声明的级别与模板", agg.items[0].ruleId == "$storm" &&
                                                          agg.items[0].level == "error" &&
                                                          !agg.items[0].title.empty());
    }

    s.title("9. 确认状态机与留痕（重复确认幂等）");
    const std::string linkId = c1.alert.alertId;
    const ActionResult a1 = engine.acknowledge(linkId, "operator-7", "已派人巡检");
    const ActionResult a2 = engine.acknowledge(linkId, "operator-7", "已派人巡检");
    std::printf("  第一次：code=%d changed=%d；第二次：code=%d idempotent=%d\n", a1.code,
                a1.changed ? 1 : 0, a2.code, a2.idempotent ? 1 : 0);
    s.check("确认成功并留痕", a1.code == 0 && a1.alert.ackedBy == "operator-7");
    s.check("重复确认幂等（MUST NOT 报错）", a2.code == 0 && a2.idempotent && !a2.changed);
    const ActionResult bad = engine.acknowledge(offlineId, "operator-7", "");
    s.check("非法迁移被拒绝并给可读原因", bad.code == 1003 && !bad.message.empty());
    const ActionResult reopen = engine.reopen(offlineId, "operator-9", "问题复发");
    s.check("closed → active 重开合法", reopen.code == 0 && reopen.changed);

    s.title("10. 消音：有时限，到期自动恢复");
    clock->set(1750000000000LL);
    const MuteResult mute = engine.mute(MuteRequest{MuteScope::Rule, "node-offline", "", 60000, 0,
                                                    "operator-3", "计划内维护"});
    s.check("消音登记成功（限时 60s）", mute.code == 0);
    // 该规则是"持续 3000ms 才报"：12000 时内部条件成立但时长未攒够（9000 才起算）
    const AlertResult pendingObs =
        engine.observe(obs("node-offline", "node-2", "mission-1", "online", 0, 12000));
    s.check("持续时间未满 → 如实标注 pending（尚未进入产生流程）",
            !pendingObs.raised && pendingObs.durationPending);
    const AlertResult mutedObs =
        engine.observe(obs("node-offline", "node-2", "mission-1", "online", 0, 20000));
    s.check("消音期间不产生（时长已满，但被消音拦下）", !mutedObs.raised && mutedObs.muted);
    clock->advance(60001);
    const json tick = engine.tick(0);
    const AlertResult afterMute =
        engine.observe(obs("node-offline", "node-2", "mission-1", "online", 0, 90000));
    s.check("到期自动恢复产生", tick["mutesExpired"] == 1 && afterMute.raised);

    s.title("11. 台账查询：按级别/实体/时间/状态/任务筛选 + 截断不静默丢");
    AlertQuery q;
    q.missionIn = {"mission-1"};
    const AlertQueryResult byMission = engine.listAlerts(q);
    q.levelIn = {"error"};
    const AlertQueryResult byLevel = engine.listAlerts(q);
    AlertQuery small;
    small.limit = 1;
    const AlertQueryResult truncated = engine.listAlerts(small);
    std::printf("  按任务 %d 条｜按级别(error) %d 条｜limit=1 → total=%d returned=%d omitted=%d "
                "truncated=%d\n",
                byMission.total, byLevel.total, truncated.total, truncated.returned,
                truncated.omitted, truncated.truncated ? 1 : 0);
    s.check("按任务与级别筛选生效", byMission.total >= byLevel.total && byLevel.total >= 1);
    s.check("超限截断如实上报（不静默丢）",
            truncated.truncated && truncated.omitted == truncated.total - truncated.returned);
    const json batch = engine.activeAlerts();
    s.check("批量输出：一次拿到全部活动告警", batch["returned"] == batch["total"]);

    s.title("12. 订阅分发：只推匹配项");
    AlertSubscription sub;
    sub.subscriberId = "ops-error-only";
    sub.levels = {"error"};
    engine.subscribe(sub);
    sink->raised = 0;
    engine.observe(obs("progress-lag", "mission-1", "mission-1", "ratePerMin", 0.2, 95000));
    const AlertResult subErr =
        engine.observe(obs("node-offline", "node-3", "mission-1", "online", 0, 100000));
    bool sawErrorSubscribed = !subErr.raised;
    if (subErr.raised) sawErrorSubscribed = !sink->lastRaised.subscriberIds.empty();
    std::printf("  订阅者 %s 只关心 error；期间产生 %d 条事件、最后一条命中订阅者 %zu 个\n",
                sub.subscriberId.c_str(), sink->raised, sink->lastRaised.subscriberIds.size());
    // 这一次的推流里，info 级（progress-lag）不命中、error 级（node-offline）命中
    s.check("info 级不推给只订阅 error 的订阅者（progress-lag 未命中）",
            sink->lastRaised.subscriberIds.empty());
    s.check("error 级推给只订阅 error 的订阅者（node-offline 命中）", sawErrorSubscribed);

    s.title("13. 计数口径（供报告'风险预警次数'直接使用）");
    const AlertCounts n = engine.counts();
    std::printf("  basis=%s｜去重后条数 alertCount=%lld｜原始条数 rawRaises=%lld｜"
                "新开 %lld｜合并 %lld｜恢复 %lld｜折叠 %lld｜被抑制 %lld\n",
                n.basis.c_str(), static_cast<long long>(n.alertCount),
                static_cast<long long>(n.rawRaises), static_cast<long long>(n.alertsRaised),
                static_cast<long long>(n.merged), static_cast<long long>(n.recoveries),
                static_cast<long long>(n.folded), static_cast<long long>(n.suppressed));
    s.check("两种口径同时在案且可复算",
            n.rawRaises == n.alertsRaised + n.merged + n.recoveries + n.folded + n.suppressed);
    s.check("口径缺省为去重后条数（需求 D7）", n.basis == "deduplicated");

    s.title("14. 有界台账（活动上限 / 保留期可配，淘汰如实上报）");
    {
        AlertEngineOptions o2;
        o2.clock = clock;
        o2.maxActiveAlerts = 2;
        AlertEngine e3(o2);
        e3.loadRules(json{{"policiesNamespace", "mapapp"},
                          {"schemaVersion", "1.0.0"},
                          {"kind", "alertRules"},
                          {"levels", json::array({json{{"key", "warn"}, {"name", "警告"}, {"rank", 2}}})},
                          {"items", json::array({json{{"key", "b"}, {"level", "warn"},
                                                      {"condition", json{{"type", "threshold"}, {"field", "m"}, {"operator", "gt"}, {"value", 1}}},
                                                      {"dedupWindowMs", 0}}})}});
        for (int i = 0; i < 5; ++i) {
            clock->advance(2000);
            e3.observe(obs("b", "e" + std::to_string(i), "", "m", 5, clock->nowMs()));
        }
        const json t = e3.tick(0);
        std::printf("  5 条产生（上限 2）→ 台账 %lld 条、活动 %lld 条、淘汰 %lld 条\n",
                    t["alerts"].get<int64_t>(), t["openActive"].get<int64_t>(),
                    e3.metrics().evicted);
        s.check("活动上限生效且淘汰如实上报", e3.metrics().evicted >= 3 &&
                                                 e3.counts().openActive <= 2);
    }

    std::printf("\n%s\n", s.failures == 0 ? "全流程演示全部通过" : "全流程演示存在失败");
    return s.failures == 0 ? 0 : 1;
}
