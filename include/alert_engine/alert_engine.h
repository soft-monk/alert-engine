// alert-engine · alert_engine.h —— 唯一公开头文件
//
// 权威依据（本文件不新增业务取值；一切业务内容来自规则包）：
//   · 需求专篇 docs/需求/alert-engine需求专篇.md
//       ALT-RULE 5 / ALT-GEN 6 / ALT-DEDUP 6 / ALT-ACK 5 / ALT-SUB 6 / ALT-NFR 7 = 35 条
//   · 共享契约 ../phase-engine/docs/契约/protocol.md v1.0
//       P1–P10、§3 错误码、§4 事件名（`alert.raised` / `alert.updated` / `alert.acked`
//       已登记，见 §4.4）、§5 policies schema（`kind:"alertRules"`）、§6 反向接口命名
//   · 冲突裁决 ../phase-engine/docs/契约/冲突裁决.md
//       C15（废弃 1001：幂等成功 = `code=0` + `data.idempotent=true`）
//       C16（收窄 1002 = 冲突拒绝 + HTTP 409，MUST NOT 用于幂等）
//       C17（ADR-C17-04：专篇与 protocol §3 冲突时一律以 protocol 为准）
//
// 三条不可协商的口径：
//   1. **引擎不认识任何业务规则**。级别取值与名称、阈值、持续时间、去重窗口、抖动参数、
//      抑制表、自动关闭策略、风暴上限、消音默认时长全部来自注入的规则包
//      （`kind:"alertRules"` 的 `levels` / `items` / `storm` / `mute` 段）。
//      本文件内 MUST NOT 出现任何业务取值（P6 / P7 / ALT-RULE-01）。
//      判据（需求 §4）：公开接口里出现"失联 3 秒"、"链路受限"、"高危区"即破线。
//   2. **引擎不落库、不广播、不推 UI、不取挂钟、不做业务判断**。出口为反向接口
//      `IAlertStore` / `IAlertSink` / `IClock` / `ILogSink`；未注入时引擎仍 MUST 可工作
//      （P8 / P9 / ALT-SUB-04 / ALT-NFR-01）。
//   3. **失败 MUST NOT 抛异常跨边界**（P10）。一切裁决走统一信封 `{code, message, data}`，
//      其中 `code` 只取 protocol.md §3.2 的取值；`1001` 保留不用（C15）。
//
// 恒等式 / 不变量（任何操作序列后 MUST 成立）：
//   · `count == max(1, 合并次数)`；`count == firstTriggerCount + mergeCount`
//   · `firstAt` 一经产生 MUST NOT 被合并修改（ALT-DEDUP-02）
//   · `levelHistory[0].level == originLevel`；`level == levelHistory.back().level`（ALT-DEDUP-04）
//   · `stateHistory` 与 `mergeHistory` 只追加、不改写（ALT-ACK-03 的审计可复原性）
//   · 台账计数口径恒成立：`counts.rawRaises == counts.alertsRaised + counts.merged`
//     （ALT-GEN-05：`alert_count` 口径显式、可复算）
//
// 宿主只允许 #include <alert_engine/alert_engine.h>；其余头文件是内部实现细节。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace alert_engine {

/// 引擎的 JSON 类型。
///
/// 为什么是 `ordered_json`（而不是 nlohmann 默认的 `json`）：
/// 契约要求返回信封的键序稳定（宿主与前端按声明顺序读取），且确定性验收要求
/// "同输入 + 同假时钟 → 逐字节一致"（ALT-NFR-03）。nlohmann 默认的 `json`
/// 以 `std::map` 为后端，会按键名字典序重排；`ordered_json`（`std::vector` 后端）
/// 保留插入顺序。两者取值语义、`dump()`、`parse()` 行为一致。
using json = nlohmann::ordered_json;

/// 引擎支持的规则包 MAJOR（protocol.md §5.2：不匹配 MUST 拒绝装载 + code 1006，
/// MUST NOT 静默降级）
inline constexpr int kSupportedPoliciesMajor = 1;
/// 引擎版本（capabilities 自述用）
inline constexpr const char* kEngineVersion = "0.1.0";
/// 聚合（风暴汇总）告警的内建规则 id。
///
/// **不是业务规则**：它没有阈值、没有条件、没有级别取值 —— 级别与文案仍由规则包的
/// `storm` 段声明。它只是一个**保留 id**，用于让"汇总告警"与业务规则互不冲突
/// （同 phase-engine 的 `$` 前缀 gate id：保留命名空间 MUST NOT 被规则包占用）。
inline constexpr const char* kAggregateRuleId = "$storm";

// ============================================================================
// §0 JSON 序列化入口（**前置声明**）
// ============================================================================
//
// 为什么放在最前面：`*::toJson()` 成员与同名自由函数在同一个命名空间里共存，
// 而超载决议只看**调用点之前**已声明的重载（MSVC 的两阶段查找对此尤其严格）。
// 把全部自由函数重载前置，调用方（宿主与引擎内部）无论在哪里写 `toJson(x)`
// 都能解析到正确的那一个。
struct AlertLevelDef;
struct AlertRuleDef;
struct StormPolicy;
struct MutePolicy;
struct AlertRulesInfo;
struct Condition;
struct Observation;
struct MergeEntry;
struct LevelChange;
struct StateChange;
struct AlertRecord;
struct AlertResult;
struct ActionResult;
struct AlertQuery;
struct AlertQueryResult;
struct AlertCounts;
struct AlertSubscription;
struct AlertRaisedEvent;
struct AlertUpdatedEvent;
struct AlertAckedEvent;
struct AlertDelivery;
struct MuteEntry;
struct MuteRequest;
struct MuteResult;
struct LoadIssue;
struct LoadResult;
struct Capabilities;
struct Metrics;

