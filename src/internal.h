// src/internal.h —— 内部实现细节。**宿主 MUST NOT 包含本文件**
// （只允许 #include <alert_engine/alert_engine.h>）。
//
// 本文件只放实现用的数据容器与工具，不含任何业务取值（P6 / P7 / ALT-RULE-01）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "alert_engine/alert_engine.h"

namespace alert_engine {
namespace detail {

/// 条件树的编译结果：节点自带**稳定路径 id**（`c` / `c.0` / `c.0.1` …）。
///
/// 为什么需要路径 id：持续时间条件要按"同一节点"累计连续成立时长，
/// 而路径 id 由规则包结构唯一决定（MUST NOT 依赖指针地址 —— 确定性要求 ALT-NFR-03）。
struct CompiledCondition {
    Condition cond;
    std::string nodeId;
    std::vector<CompiledCondition> children;      // allOf / anyOf
    std::shared_ptr<CompiledCondition> child;     // not
    std::shared_ptr<CompiledCondition> inner;     // duration / recovery

    /// 是否是"持续时间"节点（其满足 = 内部条件连续成立 forMs）
    bool isDuration() const { return cond.type == ConditionType::Duration; }
};

/// 持续时间 / 抖动判定的逐节点状态（按 规则+实体+节点 保存）。
struct ConditionState {
    int64_t lastTs = 0;          // 最近一次求值时间（连续性判定）
    bool hasVal = false;
    double val = 0;              // 最近一次"字段存在"的取值
    bool raw = false;            // 内部条件当前是否成立（恢复判定读取它）
    bool durStarted = false;     // 持续时间计时是否在跑
    int64_t durFrom = 0;         // 连续成立起点
    int64_t durSatisfiedAt = 0;  // 首次满足 forMs 的时刻（用于最小驻留判定）
    bool durSatisfied = false;   // 内部条件是否曾经连续成立满 forMs（持久锚点，不清零）
    bool everMet = false;        // **曾经成立过**的持久锚点（任何条件类型都置位，不清零）
};

/// 抖动抑制状态（逐 规则+实体）
struct FlapState {
    int streak = 0;               // 连续满足次数（confirmCount）
    int64_t streakFrom = 0;
    bool sawTrue = false;         // 当前这一轮是否出现过"满足"
};

/// 监控身份（去重锚点 = 规则 + 实体；ALT-GEN-03 / 风险 R3）
struct MonitorKey {
    std::string ruleId;
    std::string entityId;

    bool operator<(const MonitorKey& o) const {
        if (ruleId != o.ruleId) return ruleId < o.ruleId;
        return entityId < o.entityId;
    }
    bool operator==(const MonitorKey& o) const {
        return ruleId == o.ruleId && entityId == o.entityId;
    }
};

/// 已编译的规则（内容全部来自规则包；引擎内部无任何缺省业务取值）
struct RuleRuntime {
    AlertRuleDef def;
    CompiledCondition condition;
    std::string titleTemplate;
    bool enabled = true;
};

/// 风暴保护编译结果（阈值与级别取值全部来自规则包）
struct StormRuntime {
    StormPolicy policy;
    bool enabled = false;
    std::vector<int64_t> raisedAt;  // 窗口内"新开告警"的时间戳（升序）
};

/// 在途观测的连续状态（按 规则+实体 保存；
/// 只在"条件成立且已产生告警"期间存活，条件不成立即整体清空）。
struct MonitorState {
    std::map<std::string, ConditionState> nodes;  // nodeId → 状态
    FlapState flap;
};

/// 一个告警 id 的"同一监控身份"索引
struct DedupIndex {
    std::map<MonitorKey, std::string> openAlertId;    // 未关闭告警的记录 id
    std::map<MonitorKey, std::string> closedAlertId;  // 最近一条已关闭记录（冷却判定用）
};

/// 引擎实现（PIMPL：公开头保持稳定，实现细节不外泄）
struct EngineCore {
    mutable std::mutex mutex;

    std::shared_ptr<IAlertStore> store;
    std::shared_ptr<IClock> clock;
    std::shared_ptr<IAlertSink> sink;
    std::shared_ptr<ILogSink> log;

