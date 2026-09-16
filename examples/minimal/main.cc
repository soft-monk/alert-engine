// examples/minimal/main.cc —— 最小示例：
//   装载规则包 → 触发 → 3 秒内重复触发被去重 → 确认（幂等）→ 关闭 → 台账查询 → 计数口径
//
// 示例与测试一样**自己提供依赖实现**（内存 Store / 假时钟 / 打印 Sink），
// 引擎侧只认识反向接口（ALT-SUB-04 / ALT-NFR-01）。
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "alert_engine/alert_engine.h"

using namespace alert_engine;

namespace {

/// 假时钟：示例里"现在"由我们推进，输出因此可复现（ALT-NFR-02）
class StepClock : public IClock {
public:
    explicit StepClock(int64_t start = 1750000000000LL) : now_(start) {}
    int64_t nowMs() const override { return now_; }
    void set(int64_t ms) { now_ = ms; }

private:
    int64_t now_;
};

/// 打印型 Sink：每个事件打一行（宿主真实实现里应改为包信封 + 广播）
class PrintSink : public IAlertSink {
public:
    void onAlertRaised(const AlertDelivery& d) override {
        std::printf("  [sink] alert.raised  %s level=%s count=%lld subscribers=%zu%s\n",
                    d.payload.value("alertId", std::string()).c_str(),
                    d.payload.value("level", std::string()).c_str(),
                    static_cast<long long>(d.payload.value("count", static_cast<int64_t>(0))),
                    d.subscriberIds.size(), d.broadcast ? "（广播）" : "");
    }
    void onAlertUpdated(const AlertDelivery& d) override {
        std::printf("  [sink] alert.updated %s count=%lld upgraded=%d\n",
                    d.payload.value("alertId", std::string()).c_str(),
                    static_cast<long long>(d.payload.value("count", static_cast<int64_t>(0))),
                    d.payload.value("upgraded", false) ? 1 : 0);
    }
    void onAlertAcked(const AlertDelivery& d) override {
        std::printf("  [sink] alert.acked   %s state=%s operator=%s idempotent=%d\n",
                    d.payload.value("alertId", std::string()).c_str(),
                    d.payload.value("state", std::string()).c_str(),
                    d.payload.value("operatorId", std::string()).c_str(),
                    d.payload.value("idempotent", false) ? 1 : 0);
    }
};

/// 内存 Store：宿主真实实现里换成落库（引擎不接触 SQL —— P3）
class MemStore : public IAlertStore {
public:
    bool save(const AlertRecord& rec) override {
        rows[rec.alertId] = rec;
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
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // 控制台切 UTF-8，中文才不乱码；只在真有控制台时切（重定向时 chcp 会失败并抛异常）
    try {
        std::system("chcp 65001 > nul");
    } catch (...) {
    }
#endif
    const std::string policyDir = (argc > 1) ? argv[1] : ALERT_ENGINE_POLICY_DIR;

    std::printf("== alert-engine 最小示例 ==\n");
    std::printf("规则包目录（外部注入）：%s\n", policyDir.c_str());

    auto clock = std::make_shared<StepClock>();
    auto sink = std::make_shared<PrintSink>();
    auto store = std::make_shared<MemStore>();

    AlertEngineOptions opts;
    opts.clock = clock;  // 注入假时钟 → 输出可复现
    opts.sink = sink;
    opts.store = store;
    AlertEngine engine(opts);

    int failures = 0;
    auto expect = [&](const char* what, bool ok) {
        std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok) ++failures;
    };