// JSON 序列化（camelCase；键序与成员声明顺序一致）
json toJson(const AlertLevelDef& v);
json toJson(const StormPolicy& v);
json toJson(const MutePolicy& v);
json toJson(const AlertRulesInfo& v);
json toJson(const Condition& v);
json toJson(const Observation& v);
json toJson(const MergeEntry& v);
json toJson(const LevelChange& v);
json toJson(const StateChange& v);
json toJson(const AlertCounts& v);
json toJson(const AlertSubscription& v);
json toJson(const AlertRaisedEvent& v);
json toJson(const AlertUpdatedEvent& v);
json toJson(const AlertAckedEvent& v);
json toJson(const AlertDelivery& v);
json toJson(const MuteEntry& v);
json toJson(const LoadIssue& v);
json toJson(const LoadResult& v);
json toJson(const AlertResult& v);
json toJson(const ActionResult& v);
json toJson(const AlertQueryResult& v);
json toJson(const Capabilities& v);
json toJson(const Metrics& v);

// ============================================================================
// §1 错误码（protocol.md §3.2 的取值，逐值对齐；1001 保留不用 —— 冲突裁决 C15）
// ============================================================================

enum class ErrorCode {
    Ok = 0,                // 唯一成功码；幂等命中也是 0 + data.idempotent=true（CTR-EC-01）
    BadRequest = 1000,     // 请求参数错误 / 规则包形状非法
    Conflict = 1002,       // 冲突拒绝（HTTP 409）；与幂等成功 MUST 分开表达（C16）
    Unmet = 1003,          // 前置条件未满足：非法状态迁移 / 规则包 MAJOR 不符之外的不通过项
    NotFound = 1004,       // 资源不存在：未知告警 id / 未知规则 id
    Internal = 1005,       // 服务端执行失败（含 store 写失败 —— 写前提交，ALT-NFR-* 同口径）
    VersionMismatch = 1006 // 规则包 schemaVersion 的 MAJOR 不符（CTR-PL-01 / §5.2）
};

/// 三态结果（对齐 phase-engine 的 TransitionStatus 口径）
enum class ResultStatus {
    Ok,              // 状态已改变
    Rejected,        // 被拒绝（code 非 0，或 1003 的非法迁移）
    AlreadyApplied   // 幂等命中：code=0 + idempotent=true + 零副作用（CTR-EC-01）
};

/// 告警状态机（ALT-ACK-01）：`active → acknowledged → closed`，`closed → active`（reopen）
enum class AlertState { Active, Acknowledged, Closed };

/// 条件类型（ALT-RULE-02 的四类：阈值 / 持续时间 / 布尔组合 / 恢复）。
///
/// 引擎只认识这四种"机制"；**具体阈值、字段名、持续时间全部来自规则包**。
enum class ConditionType {
    Threshold,   // 阈值比较：values[field] <op> threshold
    Duration,    // 持续时间："持续 N 秒才报"（`forMs` 为内部条件连续成立的时长）
    Bool,        // 布尔组合：allOf / anyOf / not
    Recovery     // 恢复：（`not` 的内部条件成立即视为"条件不再满足"）
};

/// 比较算子。`in` / `notIn` 用 `values` 数组；其余用 `value`。
enum class CompareOp { Eq, Ne, Gt, Gte, Lt, Lte, Exists, Missing, In, NotIn };

/// 合并策略（ALT-DEDUP-01：窗口内合并；窗口外新开一条）
enum class MergeMode {
    Accumulate,   // 缺省：计数累加（ALT-GEN-03 的"3 秒内触发 10 次 → 计数 10"）
    Latest,       // 只更新最近值与最近时间，计数不累加
    Max,          // 计数取"窗口内去重后的不同实体数上限"，数值取最大（适用于聚合口径）
    Keep          // 计数与数值都不变，只刷新 lastAt
};

/// 恢复语义（ALT-GEN-04 / ALT-ACK-05：按规则声明）
enum class RecoveryMode {
    None,      // 不产生恢复记录、不自动关闭
    Record,    // 产生一条 `kind="recovery"` 的恢复记录，并可关联原告警（linkedAlertId）
    Close      // 按规则自动关闭原告警并留痕（ALT-ACK-05）
};

/// 消音范围
enum class MuteScope { Rule, Entity };

// ============================================================================
// §2 规则包形状（protocol.md §5；`kind:"alertRules"`）
// ============================================================================

/// 条件树节点（ALT-RULE-02：阈值 / 持续时间 / 布尔组合 / 恢复 四类机制）。
///
/// 求值输入是**结构化观测**（ALT-GEN-01：引擎 MUST NOT 接受"成句文案"）：
///   · `field` 缺省 = 观测的 `metric`；显式给出时先查 `Observation.values[field]`
///     （布尔组合可比较多个指标），再回落 `Observation.context[field]`（布尔标志）
///     —— 字段名一律来自规则包，引擎不认识任何具体字段。
///   · `operator == Missing` 即需求里的**缺失**条件；`type == Recovery` 即**恢复**条件。
struct Condition {
    ConditionType type = ConditionType::Threshold;

    // ---- Threshold / Recovery ----
    std::string field;               // 空 = 观测的 metric
    CompareOp op = CompareOp::Gte;
    double value = 0;                // 数值比较的右值
    std::vector<std::string> values; // in / notIn
    std::string textValue;           // 字符串比较（eq/ne 时 `value` 不适用）
    bool hasTextValue = false;