    // ---- 规则包（protocol.md §5；装载为原子替换） ----
    bool loaded = false;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string digest;
    std::vector<AlertLevelDef> levels;                 // 按 rank 非降序
    std::map<std::string, int> levelRank;              // level key → rank
    mutable std::map<std::string, RuleRuntime> rules;  // 可变：enable/disable（ALT-RULE-04）
    std::vector<std::string> ruleOrder;                // 规则包声明顺序（确定性）
    std::map<std::string, std::vector<std::string>> suppressedBy;  // 父规则 → 子规则集合
    StormRuntime storm;
    MutePolicy mutePolicy;
    std::vector<std::string> warnings;

    // ---- 台账 ----
    std::map<std::string, AlertRecord> alerts;   // alertId → 记录（有序 ⇒ 输出稳定）
    DedupIndex dedup;
    std::map<MonitorKey, std::vector<int64_t>> stormBuckets;  // 保留：按身份的风暴窗口（诊断用）
    std::map<std::string, MuteEntry> mutes;                   // muteId → 消音
    std::map<std::string, AlertSubscription> subscriptions;    // 按注册顺序（有序 map 保序）

    // ---- 监控状态（在途观测的持续时间 / 抖动判定） ----
    std::map<MonitorKey, MonitorState> monitors;

    // ---- 计数与自述 ----
    Metrics metrics;
    AlertCounts counts;
    int64_t nextAlertSeq = 0;
    int64_t nextMuteSeq = 0;
    bool clockInjectedFlag = false;
    bool sinkInjectedFlag = false;
    bool storeInjectedFlag = false;
    bool logInjectedFlag = false;
    int maxActiveAlerts = 0;
    int64_t retentionMs = 0;

    /// 最近一次已知时间（时钟回拨如实上报，不伪造 —— 与 phase-engine CTR-PHE-CK-03 同口径）
    int64_t lastKnownNow = 0;
    /// 最近一次台账清扫时间（ALT-NFR-07 的节流；避免 1000 条/秒下逐条全表扫描）
    int64_t lastHousekeep = 0;

    // ---- 规则包缓存（供 enable/disable 后的原子重编译） ----
    json loadedPack = json::object();
    AlertRulesInfo info;

    const RuleRuntime* findRule(const std::string& ruleId) const {
        auto it = rules.find(ruleId);
        return it == rules.end() ? nullptr : &it->second;
    }
    AlertRecord* findAlert(const std::string& alertId) {
        auto it = alerts.find(alertId);
        return it == alerts.end() ? nullptr : &it->second;
    }
    const AlertRecord* findAlert(const std::string& alertId) const {
        auto it = alerts.find(alertId);
        return it == alerts.end() ? nullptr : &it->second;
    }
    MonitorState& ensureMonitor(const MonitorKey& k) { return monitors[k]; }
    void clearMonitor(const MonitorKey& k) { monitors.erase(k); }

    /// 空闲监控淘汰（有界台账的一部分 —— ALT-NFR-07）：某个（规则+实体）的
    /// 全部计时单元都早于 `cutoff` 即整体移除。**MUST NOT 影响在途的持续时间计时**
    /// （因此只在调用方确认条件长期不成立时才会命中）。
    void pruneMonitors(int64_t cutoff);