    // ------------------------------------------------------------------ 规则包
    const LoadResult loaded = engine.loadRulesFromDirectory(policyDir);
    if (loaded.code != 0) {
        std::printf("[FAIL] 规则包装载失败 code=%d %s\n", loaded.code, loaded.message.c_str());
        for (const auto& i : loaded.issues) {
            std::printf("       %s.%s：%s\n", i.path.c_str(), i.field.c_str(), i.reason.c_str());
        }
        return 1;
    }
    std::printf("装载成功：规则 %d 条｜级别 %d 个（%s）｜抑制边 %d 条｜digest=%s\n",
                loaded.data.ruleCount, loaded.data.levelCount,
                [&] {
                    std::string s;
                    for (const auto& k : loaded.data.levelKeys) s += (s.empty() ? "" : "/") + k;
                    return s;
                }()
                                                    .c_str(),
                loaded.data.suppressionEdges, loaded.data.digest.c_str());

    // ------------------------------------------------------------------ 产生
    // 结构化观测（ALT-GEN-01）：{规则id, 实体?, 指标, 数值, 时间, 上下文}
    Observation offline;
    offline.ruleId = "node-offline";
    offline.entityId = "node-1";
    offline.missionId = "mission-1";
    offline.metric = "online";
    offline.value = 0;  // 规则包声明"online == false 持续 3000ms 才报"
    offline.ts = 1000;
    const AlertResult r0 = engine.observe(offline);
    expect("持续时长未满 → 不产生（ALT-RULE-02 的'持续 N 秒才报'）", !r0.raised);

    offline.ts = 4000;  // 满足持续 3000ms
    const AlertResult r1 = engine.observe(offline);
    expect("满足持续时间 → 产生告警", r1.raised);
    const std::string id = r1.alert.alertId;

    // ------------------------------------------------------------------ 去重
    // 3 秒内触发 10 次 → 一条告警、计数 10（ALT-GEN-03）
    for (int i = 0; i < 9; ++i) {
        offline.ts = 4100 + i * 300;
        engine.observe(offline);
    }
    const auto rec = engine.getAlert(id).value();
    std::printf("  3 秒内共触发 10 次 → 台账 %zu 条、该条计数 %lld\n", store->rows.size(),
                static_cast<long long>(rec.count));
    expect("去重：10 次触发归一为 1 条且计数为 10", store->rows.size() == 1 && rec.count == 10);
    expect("首次时间未被合并修改（ALT-DEDUP-02）", rec.firstAt == 4000);

    // ------------------------------------------------------------------ 确认
    const ActionResult a1 = engine.acknowledge(id, "operator-1", "已知悉");
    const ActionResult a2 = engine.acknowledge(id, "operator-1", "已知悉");
    expect("确认成功", a1.code == 0 && a1.changed);
    expect("重复确认幂等（code=0 + idempotent，MUST NOT 报错）", a2.code == 0 && a2.idempotent);

    // ------------------------------------------------------------------ 关闭与台账
    expect("关闭成功", engine.close(id, "operator-1", "恢复确认").code == 0);
    AlertQuery q;
    q.missionIn = {"mission-1"};
    const AlertQueryResult page = engine.listAlerts(q);
    std::printf("  台账查询（任务 mission-1）：命中 %d 条、返回 %d 条、截断=%d\n", page.total,
                page.returned, page.truncated ? 1 : 0);
    expect("台账按任务筛选可查（ALT-SUB-02）", page.total == 1 && !page.truncated);

    // ------------------------------------------------------------------ 计数口径
    const AlertCounts n = engine.counts();
    std::printf("  计数口径：basis=%s alertCount=%lld rawRaises=%lld alertsRaised=%lld merged=%lld\n",
                n.basis.c_str(), static_cast<long long>(n.alertCount),
                static_cast<long long>(n.rawRaises), static_cast<long long>(n.alertsRaised),
                static_cast<long long>(n.merged));
    expect("计数口径显式且可复算（rawRaises == alertsRaised + merged + …）",
           n.rawRaises == n.alertsRaised + n.merged + n.recoveries + n.folded);

    std::printf("\n%s\n", failures == 0 ? "最小示例全部通过" : "最小示例存在失败");
    return failures == 0 ? 0 : 1;
}