    // ---- Duration ----
    std::shared_ptr<Condition> inner;  // 内部条件（连续成立 forMs 才算满足）
    int64_t forMs = 0;                 // 持续时间（规则包取值）

    // ---- Bool ----
    std::string logic;                       // allOf | anyOf | not
    std::vector<Condition> children;         // allOf / anyOf
    std::shared_ptr<Condition> child;        // not

    json toJson() const;                     // 条件树（camelCase；键序 = 成员声明顺序）
};

/// 级别声明（ALT-GEN-02：级别取值**可扩展**，MUST NOT 硬编码三级）。
///
/// 级别目录住在规则包的 `levels` 段：`key` 为取值、`rank` 为升级比较用的**非降序**位次、
/// `name` 为展示名（引擎不解释、不匹配中文）。级别**不限于三个**，也不限名称 ——
/// 引擎只做 `rank` 比较，因此"新增一个级别只需改规则包"。
struct AlertLevelDef {
    std::string key;    // 级别取值（规则包定义；引擎只当字符串用）
    std::string name;   // 展示名（属规则包）
    int rank = 0;       // 位次（越大越严重）；同一份目录内 MUST 唯一
    std::string color;  // 界面配色语义（透传，引擎不使用）
};

/// 规则定义（ALT-RULE-01：规则由**数据**声明；引擎 MUST NOT 内建任何业务规则）。
struct AlertRuleDef {
    std::string id;                    // 规则 id（唯一）；MUST 匹配 ^[a-z][a-z0-9-]{0,63}$
    std::string version;               // 规则版本（ALT-RULE-05：告警 MUST 携带产生时的规则版本）
    bool enabled = true;               // 初值；运行时可 enable/disable（ALT-RULE-04）
    std::string level;                 // 产生时的级别取值（MUST ∈ levels[].key）
    std::string missionId;             // 规则级任务归属（可空；观测可覆盖）
    Condition condition;               // 触发条件（ALT-RULE-02 四类）
    int64_t dedupWindowMs = 0;         // 去重窗口；<=0 表示不合并（每次触发新开一条）
    MergeMode merge = MergeMode::Accumulate;
    RecoveryMode recoverOn = RecoveryMode::None;
    bool suppressChildren = false;     // ALT-DEDUP-05：本条存在时抑制"以本条为父"的子告警
    std::vector<std::string> suppressChildrenOf;  // 反向批量声明：这些规则的子告警被本条抑制
    std::string parent;                // 父规则 id：父存在时本条被抑制
    int confirmCount = 1;              // 抖动抑制：连续 N 次满足才产生（ALT-DEDUP-03）
    int minDwellMs = 0;                // 抖动抑制：判定为"满足"后的最小驻留时长
    int64_t cooldownMs = 0;            // 抖动抑制：关闭后该时长内不新开，只累加原记录
    int64_t maxGapMs = 0;              // 持续时间连续性：两次观测间隔超过它即重新计时（0=自动）
    std::string muteDefaultMs;         // 保留字段（消音默认时长在 storm/mute 段声明）
};

/// 风暴保护（ALT-DEDUP-06：单位时间告警条数超上限 → 汇总告警 + 如实上报被折叠条数）。
///
/// **阈值与级别取值全部来自规则包**；`enabled=false` 时引擎完全不折叠。
struct StormPolicy {
    bool enabled = false;
    int64_t windowMs = 0;     // 单位时间窗口
    int maxRaised = 0;        // 窗口内新开告警条数上限（超限开始折叠）
    std::string level;        // 汇总告警的级别取值（MUST ∈ levels[].key）
    std::string title;        // 汇总告警标题模板（属规则包）
    bool countActive = false; // 汇总是否同时统计窗口内的活动告警
};

/// 消音缺省（ALT-ACK-04：消音有时限，到期自动恢复）
struct MutePolicy {
    int64_t defaultMs = 0;   // `mute()` 未给 `durationMs`/`untilMs` 时的时长（0 = 必须显式给）
    int64_t maxMs = 0;       // 单次消音上限（0 = 不限）；超出即拒绝
};

/// 规则包的信息与生效内容（CTR-PL-06：MUST 可导出"当前生效规则"供审计与问题复现）
struct AlertRulesInfo {
    bool loaded = false;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string digest;                  // FNV-1a 64 → 16 位小写十六进制
    int ruleCount = 0;
    int levelCount = 0;
    int disabledCount = 0;
    int suppressionEdges = 0;
    bool stormEnabled = false;
    int64_t muteDefaultMs = 0;
    std::vector<std::string> levelKeys;  // 按 rank 非降序
    std::vector<std::string> ruleIds;    // 按规则包声明顺序
    std::vector<std::string> warnings;   // 未知字段 / 未知顶层段（CTR-PL-03）
};

// ============================================================================
// §3 观测（ALT-GEN-01 的结构化产生接口；ALT-NFR-02 的时间可注入）
// ============================================================================

/// 结构化观测（ALT-GEN-01 的产生接口入参；ALT-NFR-02 的时间可注入）。
///
/// 入参形状**结构化**：`{ruleId, entityId?, missionId, metric, value, ts, context}`。
/// 引擎不产出自然语言；文案由规则模板或 `llm-provider` 生成（D4）。
///
/// 聚合初始化短形式：`engine.observe({ruleId, entityId, missionId, metric, value, ts})`。
struct Observation {
    std::string ruleId;
    std::string entityId;   // 可空（规则级观测）
    std::string missionId;  // 可空 → 回落规则包与台账记录的归属
    std::string metric;     // 指标名（字段名来自规则包）
    double value = 0;
    int64_t ts = 0;         // epoch 毫秒（P5）；持续时间与去重窗口一律以它为准
    std::string contextJson;  // 保留位（结构化上下文；当前实现只读 `context`）