    std::string nextAlertId();
    std::string nextMuteId();
};

// ---- 工具（src/util.cc） ----

/// 标识符规则（与 phase-engine 的 gate id 同口径）：`^[a-z][a-z0-9-]{0,63}$`
bool isIdentifier(const std::string& s);

/// 订阅者 id 规则：`^[a-z][a-z0-9-]{0,63}$`（同口径）
bool isSubscriberId(const std::string& s);

/// FNV-1a 64 位摘要（16 位小写十六进制）
std::string fnv1a64(const std::string& bytes);

/// 规则包摘要（= 公开自由函数 `policiesDigest` 的内部实现，供 compileRules 复用）
std::string policiesDigestOf(const json& pkg);

/// 规范化 JSON 文本（键按 ASCII 升序、数组保序、无空白）→ 用于 digest
std::string canonicalJson(const json& v);

/// 逐条装载问题的统一构造
LoadIssue makeIssue(const std::string& path, const std::string& field, const std::string& reason);

/// 规则包 MAJOR 解析（`MAJOR.MINOR.PATCH`）；不合法 → false
bool parseSemver(const std::string& s, int& major, int& minor, int& patch);

/// 模板填充：`{key}` 占位符替换（引擎只做**模板填充**，不产出自然语言 —— D4）
std::string renderTemplate(const std::string& tpl, const std::map<std::string, std::string>& vars);

// ---- 条件编译与求值（src/condition.cc） ----

/// 编译条件子树（纯函数；失败给出 `path` 与原因）。`path` 形如 `items[2].condition`
bool compileCondition(const json& j, const std::string& path, const std::string& nodeId,
                      CompiledCondition& out, std::vector<LoadIssue>& issues);

/// **瞬时**求值（不含持续时间累计）：用于 `evaluateCondition` 公开纯函数与
/// "内部条件是否成立"的判定。`raw` 输出本次命中节点上"内部条件成立"的事实。
bool evalInstant(const CompiledCondition& n, const Observation& obs, bool& raw);

/// 字段取值：先 `values`，再 `context`，最后回落到 `metric → value`。
/// `found=false` 表示字段缺失（Missing 算子据此判定）。
bool lookupField(const Observation& obs, const std::string& field, double& out, bool& found);

/// 带**持续时间累计**的求值（引擎内部用）：按 `state` 逐节点累计连续成立时长。
/// 返回该条件树是否满足（Duration 节点必须连续成立 `forMs`）。
///
/// `maxGapMs`：`<= 0` 时取该 Duration 节点的 `forMs`（即"两次观测间隔超过这一时长
/// 就认为中间没被观察到，重新计时"）—— 这是持续时间的**连续性口径**，属机制不属业务。
/// `everSatisfied`：输出"该树是否曾经满足过"（恢复规则的首次判定锚点）。
bool evalTracked(const CompiledCondition& n, const Observation& obs, MonitorState& st,
                 int64_t maxGapMs, bool* everSatisfied = nullptr);

/// `evalTracked` 的只读伴生：判断条件树的"内部条件"当前是否成立（恢复判定读取它，
/// 不受 Duration 的时长门槛影响 —— ALT-GEN-04："条件不再满足"是原始条件的事实）
bool evalRawState(const CompiledCondition& n, const MonitorState& st);

// ---- 规则包装载（src/policies.cc） ----

/// 编译规则包（纯函数；validateRules 与 loadRules 共用同一实现，避免两套口径）
struct CompiledRules {
    LoadResult result;                       // 对外结果（失败时含逐条 issues）
    bool ok = false;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string digest;
    std::vector<AlertLevelDef> levels;
    std::map<std::string, int> levelRank;
    std::vector<RuleRuntime> rules;          // 按声明顺序
    std::map<std::string, std::vector<std::string>> suppressedBy;
    StormRuntime storm;
    MutePolicy mutePolicy;
    std::vector<std::string> warnings;
    AlertRulesInfo info;
};
CompiledRules compileRules(const json& pkg);

// ---- 序列化（src/json.cc） ----

json alertRecordJson(const AlertRecord& r);
json conditionJson(const Condition& c);

/// 事件负载 = 台账记录 → `alert.raised` 的 data（契约字段在前，只增字段在后）
AlertRaisedEvent raisedEventOf(const AlertRecord& r);
AlertUpdatedEvent updatedEventOf(const AlertRecord& r, const MergeEntry& m);
AlertAckedEvent ackedEventOf(const AlertRecord& r, const StateChange& s, bool idempotent);

/// 订阅匹配（ALT-SUB-03：空集合 = 不筛；非空集合 MUST 命中）
bool subscriptionMatches(const AlertSubscription& s, const AlertRecord& r);

}  // namespace detail

/// PIMPL 收口：公开头的 `AlertEngine::Impl` 就是 `detail::EngineCore`
struct AlertEngine::Impl : detail::EngineCore {};

}  // namespace alert_engine