    /// 结构化上下文：布尔组合可比较的附加字段（如 `{"online": 0}`）
    std::map<std::string, double> context;
    /// 多指标值表（缺省时 `metric → value` 即唯一取值）
    std::map<std::string, double> values;

    json toJson() const;   // 观测（camelCase）
};

// ============================================================================
// §4 告警台账（ALT-GEN / ALT-DEDUP / ALT-ACK）
// ============================================================================

/// 一次消除（合并）的记录（ALT-DEDUP-02/04：合并可追溯）
struct MergeEntry {
    int64_t at = 0;
    int countAdded = 1;
    double value = 0;
    bool hasValue = false;
    bool upgraded = false;   // 本次合并发生级别升高（ALT-DEDUP-04）
    bool reopened = false;   // 本次合并把已关闭的告警重新打开
    std::string fromLevel;   // 升级前级别
    std::string toLevel;     // 升级后级别
};

/// 一次级别变化的记录（ALT-DEDUP-04：保留原级别历史）
struct LevelChange {
    int64_t at = 0;
    std::string level;
    std::string reason;   // created | upgraded | reopened
};

/// 一次状态迁移的记录（ALT-ACK-03：确认 / 关闭 / 重开 MUST 记录操作者与时间）
struct StateChange {
    int64_t at = 0;
    std::string fromState;
    std::string toState;
    std::string action;       // create | acknowledge | close | reopen | auto-close
    std::string operatorId;   // 操作者 id（由宿主注入；自动动作为空）
    std::string reason;
};

/// 一条告警（台账记录；ALT-GEN-03 的幂等锚点 = （规则 + 实体））
struct AlertRecord {
    std::string alertId;        // 唯一 id（`a-<seq>`；确定性、MUST NOT 依赖地址/时间）
    std::string ruleId;
    std::string ruleVersion;    // ALT-RULE-05：产生时的规则版本
    std::string kind;           // alert | recovery | aggregate（风暴汇总）
    std::string level;          // 当前级别
    std::string originLevel;    // 首次触发时的级别（合并 MUST NOT 改动它）
    std::string entityId;       // 幂等锚点的一半（可为空 = 规则级）
    std::string missionId;      // 任务归属（ALT-SUB-02：告警 MUST 可归属到任务）
    std::string metric;         // 触发的指标名
    AlertState state = AlertState::Active;
    double firstValue = 0;
    double lastValue = 0;
    int64_t firstAt = 0;        // ALT-DEDUP-02：MUST NOT 被合并修改
    int64_t lastAt = 0;
    int64_t count = 0;          // 触发/合并计数（ALT-GEN-03：3 秒 10 次 → 10）
    int64_t foldedCount = 0;    // 仅汇总告警：被折叠的条数（ALT-DEDUP-06 如实上报）
    int64_t windowStart = 0;    // 当前去重窗口的起点
    bool active = true;         // state == Active || Acknowledged（未关闭）
    std::string linkedAlertId;  // 恢复记录指向的原告警（ALT-GEN-04 关联）
    std::string parentRuleId;   // 触发时存在的父告警所属规则（抑制关系留痕）
    std::string title;          // 模板填充后的结构化文案（规则模板，ALT-GEN-01）
    /// 引擎内部记账（**不进 JSON**）：本条记录最近一次观测时"条件是否成立"。
    /// 恢复判定读它 —— 只有成立过的告警才谈得上"不再成立"（ALT-GEN-04）。
    bool conditionHeld = false;
    std::string ackedBy;
    int64_t ackedAt = 0;
    std::string closedBy;
    int64_t closedAt = 0;
    std::string closeReason;
    std::vector<LevelChange> levelHistory;   // [0] = 创建时级别
    std::vector<MergeEntry> mergeHistory;
    std::vector<StateChange> stateHistory;
    json toJson() const;        // 台账行（camelCase；键序 = 成员声明顺序）
};

// ============================================================================
// §5 结果形状（统一信封 `{code, message, data}`）
// ============================================================================

/// 产生 / 触发类结果
struct AlertResult {
    ResultStatus status = ResultStatus::Rejected;
    int code = 0;
    std::string message;
    bool raised = false;        // 本次产生了一条新告警
    bool merged = false;        // 本次并入既有告警（窗口内）
    bool suppressed = false;    // 本次被抑制（父告警存在 / 消音 / 风暴折叠）
    bool recovered = false;     // 本次产生了恢复记录
    bool muted = false;         // 本次因消音而未产生
    bool folded = false;        // 本次被风暴保护折叠
    bool durationPending = false;  // 持续时间条件尚未满足（ALT-RULE-02 的"持续 N 秒才报"）
    std::string ruleId;
    std::string suppressReason; // 抑制原因（可读）
    AlertRecord alert;          // 命中的台账记录（未命中则 `alertId` 为空）
    json toJson() const;
};

/// 状态迁移类结果（确认 / 关闭 / 重开；ALT-ACK-01/02/03）
struct ActionResult {
    ResultStatus status = ResultStatus::Rejected;
    int code = 0;
    std::string message;
    bool idempotent = false;    // CTR-EC-01：幂等命中 MUST 为 true（code 仍为 0）
    bool conflict = false;      // CTR-EC-03：互斥拒绝时 true（code 1002）
    std::string action;         // acknowledge | close | reopen
    std::string operatorId;
    std::string fromState;
    std::string toState;
    bool changed = false;       // 状态是否真的变了
    AlertRecord alert;
    json toJson() const;
};

/// 台账查询入参（ALT-SUB-01 / ALT-SUB-02：按级别 / 实体 / 时间区间 / 状态 / 任务筛选）
///
/// 空集合 = 不筛该项。`includeRecovery` / `includeAggregate` 缺省 true（口径显式）。
struct AlertQuery {
    std::vector<std::string> levelIn;
    std::vector<std::string> stateIn;      // active | acknowledged | closed
    std::vector<std::string> entityIn;
    std::vector<std::string> ruleIn;
    std::vector<std::string> missionIn;    // ALT-SUB-02：按任务筛选
    std::vector<std::string> kindIn;       // alert | recovery | aggregate
    bool includeRecovery = true;
    bool includeAggregate = true;
    bool activeOnly = false;               // 只回未关闭（active + acknowledged）
    int64_t from = 0;                      // 时间区间：按 lastAt 判定（含端点）
    int64_t to = 0;                        // 0 = 不限
    bool useFirstAt = false;               // true 时按 firstAt 判定时间区间
    int limit = 100;                       // 单次上限（缺省 100）
    int offset = 0;
};

/// 台账查询结果（ALT-SUB-01：**超限截断不静默丢**）
struct AlertQueryResult {
    int code = 0;
    std::string message;
    int total = 0;          // 筛选命中的总条数
    int returned = 0;       // 本次实际返回的条数
    bool truncated = false; // 是否发生截断（true 时 MUST NOT 被当作"就这些"）
    int omitted = 0;        // 被截断掉的条数（= total - offset - returned，下界 0）
    std::string truncationReason;  // 可读原因（limit / offset …）
    int limit = 0;
    int offset = 0;
    std::vector<AlertRecord> items;
    json toJson() const;    // `{total, returned, truncated, omitted, limit, offset, items[]}`
    json toEnvelope() const;  // `{code, message, data}`
};

/// 计数口径（ALT-GEN-05：**显式**，供报告"风险预警次数"直接使用）
///
/// 两种口径同时在案，且**可由台账复算**：
///   · `rawRaises`   —— 原始条数：每一次"条件满足的触发"各算一条
///   · `alertCount`  —— 去重后条数：台账里的记录条数（告警 + 恢复 + 汇总）
///
/// `basis` 声明"报告应当取哪个"（D7 缺省 `deduplicated`）；宿主 MUST 按 `basis` 取数，
/// 并把 `basis` 一并落进报告，否则口径会漂移。
struct AlertCounts {
    std::string basis = "deduplicated";  // deduplicated | raw
    int64_t alertCount = 0;   // 去重后条数（= 台账记录总数）
    int64_t rawRaises = 0;    // 原始条数（触发次数总和）
    int64_t alertsRaised = 0; // 新开告警条数（不含恢复与汇总）
    int64_t merged = 0;       // 被合并进既有告警的次数
    int64_t recoveries = 0;   // 恢复记录条数
    int64_t aggregates = 0;   // 风暴汇总告警条数
    int64_t suppressed = 0;   // 被抑制的次数（父告警/消音）
    int64_t folded = 0;       // 被风暴保护折叠的条数
    int64_t openActive = 0;   // 当前活动（未关闭）条数
    json toJson() const;      // 口径可复算：rawRaises == alertsRaised + merged
};

// ============================================================================
// §6 订阅式分发（ALT-SUB-03 / ALT-SUB-05）
// ============================================================================

/// 订阅者声明（ALT-SUB-03：声明关心的规则 / 级别 / 实体，引擎**只推匹配项**）
///
/// 空集合 = 不筛该项（"全部"）。匹配口径：每个非空集合都必须命中（AND），
/// 集合内部任一命中即可（OR）—— 这样"关心规则 R 的 warn 与 error"可以表达。
struct AlertSubscription {
    std::string subscriberId;
    std::vector<std::string> ruleIds;
    std::vector<std::string> levels;
    std::vector<std::string> entityIds;
    std::vector<std::string> missionIds;
    bool includeRecovery = true;
    bool includeAggregate = true;
};

/// 事件负载（= protocol.md §4.4 登记事件 `alert.raised` 的 `data`；信封由宿主包）
///
/// 契约字段**逐字不改**：`{alertId, ruleId, level, entityId?, missionId, firstAt, count}`
/// 其中 `entityId` 缺失时**省略该字段**（不写 null —— 对齐 realtime-hub/protocol §3）。
/// 其余字段为**只增**（CTR-EV-04：负载字段只增不改），既有前端 `applyWs` 的 `alert`
/// case 因此零改动即可工作（ALT-SUB-06）。
struct AlertRaisedEvent {
    std::string alertId;
    std::string ruleId;
    std::string level;
    std::string entityId;   // 空 → 省略
    std::string missionId;
    int64_t firstAt = 0;
    int64_t count = 0;
    // ---- 只增字段 ----
    std::string kind;           // alert | recovery | aggregate
    std::string title;          // 模板填充后的文案（宿主可直接给浮层）
    std::string metric;
    double value = 0;
    std::string ruleVersion;
    std::string originLevel;
    std::string linkedAlertId;  // 恢复记录指向的原告警
    int64_t foldedCount = 0;    // 汇总告警：被折叠条数（ALT-DEDUP-06 如实上报）
    int64_t ts = 0;
    json toJson() const;
};

/// = `alert.updated` 的 data（合并 / 升级）
struct AlertUpdatedEvent {
    std::string alertId;
    int64_t count = 0;
    int64_t lastAt = 0;
    std::string level;
    bool upgraded = false;
    // ---- 只增字段 ----
    std::string ruleId;
    std::string entityId;
    std::string missionId;
    std::string fromLevel;
    std::string originLevel;
    double value = 0;
    int64_t countAdded = 0;
    int64_t foldedCount = 0;
    int64_t ts = 0;
    json toJson() const;
};

/// = `alert.acked` 的 data（确认 / 关闭 / 重开）
struct AlertAckedEvent {
    std::string alertId;
    std::string state;      // active | acknowledged | closed
    std::string operatorId;
    int64_t at = 0;
    // ---- 只增字段 ----
    std::string ruleId;
    std::string level;
    std::string entityId;
    std::string missionId;
    std::string action;     // acknowledge | close | reopen | auto-close
    std::string fromState;
    std::string reason;
    bool idempotent = false;
    json toJson() const;
};

/// 一次投递（含"投给了哪些订阅者"——ALT-SUB-03 的可验收痕迹）
struct AlertDelivery {
    json payload = json::object();           // 事件的 data
    std::string event;                       // alert.raised | alert.updated | alert.acked
    std::vector<std::string> subscriberIds;  // 命中的订阅者（按注册顺序）
    bool broadcast = false;                  // 无任何订阅者时 = 广播（宿主自决）
    int64_t ts = 0;
    json toJson() const;
};

// ============================================================================
// §7 消音（ALT-ACK-04：有时限，到期自动恢复）
// ============================================================================

struct MuteEntry {
    std::string muteId;       // `m-<seq>`（确定性）
    MuteScope scope = MuteScope::Rule;
    std::string key;          // 规则 id 或实体 id
    std::string level;        // 空 = 该范围全部级别
    int64_t from = 0;
    int64_t until = 0;        // 到期时刻（epoch ms）；到期即自动恢复
    std::string operatorId;
    std::string reason;
    bool expired = false;     // 由 `mutes()` 依据注入时钟判定
    json toJson() const;
};

struct MuteRequest {
    MuteScope scope = MuteScope::Rule;
    std::string key;
    std::string level;
    int64_t durationMs = 0;   // 与 untilMs 二选一
    int64_t untilMs = 0;
    std::string operatorId;
    std::string reason;
};

struct MuteResult {
    int code = 0;
    std::string message;
    MuteEntry mute;
    json toJson() const;
};

// ============================================================================
// §8 台账装载（ALT-RULE-03）
// ============================================================================

/// 逐条装载问题（`path` 形如 `items[3].condition.forMs` —— 与 phase-engine 的
/// `LoadIssue` 同风格）；**一次返回全部问题**，MUST NOT 遇到第一个就停（CTR-PL-02）。
struct LoadIssue {
    std::string path;
    std::string field;
    std::string reason;
};

struct LoadResult {
    int code = 0;
    std::string message;
    AlertRulesInfo data;
    std::vector<LoadIssue> issues;  // code != 0 时含**全部**问题
    json toJson() const;            // `{code, message, data, issues[]}`（issues 为只增字段）
};

// ============================================================================
// §9 反向接口（宿主 MUST 实现；引擎 MUST NOT 自带业务实现 —— P8）
// ============================================================================

/// 告警出口（protocol §6：`IAlertSink :: on<Event>(const <Payload>&)`，**立即返回**）。
///
/// 引擎把"产生 / 变更 / 确认"三个动作**逐个订阅者**投递；`d.subscriberIds` 为空且
/// `d.broadcast` 为 true 时表示"无订阅者声明，宿主自决"（未注入 Sink 时引擎照常记账）。
class IAlertSink {
public:
    virtual ~IAlertSink() = default;
    virtual void onAlertRaised(const AlertDelivery& d) = 0;   // → 广播 alert.raised
    virtual void onAlertUpdated(const AlertDelivery& d) = 0;  // → 广播 alert.updated
    virtual void onAlertAcked(const AlertDelivery& d) = 0;    // → 广播 alert.acked
};

/// 台账持久化出口（引擎不接触 SQL —— P3）。未注入 = 纯内存模式，引擎仍 MUST 可工作。
///
/// **写前提交**（与 phase-engine `D-PHE-07` 同口径）：`save()` 返回 true 之后引擎才把
/// 变更视作生效；返回 false / 抛异常 → 本次操作整体失败 `1005` 且内存状态不变。
class IAlertStore {
public:
    virtual ~IAlertStore() = default;
    virtual bool save(const AlertRecord& rec) = 0;
    virtual bool load(const std::string& alertId, AlertRecord& out) = 0;
    virtual bool remove(const std::string& alertId) = 0;
    virtual bool supportsList() const { return false; }
    virtual std::vector<AlertRecord> list(const AlertQuery&) { return {}; }
};

/// 时间注入（epoch 毫秒 —— P5）。
///
/// **口径**：`observe()` 的持续时间与去重窗口一律以**观测自带的 `ts`** 为准
/// （确定性可复现，ALT-NFR-02）；时钟只用于三类"确实需要挂钟"的判定：
/// ① 消音到期 ② 风暴保护窗口 ③ 由宿主显式驱动的 `tick(nowMs)`。
class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t nowMs() const = 0;
};

/// 可选日志出口（缺省 = 静默；MUST 立即返回）
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void log(int level, const std::string& event, const json& data) {
        (void)level;
        (void)event;
        (void)data;
    }
    virtual void commandAudit(const std::string& action, const std::string& targetId,
                              const json& detail) {
        (void)action;
        (void)targetId;
        (void)detail;
    }
};

/// 依赖注入结构：只有四个依赖 + **台账口径开关**，没有业务配置（业务全在规则包）
struct AlertEngineOptions {
    std::shared_ptr<IAlertStore> store;  // 可空 → 纯内存
    std::shared_ptr<IClock> clock;       // 可空 → SystemClock
    std::shared_ptr<IAlertSink> sink;    // 可空 → 空 Sink
    std::shared_ptr<ILogSink> log;       // 可空 → 静默

    /// ALT-NFR-07：台账有界。活动告警上限（<=0 = 不限）；超限时最旧的已关闭记录被淘汰，
    /// 淘汰**如实计入** `metrics.evicted`（MUST NOT 静默丢）。
    int maxActiveAlerts = 0;
    /// ALT-NFR-07：历史保留期（<=0 = 不限）。超过 `now - retentionMs` 的已关闭记录被淘汰。
    int64_t retentionMs = 0;
    /// ALT-GEN-05：`alert_count` 的权威口径（`deduplicated` | `raw`）。
    /// 引擎**只声明**，报告侧按它取数（D7 缺省去重后条数）。
    std::string alertCountBasis = "deduplicated";
};

// ============================================================================
// §10 自述与观测
// ============================================================================

struct Capabilities {
    bool rulesLoaded = false;
    bool pureMemory = true;
    bool storeInjected = false;
    bool clockInjected = false;
    bool sinkInjected = false;
    bool logInjected = false;
    int ruleCount = 0;
    int levelCount = 0;
    int subscriptionCount = 0;
    int stormEnabled = 0;
    int maxActiveAlerts = 0;
    int64_t retentionMs = 0;
    std::string alertCountBasis;
    std::string engineVersion = kEngineVersion;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string policiesDigest;
    json toJson() const;
};

struct Metrics {
    int64_t observations = 0;      // 收到的观测条数
    int64_t rawRaises = 0;         // 原始触发条数（口径①）
    int64_t alertsRaised = 0;      // 新开告警条数
    int64_t merged = 0;            // 并入既有告警的次数
    int64_t suppressed = 0;        // 被抑制（父告警 / 消音）
    int64_t suppressedByParent = 0;
    int64_t mutedSuppressed = 0;
    int64_t durationPending = 0;   // 持续时间条件尚未满足的次数
    int64_t flappedBlocked = 0;    // 抖动抑制拦下的次数（ALT-DEDUP-03）
    int64_t upgrades = 0;          // 级别升高次数
    int64_t recoveries = 0;        // 恢复记录条数
    int64_t autoClosed = 0;        // 自动关闭次数
    int64_t aggregates = 0;        // 风暴汇总告警条数
    int64_t folded = 0;            // 被折叠条数
    int64_t idempotentHits = 0;    // 幂等命中（CTR-EC-01）
    int64_t rejected = 0;          // 被拒次数
    int64_t notFound = 0;
    int64_t storeErrors = 0;
    int64_t sinkErrors = 0;        // Sink 抛异常被吞掉的次数（MUST NOT 影响已生效状态）
    int64_t unknownFields = 0;     // 规则包未知字段告警计数（CTR-PL-03）
    int64_t truncations = 0;       // 台账查询发生截断的次数（ALT-SUB-01）
    int64_t evicted = 0;           // 因有界策略淘汰的记录数（ALT-NFR-07，如实上报）
    int64_t clockRegressions = 0;  // 时钟回拨次数（如实使用，不伪造）
    json toJson() const;
};

// ============================================================================
// §11 引擎
// ============================================================================

/// 告警与事件引擎。
///
/// 线程安全（ALT-NFR-04）：全部公开方法可被多线程并发调用；一次 `observe()` 是**原子**的
/// （求值 → 去重 → 抑制 → 上报在同一临界区内完成）；Sink 回调在**锁外**派发
/// （宿主实现可能广播，MUST NOT 在锁内调用 —— protocol §6）。
class AlertEngine {
public:
    AlertEngine();
    explicit AlertEngine(const AlertEngineOptions& opts);

    ~AlertEngine();
    AlertEngine(const AlertEngine&) = delete;
    AlertEngine& operator=(const AlertEngine&) = delete;
    AlertEngine(AlertEngine&&) noexcept;
    AlertEngine& operator=(AlertEngine&&) noexcept;

    // ---- 规则包（protocol.md §5；ALT-RULE-01/03/04/05） ----

    /// 装载规则包（`kind:"alertRules"`）。**原子替换**：失败时保留上一次成功装载的内容。
    LoadResult loadRules(const json& pkg);
    /// 从文件装载（便利方法；读不到 / 非法 JSON → `1000` + 可读原因）
    LoadResult loadRulesFile(const std::string& path);
    /// 从目录装载 `<dir>/alertRules.json`（便利方法）
    LoadResult loadRulesFromDirectory(const std::string& dir);
    /// **纯函数**：只校验不装载（供 CI / 宿主预检）
    static LoadResult validateRules(const json& pkg);
    /// 当前生效规则（CTR-PL-06）
    AlertRulesInfo rulesInfo() const;
    /// 运行时启用 / 禁用（ALT-RULE-04：不影响已产生告警）。未知 id → `1004`。
    LoadResult setRuleEnabled(const std::string& ruleId, bool enabled);
    bool isRuleEnabled(const std::string& ruleId) const;
    /// 级别目录（按 rank 非降序）—— ALT-GEN-02 的"取值可扩展"出口
    std::vector<AlertLevelDef> levels() const;
    /// `rank` 比较（升级判定）；未知级别 → `nullopt`（MUST NOT 猜）
    std::optional<int> levelRank(const std::string& level) const;

    // ---- 产生（ALT-GEN / ALT-DEDUP） ----

    /// 唯一产生入口：**结构化观测**（ALT-GEN-01）。返回三态结果，**不抛异常**。
    AlertResult observe(const Observation& obs);
    /// 便利重载：`{ruleId, entityId, missionId, metric, value, ts}`（未知字段忽略）
    AlertResult observe(const json& observation);
    /// 由宿主驱动时间前进：触发消音到期、风暴窗口滑动与持续时间判定（**不产生观测**）。
    /// 返回本次到期消音的条数 + 被自动关闭的条数（可读结果在 `data` 内）。
    json tick(int64_t nowMs);

    // ---- 生命周期（ALT-ACK） ----

    /// 确认（ALT-ACK-02：**幂等** —— 重复确认返回 `code=0` + `idempotent=true`，MUST NOT 报错）
    ActionResult acknowledge(const std::string& alertId, const std::string& operatorId = "",
                             const std::string& reason = "");
    /// 关闭（`active`/`acknowledged` → `closed`）
    ActionResult close(const std::string& alertId, const std::string& operatorId = "",
                       const std::string& reason = "");
    /// 重开（`closed` → `active`；未关闭 → 幂等命中）
    ActionResult reopen(const std::string& alertId, const std::string& operatorId = "",
                        const std::string& reason = "");

    // ---- 消音（ALT-ACK-04） ----

    /// 新增消音（有时限）。`durationMs` 与 `untilMs` 都未给 → 用规则包的 `mute.defaultMs`。
    MuteResult mute(const MuteRequest& req);
    /// 取消消音
    MuteResult unmute(const std::string& muteId, const std::string& operatorId = "");
    /// 当前消音（含到期判定；`expired=true` 的项已**自动恢复**，不再拦截告警）
    std::vector<MuteEntry> mutes() const;

    // ---- 台账查询（ALT-SUB-01 / ALT-SUB-02 / ALT-SUB-05） ----

    /// 按级别 / 实体 / 时间区间 / 状态 / 任务 / 规则筛选；分页 + **超限截断（不静默丢）**
    AlertQueryResult listAlerts(const AlertQuery& q = {}) const;
    /// 单条台账（不存在 → `nullopt`，MUST NOT 返回空对象当真值 —— protocol §2.3）
    std::optional<AlertRecord> getAlert(const std::string& alertId) const;
    /// ALT-SUB-05：一次调用返回全部活动告警（新客户端接入用，不是逐条推送）
    json activeAlerts() const;
    /// ALT-GEN-05：计数口径（两种口径同时在案，可复算）
    AlertCounts counts() const;

    // ---- 订阅（ALT-SUB-03） ----

    /// 注册 / 覆盖订阅者。`subscriberId` 非法（空 / 不以小写字母开头）→ `1000`
    ActionResult subscribe(const AlertSubscription& sub);
    bool unsubscribe(const std::string& subscriberId);
    std::vector<AlertSubscription> subscriptions() const;

    // ---- 依赖注入（P8/P9） ----

    void setStore(std::shared_ptr<IAlertStore> store);
    void setClock(std::shared_ptr<IClock> clock);
    void setSink(std::shared_ptr<IAlertSink> sink);
    void setLog(std::shared_ptr<ILogSink> log);

    // ---- 自述与观测 ----

    Capabilities capabilities() const;  // 永远可调用
    Metrics metrics() const;            // 永远可调用
    void resetMetrics();

    /// ● 码的稳定短名；未知码 → "unknown"
    static const char* errorCodeName(int code);

    // PIMPL：公开头保持稳定，实现细节（src/internal.h）不外泄。
    struct Impl;

    /// 状态迁移的统一实现（确认 / 关闭 / 重开共用同一套裁决，避免三套口径漂移）
    friend ActionResult detailApplyTransition(AlertEngine& engine, const std::string& alertId,
                                              AlertState target, const std::string& action,
                                              const std::string& operatorId,
                                              const std::string& reason);

private:
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// ● 自由函数（纯函数，供 CI / 宿主预检与自测锁定口径）
// ============================================================================

/// ● 规则包校验（等价于 AlertEngine::validateRules）
LoadResult validateRules(const json& pkg);
/// ● 错误码短名
const char* errorCodeName(int code);
/// ● FNV-1a 64 位摘要（16 位小写十六进制；同一份规则包字节 → 同一 digest）
std::string policiesDigest(const json& pkg);

/// ● 条件求值（**纯函数**，不依赖引擎状态、不涉及持续时间）
///
/// `nowMs` 仅用于把 `Duration` 叶当作"瞬时成立"处理（自测与宿主预检用）；
/// 引擎内的持续时间由 `observe()` 按观测时间戳累计（ALT-RULE-02）。
/// 返回：`true` 满足。`missing` 输出本次求值是否命中"字段缺失"。
bool evaluateCondition(const Condition& c, const Observation& obs, bool* missing = nullptr);

/// ● 枚举 ⇄ JSON 字符串（取值冻结；非法取值 → 空串 / nullopt，MUST NOT 猜）
const char* toString(ResultStatus s);
const char* toString(AlertState s);
const char* toString(ConditionType t);
const char* toString(CompareOp op);
const char* toString(MergeMode m);
const char* toString(RecoveryMode m);
const char* toString(MuteScope s);
std::optional<AlertState> alertStateFromString(const std::string& s);
std::optional<CompareOp> compareOpFromString(const std::string& s);
std::optional<MergeMode> mergeModeFromString(const std::string& s);
std::optional<RecoveryMode> recoveryModeFromString(const std::string& s);
std::optional<ConditionType> conditionTypeFromString(const std::string& s);
std::optional<MuteScope> muteScopeFromString(const std::string& s);

/// 内置系统时钟：引擎唯一的非确定性来源（仅在 IClock 未注入时使用）
class SystemClock : public IClock {
public:
    int64_t nowMs() const override;
};

}  // namespace alert_engine
