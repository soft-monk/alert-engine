// src/engine.cc —— 告警引擎：规则装载 / 观测求值 / 产生与分级 / 去重合并 / 抖动抑制 /
//                  升级 / 父子抑制 / 风暴保护 / 确认状态机 / 消音 / 台账查询 / 订阅分发
//
// 需求依据：ALT-RULE-01..05 / ALT-GEN-01..06 / ALT-DEDUP-01..06 / ALT-ACK-01..05 /
//           ALT-SUB-01..06 / ALT-NFR-01..07
// 契约依据：protocol.md §3.2（码表，1001 保留不用）/ §3.3（幂等 = code 0 + idempotent）/
//           §4.4（alert.raised / alert.updated / alert.acked）/ §5（规则包）/ §6（反向接口）；
//           冲突裁决 C15 / C16 / C17
//
// 七条结构性口径：
//   1. **一次观测一次裁决**：求值 → 恢复 → 抖动 → 抑制 → 去重/风暴 → 上报，全程在一个
//      临界区内完成（ALT-NFR-04：多线程不丢不重）。
//   2. **时间可注入**：持续时间与去重窗口一律用**观测自带的 ts**；时钟只用于消音到期、
//      风暴窗口与 `tick()`（ALT-NFR-02/03 的确定性前提）。
//   3. **写前提交**：`IAlertStore::save()` 成功后才改内存并通知 Sink（同 phase-engine D-PHE-07）。
//   4. **通知在锁外**：Sink 由宿主实现（可能广播），MUST NOT 在锁内调用（protocol §6）。
//   5. **台账查询超限截断但不静默丢**：`truncated` / `omitted` 如实上报（ALT-SUB-01）。
//   6. **计数口径显式**：`counts.rawRaises == counts.alertsRaised + counts.merged`（ALT-GEN-05）。
//   7. **台账有界**：保留期 + 活动上限可配，淘汰**如实计入** `metrics.evicted`（ALT-NFR-07）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "internal.h"

namespace alert_engine {

using detail::CompiledCondition;
using detail::ConditionState;
using detail::EngineCore;
using detail::MonitorKey;
using detail::MonitorState;
using detail::RuleRuntime;
using EngineImpl = AlertEngine::Impl;

// 结果类型与事件类型都在本命名空间（公开头声明）；显式 using 以避免 MSVC 在
// 成员函数体内把未加限定的名字解析到 detail 里去。
using alert_engine::AlertDelivery;
using alert_engine::LevelChange;
using alert_engine::MergeEntry;
using alert_engine::StateChange;

namespace {

// ============================================================================
// 宿主回调的安全包装（P10：MUST NOT 抛异常跨边界）
// ============================================================================

void safeRaised(const std::shared_ptr<IAlertSink>& sink, const AlertDelivery& d, int64_t& errors) {
    if (!sink) return;
    try {
        sink->onAlertRaised(d);
    } catch (...) {
        ++errors;
    }
}

void safeUpdated(const std::shared_ptr<IAlertSink>& sink, const AlertDelivery& d, int64_t& errors) {
    if (!sink) return;
    try {
        sink->onAlertUpdated(d);
    } catch (...) {
        ++errors;
    }
}

void safeAcked(const std::shared_ptr<IAlertSink>& sink, const AlertDelivery& d, int64_t& errors) {
    if (!sink) return;
    try {
        sink->onAlertAcked(d);
    } catch (...) {
        ++errors;
    }
}

void safeLog(const std::shared_ptr<ILogSink>& log, int level, const std::string& event,
             const json& data) {
    if (!log) return;
    try {
        log->log(level, event, data);
    } catch (...) {
    }
}

void safeAudit(const std::shared_ptr<ILogSink>& log, const std::string& action,
               const std::string& target, const json& detail) {
    if (!log) return;
    try {
        log->commandAudit(action, target, detail);
    } catch (...) {
    }
}

/// 写前提交：未注入 store = 纯内存（恒成功）；save 返回 false / 抛异常 → 本次操作整体失败
bool commitRecord(EngineCore& c, const AlertRecord& rec) {
    if (!c.store) return true;
    try {
        if (!c.store->save(rec)) {
            ++c.metrics.storeErrors;
            return false;
        }
        return true;
    } catch (...) {
        ++c.metrics.storeErrors;
        return false;
    }
}

int64_t readClock(const EngineCore& c) {
    if (!c.clock) {
        SystemClock sc;
        return sc.nowMs();
    }
    return c.clock->nowMs();
}

/// 读取时钟并记账回拨（如实使用，不伪造 —— 与 phase-engine CTR-PHE-CK-03 同口径）
int64_t readClockObserved(EngineCore& c) {
    const int64_t now = readClock(c);
    if (c.lastKnownNow != 0 && now < c.lastKnownNow) ++c.metrics.clockRegressions;
    if (now > c.lastKnownNow) c.lastKnownNow = now;
    return now;
}

/// 观测 → 领域对象（宽松解析：未知字段忽略，缺失字段取缺省 —— 对齐 CTR-PL-03 的前向兼容口径）
Observation observationFromJson(const json& j) {
    Observation o;
    o.ruleId = j.value("ruleId", std::string());
    if (o.ruleId.empty()) o.ruleId = j.value("key", std::string());
    o.entityId = j.value("entityId", std::string());
    o.missionId = j.value("missionId", std::string());
    o.metric = j.value("metric", std::string());
    o.value = j.value("value", 0.0);
    o.ts = j.value("ts", static_cast<int64_t>(0));
    o.contextJson = j.value("contextJson", std::string());
    if (j.contains("context") && j["context"].is_object()) {
        for (auto it = j["context"].begin(); it != j["context"].end(); ++it) {
            if (it.value().is_number()) {
                o.context[it.key()] = it.value().get<double>();
            } else if (it.value().is_boolean()) {
                o.context[it.key()] = it.value().get<bool>() ? 1.0 : 0.0;
            }
        }
    }
    if (j.contains("values") && j["values"].is_object()) {
        for (auto it = j["values"].begin(); it != j["values"].end(); ++it) {
            if (it.value().is_number()) o.values[it.key()] = it.value().get<double>();
        }
    }
    return o;
}

/// 命中该（规则+实体）的父规则（其告警当前未关闭）？返回父规则 id（空 = 无抑制）
std::string findActiveParent(EngineCore& c, const RuleRuntime& rt, const MonitorKey& key) {
    std::vector<std::string> candidates;
    if (!rt.def.parent.empty()) candidates.push_back(rt.def.parent);
    for (const auto& kv : c.suppressedBy) {
        for (const auto& child : kv.second) {
            if (child == rt.def.id) {
                candidates.push_back(kv.first);
                break;
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());  // 声明顺序无关 → 确定性
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const auto& pid : candidates) {
        auto it = c.dedup.openAlertId.find(MonitorKey{pid, key.entityId});
        if (it == c.dedup.openAlertId.end()) continue;
        const AlertRecord* parent = c.findAlert(it->second);
        if (parent != nullptr && parent->active && parent->kind == "alert") return pid;
    }
    return std::string();
}

/// 消音命中（ALT-ACK-04）。已到期的项由 `reapMutes` 先行清除 → 到期即自动恢复。
bool mutedBy(const EngineCore& c, const std::string& ruleId, const std::string& entityId,
             const std::string& level, int64_t nowMs, int64_t* untilOut) {
    for (const auto& kv : c.mutes) {
        const MuteEntry& m = kv.second;
        if (nowMs >= m.until) continue;  // 已到期
        if (!m.level.empty() && m.level != level) continue;
        if (m.scope == MuteScope::Rule && m.key != ruleId) continue;
        if (m.scope == MuteScope::Entity) {
            if (entityId.empty() || m.key != entityId) continue;
        }
        if (untilOut != nullptr) *untilOut = m.until;
        return true;
    }
    return false;
}

/// 消音到期清理；返回本次到期条数
int reapMutes(EngineCore& c, int64_t nowMs) {
    int n = 0;
    for (auto it = c.mutes.begin(); it != c.mutes.end();) {
        if (nowMs >= it->second.until) {
            it = c.mutes.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

/// 台账有界（ALT-NFR-07）：保留期 + 活动上限；淘汰如实计入 `metrics.evicted`。
/// **节流**：只有配了有界策略且距上次清扫超过一个保留期（或 1 秒）才真正扫一遍，
/// 使 1000 条/秒的压力下不引入额外开销（ALT-NFR-06）。
void housekeep(EngineCore& c, int64_t nowMs) {
    if (c.retentionMs <= 0 && c.maxActiveAlerts <= 0) return;
    const int64_t interval = c.retentionMs > 0 ? std::min<int64_t>(c.retentionMs, 1000) : 1000;
    if (c.lastHousekeep != 0 && nowMs - c.lastHousekeep < interval) return;
    c.lastHousekeep = nowMs;

    std::vector<std::string> evict;
    if (c.retentionMs > 0) {
        for (const auto& kv : c.alerts) {
            if (kv.second.active) continue;
            if (nowMs - kv.second.lastAt > c.retentionMs) evict.push_back(kv.first);
        }
    }
    if (c.maxActiveAlerts > 0) {
        int active = 0;
        for (const auto& kv : c.alerts) {
            if (kv.second.active) ++active;
        }
        int excess = active - c.maxActiveAlerts;
        for (const auto& kv : c.alerts) {  // 有序 map ⇒ 按产生顺序淘汰最旧
            if (excess <= 0) break;
            if (!kv.second.active) continue;
            if (std::find(evict.begin(), evict.end(), kv.first) != evict.end()) continue;
            evict.push_back(kv.first);
            --excess;
        }
    }
    for (const auto& id : evict) {
        auto it = c.alerts.find(id);
        if (it == c.alerts.end()) continue;
        const MonitorKey key{it->second.ruleId, it->second.entityId};
        auto dit = c.dedup.openAlertId.find(key);
        if (dit != c.dedup.openAlertId.end() && dit->second == id) c.dedup.openAlertId.erase(dit);
        c.alerts.erase(it);
        ++c.metrics.evicted;
    }
    // 监控状态同样有界（ALT-NFR-07）：淘汰长期没有观测的（规则+实体）
    if (c.retentionMs > 0) c.pruneMonitors(nowMs - c.retentionMs);
}

/// 构造投递：按订阅过滤（ALT-SUB-03；空集合 = 全部），Sink 在**锁外**调用
AlertDelivery buildDelivery(EngineCore& c, const AlertRecord& r, const std::string& event,
                            int64_t ts) {
    AlertDelivery d;
    d.event = event;
    d.ts = ts;
    for (const auto& kv : c.subscriptions) {
        if (detail::subscriptionMatches(kv.second, r)) d.subscriberIds.push_back(kv.first);
    }
    d.broadcast = d.subscriberIds.empty();  // 无订阅者声明 → 广播（宿主自决）
    return d;
}

/// 级别升高判定（ALT-DEDUP-04）：按规则包 `levels[].rank` 比较（引擎 MUST NOT 内建级别序）
bool isUpgrade(const EngineCore& c, const std::string& from, const std::string& to) {
    auto a = c.levelRank.find(from);
    auto b = c.levelRank.find(to);
    if (a == c.levelRank.end() || b == c.levelRank.end()) return false;
    return b->second > a->second;
}

/// 模板填充（引擎只做**模板填充**，不产出自然语言 —— D4 / ALT-GEN-01）
std::string renderTitle(const std::string& tpl, const Observation& obs, const std::string& level) {
    char v[64];
    std::map<std::string, std::string> vars;
    vars["ruleId"] = obs.ruleId;
    vars["entityId"] = obs.entityId;
    vars["missionId"] = obs.missionId;
    vars["metric"] = obs.metric;
    vars["level"] = level;
    std::snprintf(v, sizeof(v), "%g", obs.value);
    vars["value"] = v;
    std::snprintf(v, sizeof(v), "%lld", static_cast<long long>(obs.ts));
    vars["ts"] = v;
    return detail::renderTemplate(tpl, vars);
}

/// 窗口内合并（ALT-GEN-03 / ALT-DEDUP-02/04）：计数累加、最近值与最近时间更新，
/// **首次时间与原始级别 MUST NOT 被改动**（升级除外的级别走 levelHistory 追加）。
AlertRecord mergeInto(EngineCore& c, const AlertRecord& base, const Observation& obs,
                      const RuleRuntime& rt, alert_engine::MergeEntry& me) {
    AlertRecord merged = base;
    me.at = obs.ts;
    me.countAdded = 1;
    me.value = obs.value;
    me.hasValue = true;
    const std::string fromLevel = merged.level;
    const std::string toLevel = rt.def.level;  // 合并沿用规则当前的级别声明
    if (isUpgrade(c, fromLevel, toLevel)) {
        merged.level = toLevel;
        me.upgraded = true;
        me.fromLevel = fromLevel;
        me.toLevel = toLevel;
        alert_engine::LevelChange lc;
        lc.at = obs.ts;
        lc.level = toLevel;
        lc.reason = "upgraded";
        merged.levelHistory.push_back(lc);
        ++c.metrics.upgrades;
    }
    switch (rt.def.merge) {
        case MergeMode::Accumulate:
            merged.count += 1;
            merged.lastValue = obs.value;
            break;
        case MergeMode::Latest:
            merged.lastValue = obs.value;
            break;
        case MergeMode::Max:
            merged.lastValue = std::max(merged.lastValue, obs.value);
            break;
        case MergeMode::Keep:
            break;
    }
    merged.lastAt = obs.ts;
    merged.conditionHeld = true;  // 合并本身即"条件成立"
    merged.mergeHistory.push_back(me);
    return merged;
}

// ============================================================================
// 观测裁决的骨架（各分支填 outcome；deliveries 在锁外派发）
// ============================================================================

struct ObserveOutcome {
    AlertResult result;
    std::vector<std::pair<std::string, AlertDelivery>> deliveries;
};

/// 恢复 / 自动关闭（ALT-GEN-04 / ALT-ACK-05）
void handleRecovery(EngineCore& c, const Observation& obs, RuleRuntime& rt, const MonitorKey& key,
                    const AlertRecord& openRec, ObserveOutcome& o) {
    AlertRecord rec = openRec;
    if (rt.def.recoverOn == RecoveryMode::Close) {
        const std::string fromState = toString(rec.state);
        rec.state = AlertState::Closed;
        rec.active = false;
        rec.closedAt = obs.ts;
        rec.closeReason = "condition-recovered";
        alert_engine::StateChange sc;
        sc.at = obs.ts;
        sc.fromState = fromState;
        sc.toState = "closed";
        sc.action = "auto-close";
        sc.reason = "condition-recovered";
        rec.stateHistory.push_back(sc);
        if (!commitRecord(c, rec)) {
            ++c.metrics.rejected;
            o.result.code = 1005;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "台账写入失败（写前提交），内存状态未变";
            return;
        }
        c.alerts[rec.alertId] = rec;
        c.dedup.openAlertId.erase(key);
        c.dedup.closedAlertId[key] = rec.alertId;
        ++c.metrics.autoClosed;
        AlertDelivery d = buildDelivery(c, rec, "alert.acked", obs.ts);
        d.payload = toJson(detail::ackedEventOf(rec, sc, false));
        o.deliveries.emplace_back("alert.acked", std::move(d));
        o.result.code = 0;
        o.result.status = ResultStatus::Ok;
        o.result.recovered = true;
        o.result.message = "条件恢复：按规则自动关闭";
        o.result.alert = rec;
        return;
    }
    if (rt.def.recoverOn == RecoveryMode::Record) {
        AlertRecord recoveryRec;
        recoveryRec.alertId = c.nextAlertId();
        recoveryRec.ruleId = obs.ruleId;
        recoveryRec.ruleVersion = rt.def.version;
        recoveryRec.kind = "recovery";
        recoveryRec.level = rec.level;
        recoveryRec.originLevel = rec.level;
        recoveryRec.entityId = obs.entityId;
        recoveryRec.missionId = rec.missionId;
        recoveryRec.metric = obs.metric;
        recoveryRec.firstValue = obs.value;
        recoveryRec.lastValue = obs.value;
        recoveryRec.firstAt = obs.ts;
        recoveryRec.lastAt = obs.ts;
        recoveryRec.count = 1;
        recoveryRec.windowStart = obs.ts;
        recoveryRec.active = true;
        recoveryRec.linkedAlertId = rec.alertId;  // ALT-GEN-04：可与原告警关联
        recoveryRec.title = renderTitle(rt.titleTemplate, obs, rec.level);
        alert_engine::LevelChange lc;
        lc.at = obs.ts;
        lc.level = recoveryRec.level;
        lc.reason = "created";
        recoveryRec.levelHistory.push_back(lc);
        alert_engine::StateChange sc;
        sc.at = obs.ts;
        sc.toState = "active";
        sc.action = "create";
        sc.reason = "recovered";
        recoveryRec.stateHistory.push_back(sc);
        if (!commitRecord(c, recoveryRec)) {
            ++c.metrics.rejected;
            o.result.code = 1005;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "台账写入失败（写前提交），内存状态未变";
            return;
        }
        AlertRecord closed = rec;
        const std::string fromState = toString(closed.state);
        closed.state = AlertState::Closed;
        closed.active = false;
        closed.closedAt = obs.ts;
        closed.closeReason = "recovered";
        alert_engine::StateChange sc2;
        sc2.at = obs.ts;
        sc2.fromState = fromState;
        sc2.toState = "closed";
        sc2.action = "auto-close";
        sc2.reason = "recovered";
        closed.stateHistory.push_back(sc2);
        if (!commitRecord(c, closed)) {
            ++c.metrics.rejected;
            o.result.code = 1005;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "台账写入失败（写前提交），内存状态未变";
            return;
        }
        c.alerts[closed.alertId] = closed;
        c.alerts[recoveryRec.alertId] = recoveryRec;
        c.dedup.openAlertId.erase(key);
        c.dedup.closedAlertId[key] = recoveryRec.alertId;
        ++c.metrics.recoveries;
        ++c.metrics.autoClosed;
        ++c.metrics.rawRaises;
        ++c.counts.recoveries;
        ++c.counts.rawRaises;
        AlertDelivery d1 = buildDelivery(c, recoveryRec, "alert.raised", obs.ts);
        d1.payload = toJson(detail::raisedEventOf(recoveryRec));
        o.deliveries.emplace_back("alert.raised", std::move(d1));
        AlertDelivery d2 = buildDelivery(c, closed, "alert.acked", obs.ts);
        d2.payload = toJson(detail::ackedEventOf(closed, sc2, false));
        o.deliveries.emplace_back("alert.acked", std::move(d2));
        o.result.code = 0;
        o.result.status = ResultStatus::Ok;
        o.result.recovered = true;
        o.result.message = "条件恢复：产生恢复记录并关联原告警";
        o.result.alert = recoveryRec;
        return;
    }
    // RecoveryMode::None：不产生恢复记录、不自动关闭（**按规则声明的行为**）
    o.result.code = 0;
    o.result.status = ResultStatus::Ok;
    o.result.message = "条件已不再满足（规则未声明恢复动作）";
    o.result.alert = rec;
    ++c.metrics.flappedBlocked;
}

/// 风暴保护（ALT-DEDUP-06）：窗口内新开条数超上限 → 汇总告警 + 如实上报被折叠条数
void raiseAggregate(EngineCore& c, const Observation& obs, const std::string& mission,
                    ObserveOutcome& o) {
    const MonitorKey aggKey{kAggregateRuleId, std::string()};
    auto aggIt = c.dedup.openAlertId.find(aggKey);
    AlertRecord* existing = aggIt == c.dedup.openAlertId.end() ? nullptr : c.findAlert(aggIt->second);
    const bool aggOpen = existing != nullptr && existing->active;

    std::map<std::string, std::string> vars;
    vars["ruleId"] = obs.ruleId;
    vars["entityId"] = obs.entityId;
    vars["metric"] = obs.metric;
    vars["windowMs"] = std::to_string(c.storm.policy.windowMs);
    vars["maxRaised"] = std::to_string(c.storm.policy.maxRaised);

    if (aggOpen) {
        AlertRecord merged = *existing;
        merged.count += 1;
        merged.foldedCount += 1;  // 如实累计被折叠条数
        merged.lastAt = obs.ts;
        merged.lastValue = obs.value;
        vars["folded"] = std::to_string(merged.foldedCount);
        merged.title = detail::renderTemplate(c.storm.policy.title, vars);
        alert_engine::MergeEntry me;
        me.at = obs.ts;
        me.countAdded = 1;
        me.value = obs.value;
        me.hasValue = true;
        merged.mergeHistory.push_back(me);
        if (!commitRecord(c, merged)) {
            ++c.metrics.rejected;
            o.result.code = 1005;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "台账写入失败（写前提交），内存状态未变";
            return;
        }
        c.alerts[merged.alertId] = merged;
        ++c.metrics.folded;
        ++c.metrics.rawRaises;
        ++c.counts.folded;
        ++c.counts.rawRaises;
        AlertDelivery d = buildDelivery(c, merged, "alert.updated", obs.ts);
        d.payload = toJson(detail::updatedEventOf(merged, me));
        o.deliveries.emplace_back("alert.updated", std::move(d));
        o.result.code = 0;
        o.result.status = ResultStatus::AlreadyApplied;
        o.result.folded = true;
        o.result.message = "风暴保护：已折叠进汇总告警（foldedCount=" +
                           std::to_string(merged.foldedCount) + "）";
        o.result.alert = merged;
        return;
    }

    AlertRecord agg;
    agg.alertId = c.nextAlertId();
    agg.ruleId = kAggregateRuleId;
    agg.kind = "aggregate";
    agg.level = c.storm.policy.level;
    agg.originLevel = c.storm.policy.level;
    agg.missionId = mission;
    agg.metric = obs.metric;
    agg.firstValue = obs.value;
    agg.lastValue = obs.value;
    agg.firstAt = obs.ts;
    agg.lastAt = obs.ts;
    agg.windowStart = obs.ts;
    agg.count = 1;
    agg.foldedCount = 1;
    agg.active = true;
    vars["folded"] = "1";
    agg.title = detail::renderTemplate(c.storm.policy.title, vars);
    alert_engine::LevelChange lc;
    lc.at = obs.ts;
    lc.level = agg.level;
    lc.reason = "created";
    agg.levelHistory.push_back(lc);
    alert_engine::StateChange sc;
    sc.at = obs.ts;
    sc.toState = "active";
    sc.action = "create";
    sc.reason = "storm-fold";
    agg.stateHistory.push_back(sc);
    if (!commitRecord(c, agg)) {
        ++c.metrics.rejected;
        o.result.code = 1005;
        o.result.status = ResultStatus::Rejected;
        o.result.message = "台账写入失败（写前提交），内存状态未变";
        return;
    }
    c.alerts[agg.alertId] = agg;
    c.dedup.openAlertId[aggKey] = agg.alertId;
    ++c.metrics.folded;
    ++c.metrics.aggregates;
    ++c.metrics.rawRaises;
    ++c.counts.folded;
    ++c.counts.aggregates;
    ++c.counts.rawRaises;
    AlertDelivery d = buildDelivery(c, agg, "alert.raised", obs.ts);
    d.payload = toJson(detail::raisedEventOf(agg));
    o.deliveries.emplace_back("alert.raised", std::move(d));
    o.result.code = 0;
    o.result.status = ResultStatus::Ok;
    o.result.raised = true;
    o.result.folded = true;
    o.result.message = "风暴保护：产生汇总告警（foldedCount=1）";
    o.result.alert = agg;
}

/// 新开一条业务告警
void raiseNew(EngineCore& c, const Observation& obs, RuleRuntime& rt, const MonitorKey& key,
              const std::string& mission, const std::string& parentRule, ObserveOutcome& o) {
    AlertRecord rec;
    rec.alertId = c.nextAlertId();
    rec.ruleId = obs.ruleId;
    rec.ruleVersion = rt.def.version;
    rec.kind = "alert";
    rec.level = rt.def.level;
    rec.originLevel = rt.def.level;
    rec.entityId = obs.entityId;
    rec.missionId = mission;
    rec.metric = obs.metric;
    rec.firstValue = obs.value;
    rec.lastValue = obs.value;
    rec.firstAt = obs.ts;
    rec.lastAt = obs.ts;
    rec.count = 1;
    rec.windowStart = obs.ts;
    rec.active = true;
    rec.parentRuleId = parentRule;
    rec.conditionHeld = true;  // 产生即"条件成立"
    rec.title = renderTitle(rt.titleTemplate, obs, rt.def.level);
    alert_engine::LevelChange lc;
    lc.at = obs.ts;
    lc.level = rec.level;
    lc.reason = "created";
    rec.levelHistory.push_back(lc);
    alert_engine::StateChange sc;
    sc.at = obs.ts;
    sc.toState = "active";
    sc.action = "create";
    sc.reason = "triggered";
    rec.stateHistory.push_back(sc);
    if (!commitRecord(c, rec)) {
        ++c.metrics.rejected;
        o.result.code = 1005;
        o.result.status = ResultStatus::Rejected;
        o.result.message = "台账写入失败（写前提交），内存状态未变";
        return;
    }
    c.alerts[rec.alertId] = rec;
    c.dedup.openAlertId[key] = rec.alertId;
    c.dedup.closedAlertId.erase(key);
    ++c.counts.alertsRaised;
    ++c.counts.rawRaises;
    ++c.metrics.alertsRaised;
    ++c.metrics.rawRaises;
    AlertDelivery d = buildDelivery(c, rec, "alert.raised", obs.ts);
    d.payload = toJson(detail::raisedEventOf(rec));
    o.deliveries.emplace_back("alert.raised", std::move(d));
    o.result.code = 0;
    o.result.status = ResultStatus::Ok;
    o.result.raised = true;
    o.result.message = "告警已产生";
    o.result.alert = rec;
}

}  // namespace

// ============================================================================
// 构造 / 注入
// ============================================================================

AlertEngine::AlertEngine() : impl_(std::make_unique<Impl>()) {}

AlertEngine::AlertEngine(const AlertEngineOptions& opts) : impl_(std::make_unique<Impl>()) {
    EngineCore& c = *impl_;
    c.store = opts.store;
    c.clock = opts.clock;
    c.sink = opts.sink;
    c.log = opts.log;
    c.storeInjectedFlag = (opts.store != nullptr);
    c.clockInjectedFlag = (opts.clock != nullptr);
    c.sinkInjectedFlag = (opts.sink != nullptr);
    c.logInjectedFlag = (opts.log != nullptr);
    c.maxActiveAlerts = opts.maxActiveAlerts;
    c.retentionMs = opts.retentionMs;
    c.counts.basis = opts.alertCountBasis.empty() ? "deduplicated" : opts.alertCountBasis;
}

AlertEngine::~AlertEngine() = default;
AlertEngine::AlertEngine(AlertEngine&&) noexcept = default;
AlertEngine& AlertEngine::operator=(AlertEngine&&) noexcept = default;

void AlertEngine::setStore(std::shared_ptr<IAlertStore> store) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->store = std::move(store);
    impl_->storeInjectedFlag = (impl_->store != nullptr);
}

void AlertEngine::setClock(std::shared_ptr<IClock> clock) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->clock = std::move(clock);
    impl_->clockInjectedFlag = (impl_->clock != nullptr);
}

void AlertEngine::setSink(std::shared_ptr<IAlertSink> sink) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->sink = std::move(sink);
    impl_->sinkInjectedFlag = (impl_->sink != nullptr);
}

void AlertEngine::setLog(std::shared_ptr<ILogSink> log) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->log = std::move(log);
    impl_->logInjectedFlag = (impl_->log != nullptr);
}

// ============================================================================
// 规则包装载（protocol.md §5；ALT-RULE-01/03/04/05）
// ============================================================================

LoadResult validateRules(const json& pkg) { return detail::compileRules(pkg).result; }

LoadResult AlertEngine::validateRules(const json& pkg) { return alert_engine::validateRules(pkg); }

LoadResult AlertEngine::loadRules(const json& pkg) {
    const detail::CompiledRules compiled = detail::compileRules(pkg);
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    if (!compiled.ok) {
        ++c.metrics.rejected;
        return compiled.result;  // **原子替换**：失败时保留上一次成功装载的内容
    }
    c.loaded = true;
    c.policiesNamespace = compiled.policiesNamespace;
    c.schemaVersion = compiled.schemaVersion;
    c.digest = compiled.digest;
    c.levels = compiled.levels;
    c.levelRank = compiled.levelRank;
    c.rules.clear();
    c.ruleOrder.clear();
    for (const auto& r : compiled.rules) {
        c.ruleOrder.push_back(r.def.id);
        c.rules[r.def.id] = r;
    }
    c.suppressedBy = compiled.suppressedBy;
    c.storm = compiled.storm;
    c.mutePolicy = compiled.mutePolicy;
    c.warnings = compiled.warnings;
    c.info = compiled.info;
    c.metrics.unknownFields += static_cast<int64_t>(compiled.warnings.size());
    c.loadedPack = pkg;
    return compiled.result;
}

LoadResult AlertEngine::loadRulesFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.code = 1000;
        r.message = "规则包文件读不到：" + path;
        r.issues.push_back(detail::makeIssue(path, "", "文件不存在或不可读"));
        std::lock_guard<std::mutex> lk(impl_->mutex);
        ++impl_->metrics.rejected;
        return r;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json pkg;
    try {
        pkg = json::parse(text);
    } catch (const std::exception& ex) {
        LoadResult r;
        r.code = 1000;
        r.message = std::string("规则包不是合法 JSON：") + ex.what();
        r.issues.push_back(detail::makeIssue(path, "", "JSON 解析失败"));
        std::lock_guard<std::mutex> lk(impl_->mutex);
        ++impl_->metrics.rejected;
        return r;
    }
    return loadRules(pkg);
}

LoadResult AlertEngine::loadRulesFromDirectory(const std::string& dir) {
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    return loadRulesFile(base + "alertRules.json");
}

AlertRulesInfo AlertEngine::rulesInfo() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    AlertRulesInfo info = impl_->info;
    info.loaded = impl_->loaded;
    info.disabledCount = 0;
    for (const auto& kv : impl_->rules) {
        if (!kv.second.enabled) ++info.disabledCount;
    }
    return info;
}

LoadResult AlertEngine::setRuleEnabled(const std::string& ruleId, bool enabled) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    auto it = c.rules.find(ruleId);
    if (it == c.rules.end()) {
        ++c.metrics.notFound;
        LoadResult r;
        r.code = 1004;
        r.message = "未知规则 id：" + ruleId;
        r.issues.push_back(detail::makeIssue("items[" + ruleId + "]", "key", "规则不存在"));
        r.data = c.info;
        return r;
    }
    // ALT-RULE-04：只切换产生开关 —— **已产生告警一字不动**，历史仍可查。
    it->second.enabled = enabled;
    it->second.def.enabled = enabled;
    LoadResult r;
    r.code = 0;
    r.message = enabled ? "规则已启用" : "规则已禁用";
    r.data = c.info;
    r.data.disabledCount = 0;
    for (const auto& kv : c.rules) {
        if (!kv.second.enabled) ++r.data.disabledCount;
    }
    return r;
}

bool AlertEngine::isRuleEnabled(const std::string& ruleId) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    const RuleRuntime* r = impl_->findRule(ruleId);
    return r != nullptr && r->enabled;
}

std::vector<AlertLevelDef> AlertEngine::levels() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->levels;
}

std::optional<int> AlertEngine::levelRank(const std::string& level) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->levelRank.find(level);
    if (it == impl_->levelRank.end()) return std::nullopt;
    return it->second;
}

// ============================================================================
// 计数口径刷新（ALT-GEN-05：口径显式且可复算）
// ============================================================================

namespace {

/// 台账记录数与活动条数（可从台账**复算**，不依赖任何隐藏累加器）
void refreshCountsLocked(EngineCore& c) {
    AlertCounts& n = c.counts;
    n.alertCount = static_cast<int64_t>(c.alerts.size());
    n.openActive = 0;
    for (const auto& kv : c.alerts) {
        if (kv.second.active) ++n.openActive;
    }
}

}  // namespace

// ============================================================================
// 观测 → 告警（ALT-GEN / ALT-DEDUP）
// ============================================================================

AlertResult AlertEngine::observe(const json& observation) {
    return observe(observationFromJson(observation));
}

AlertResult AlertEngine::observe(const Observation& obs) {
    EngineCore& c = *impl_;
    ObserveOutcome o;
    o.result.ruleId = obs.ruleId;

    {
        std::lock_guard<std::mutex> lk(c.mutex);
        ++c.metrics.observations;

        // ---- 0) 入参与规则存在性 ----
        if (obs.ruleId.empty()) {
            ++c.metrics.rejected;
            o.result.code = 1000;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "ruleId 不能为空";
            return o.result;
        }
        RuleRuntime* rt = nullptr;
        {
            auto it = c.rules.find(obs.ruleId);
            if (it != c.rules.end()) rt = &it->second;
        }
        if (rt == nullptr) {
            ++c.metrics.notFound;
            o.result.code = 1004;
            o.result.status = ResultStatus::Rejected;
            o.result.message = "未知规则 id：" + obs.ruleId;
            return o.result;
        }
        if (!rt->enabled) {
            // ALT-RULE-04：禁用后不再产生新告警；历史告警仍可查（台账不动）
            o.result.code = 0;
            o.result.status = ResultStatus::AlreadyApplied;
            o.result.suppressed = true;
            o.result.suppressReason = "rule-disabled";
            o.result.message = "规则已禁用，不产生告警";
            return o.result;
        }

        const int64_t now = readClockObserved(c);
        reapMutes(c, now);
        housekeep(c, now);

        const MonitorKey key{obs.ruleId, obs.entityId};

        // ---- 1) 条件求值（含持续时间累计） ----
        MonitorState& st = c.ensureMonitor(key);
        bool everSatisfied = false;
        const bool met = detail::evalTracked(rt->condition, obs, st, rt->def.maxGapMs, &everSatisfied);
        (void)everSatisfied;

        // ---- 2b) 持续时间尚未满足（ALT-RULE-02 的"持续 N 秒才报"） ----
        // 根节点是 duration 且**内部条件当前成立**、只是时长没攒够 → 如实上报 pending。
        // 这条分支必须排在恢复判定之后：pending 时 alert 尚未产生，永远不构成"恢复"。
        if (!met && rt->condition.isDuration()) {
            auto rootIt = st.nodes.find(rt->condition.nodeId);
            const auto innerIt = rt->condition.inner == nullptr
                                     ? st.nodes.end()
                                     : st.nodes.find(rt->condition.inner->nodeId);
            const bool innerHolds =
                innerIt != st.nodes.end() ? innerIt->second.raw : (rootIt != st.nodes.end() && rootIt->second.raw);
            if (innerHolds) {
                o.result.code = 0;
                o.result.status = ResultStatus::Ok;
                o.result.durationPending = true;
                o.result.message = "持续时间条件尚未满足（内部条件成立，时长未满）";
                return o.result;
            }
        }

        // ---- 3) 抖动抑制（ALT-DEDUP-03）：连续确认次数 + 最小驻留时长 ----
        bool confirmed = met;
        if (met) {
            if (st.flap.streak == 0) st.flap.streakFrom = obs.ts;
            ++st.flap.streak;
            const bool enough = st.flap.streak >= std::max(1, rt->def.confirmCount);
            const bool dwellOk = rt->def.minDwellMs <= 0 ||
                                 (obs.ts - st.flap.streakFrom) >= rt->def.minDwellMs;
            confirmed = enough && dwellOk;
            if (!confirmed) {
                ++c.metrics.flappedBlocked;
                o.result.code = 0;
                o.result.status = ResultStatus::Ok;
                o.result.durationPending = true;
                o.result.message = "抖动抑制：尚未满足连续确认次数 / 最小驻留时长";
                return o.result;
            }
        } else {
            st.flap.streak = 0;
        }

        // ---- 4) 抑制判定（父告警 / 消音） ----
        const std::string mission = !obs.missionId.empty() ? obs.missionId : rt->def.missionId;
        int64_t muteUntil = 0;
        const bool muted = mutedBy(c, obs.ruleId, obs.entityId, rt->def.level, now, &muteUntil);
        const std::string parentRule = findActiveParent(c, *rt, key);
        const bool suppressed = muted || !parentRule.empty();

        // ---- 5) 去重窗口与既有记录 ----
        auto openIt = c.dedup.openAlertId.find(key);
        AlertRecord* openRec = openIt == c.dedup.openAlertId.end() ? nullptr : c.findAlert(openIt->second);
        const bool hasOpen = openRec != nullptr && openRec->active;
        const bool windowOpen =
            hasOpen && rt->def.dedupWindowMs > 0 && obs.ts < openRec->windowStart + rt->def.dedupWindowMs;

        // ---- 2) 恢复判定（ALT-GEN-04）：曾满足过、现在不满足 ----
        // 以**台账记录**上的 `conditionHeld` 为准（只有"曾经成立过的告警"才谈得上恢复），
        // 而不是本次求值的输出 —— 后者在首次触发那一刻还是 false。
        // `kind` 必须仍是 `alert`：恢复记录与风暴汇总自身不是"条件告警"，不该再被判一次恢复。
        const bool recoveredNow =
            hasOpen && openRec->conditionHeld && openRec->kind == "alert" && !met;

        if (met) {
            // ---- 5a) 窗口内合并（ALT-GEN-03 / ALT-DEDUP-02/04） ----
            if (windowOpen) {
                alert_engine::MergeEntry me;
                const AlertRecord merged = mergeInto(c, *openRec, obs, *rt, me);
                if (!commitRecord(c, merged)) {
                    ++c.metrics.rejected;
                    o.result.code = 1005;
                    o.result.status = ResultStatus::Rejected;
                    o.result.message = "台账写入失败（写前提交），内存状态未变";
                    return o.result;
                }
                c.alerts[merged.alertId] = merged;
                ++c.counts.merged;
                ++c.counts.rawRaises;
                ++c.metrics.merged;
                ++c.metrics.rawRaises;
                AlertDelivery d = buildDelivery(c, merged, "alert.updated", obs.ts);
                d.payload = toJson(detail::updatedEventOf(merged, me));
                o.deliveries.emplace_back("alert.updated", std::move(d));
                o.result.code = 0;
                o.result.status = ResultStatus::AlreadyApplied;
                o.result.merged = true;
                o.result.message = "窗口内合并：计数累加（首次时间不变）";
                o.result.alert = merged;
                goto dispatch;
            }

            // ---- 5b) 冷却期内复发（抖动抑制）：不新开，只并入原记录并重新打开 ----
            if (rt->def.cooldownMs > 0) {
                auto closedIt = c.dedup.closedAlertId.find(key);
                AlertRecord* rec = closedIt == c.dedup.closedAlertId.end()
                                       ? nullptr
                                       : c.findAlert(closedIt->second);
                if (rec != nullptr && !rec->active && obs.ts < rec->lastAt + rt->def.cooldownMs) {
                    alert_engine::MergeEntry me;
                    AlertRecord merged = mergeInto(c, *rec, obs, *rt, me);
                    const std::string fromState = toString(merged.state);
                    merged.state = AlertState::Active;
                    merged.active = true;
                    merged.ackedBy.clear();
                    merged.ackedAt = 0;
                    merged.closedBy.clear();
                    merged.closedAt = 0;
                    merged.closeReason.clear();
                    alert_engine::StateChange sc;
                    sc.at = obs.ts;
                    sc.fromState = fromState;
                    sc.toState = "active";
                    sc.action = "reopen";
                    sc.reason = "cooldown-merge";
                    merged.stateHistory.push_back(sc);
                    me.reopened = true;
                    merged.mergeHistory.back().reopened = true;
                    if (!commitRecord(c, merged)) {
                        ++c.metrics.rejected;
                        o.result.code = 1005;
                        o.result.status = ResultStatus::Rejected;
                        o.result.message = "台账写入失败（写前提交），内存状态未变";
                        return o.result;
                    }
                    c.alerts[merged.alertId] = merged;
                    c.dedup.openAlertId[key] = merged.alertId;
                    c.dedup.closedAlertId.erase(key);
                    ++c.counts.merged;
                    ++c.counts.rawRaises;
                    ++c.metrics.merged;
                    ++c.metrics.rawRaises;
                    AlertDelivery d = buildDelivery(c, merged, "alert.updated", obs.ts);
                    d.payload = toJson(detail::updatedEventOf(merged, me));
                    o.deliveries.emplace_back("alert.updated", std::move(d));
                    o.result.code = 0;
                    o.result.status = ResultStatus::AlreadyApplied;
                    o.result.merged = true;
                    o.result.message = "冷却期内复发：并入原记录并重新打开";
                    o.result.alert = merged;
                    goto dispatch;
                }
            }

            // ---- 5c) 抑制 / 消音（ALT-DEDUP-05 / ALT-ACK-04） ----
            if (suppressed) {
                ++c.counts.rawRaises;
                ++c.metrics.rawRaises;
                ++c.counts.suppressed;
                ++c.metrics.suppressed;
                if (muted) {
                    ++c.metrics.mutedSuppressed;
                } else {
                    ++c.metrics.suppressedByParent;
                }
                o.result.code = 0;
                o.result.status = ResultStatus::Ok;
                o.result.suppressed = true;
                o.result.muted = muted;
                o.result.suppressReason =
                    muted ? ("消音中（until=" + std::to_string(muteUntil) + "）")
                          : ("父告警存在（规则 " + parentRule + "），子告警被抑制");
                o.result.message = suppressed && muted ? "消音中：未产生告警" : "被父告警抑制：未产生子告警";
                goto dispatch;
            }

            // ---- 5d) 风暴保护（ALT-DEDUP-06） ----
            if (c.storm.enabled) {
                auto& bucket = c.storm.raisedAt;
                while (!bucket.empty() && obs.ts - bucket.front() >= c.storm.policy.windowMs) {
                    bucket.erase(bucket.begin());
                }
                if (static_cast<int64_t>(bucket.size()) >= c.storm.policy.maxRaised) {
                    raiseAggregate(c, obs, mission, o);
                    goto dispatch;
                }
                bucket.push_back(obs.ts);
            }

            // ---- 5e) 新开一条（ALT-GEN-03 / ALT-DEDUP-01） ----
            raiseNew(c, obs, *rt, key, mission, parentRule, o);
            goto dispatch;
        }

        // ---- 6) 条件不再满足：恢复 / 自动关闭（ALT-GEN-04 / ALT-ACK-05） ----
        if (!met) {
            if (recoveredNow && hasOpen) {
                handleRecovery(c, obs, *rt, key, *openRec, o);
            } else {
                o.result.code = 0;
                o.result.status = ResultStatus::Ok;
                o.result.message = "条件未满足";
            }
            // 连续计数归零（ALT-DEDUP-03 的连续性：条件断开即从头累计）。
            //
            // **监控条目本身 MUST NOT 被删除** —— 它承载三类必须跨观测存活的状态：
            //   ① 持续时间计时（"持续 N 秒才报"就是跨观测累计的语义，ALT-RULE-02）
            //   ② `everMet`（"该条件曾经成立过"是恢复规则的唯一锚点，ALT-GEN-04）
            //   ③ `lastTs`（连续性缺口判定）
            // 条目清理由 `housekeep()` 按保留期统一淘汰，避免无界增长（ALT-NFR-07）。
            st.flap.streak = 0;
            st.flap.streakFrom = 0;
        }

    dispatch:;
    }

    // ---- 通知在锁外派发（protocol §6：MUST NOT 在锁内调用宿主回调） ----
    int64_t sinkErrors = 0;
    for (const auto& p : o.deliveries) {
        if (p.first == "alert.raised") {
            safeRaised(impl_->sink, p.second, sinkErrors);
        } else if (p.first == "alert.updated") {
            safeUpdated(impl_->sink, p.second, sinkErrors);
        } else {
            safeAcked(impl_->sink, p.second, sinkErrors);
        }
    }
    if (sinkErrors != 0) {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        impl_->metrics.sinkErrors += sinkErrors;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        refreshCountsLocked(*impl_);
    }
    return o.result;
}

// ============================================================================
// 生命周期（ALT-ACK-01/02/03）
// ============================================================================

namespace {

enum class TransitionKind { Legal, Idempotent, Illegal };

TransitionKind classify(AlertState from, AlertState to) {
    if (from == to) return TransitionKind::Idempotent;
    switch (to) {
        case AlertState::Acknowledged:
            return from == AlertState::Active ? TransitionKind::Legal : TransitionKind::Illegal;
        case AlertState::Closed:
            return (from == AlertState::Active || from == AlertState::Acknowledged)
                       ? TransitionKind::Legal
                       : TransitionKind::Illegal;
        case AlertState::Active:
            return from == AlertState::Closed ? TransitionKind::Legal : TransitionKind::Idempotent;
    }
    return TransitionKind::Illegal;
}

}  // namespace

ActionResult detailApplyTransition(AlertEngine& engine, const std::string& alertId,
                                   AlertState target, const std::string& action,
                                   const std::string& operatorId, const std::string& reason) {
    EngineCore& c = *engine.impl_;
    ActionResult out;
    out.action = action;
    out.operatorId = operatorId;

    AlertDelivery delivery;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(c.mutex);
        AlertRecord* rec = c.findAlert(alertId);
        if (alertId.empty()) {
            ++c.metrics.rejected;
            out.code = 1000;
            out.status = ResultStatus::Rejected;
            out.message = "alertId 不能为空";
        } else if (rec == nullptr) {
            ++c.metrics.notFound;
            out.code = 1004;
            out.status = ResultStatus::Rejected;
            out.message = "未知告警 id：" + alertId;
        } else {
            const AlertState from = rec->state;
            out.fromState = toString(from);
            out.toState = toString(target);
            const TransitionKind kind = classify(from, target);

            if (kind == TransitionKind::Illegal) {
                // ALT-ACK-01：非法迁移 MUST 被拒绝并给可读原因（1003，不抛异常）
                ++c.metrics.rejected;
                out.code = 1003;
                out.status = ResultStatus::Rejected;
                out.message = "非法状态迁移：" + out.fromState + " → " + out.toState +
                              "（允许 active→acknowledged→closed，以及 closed→active 重开）";
                out.alert = *rec;
            } else if (kind == TransitionKind::Idempotent) {
                // ALT-ACK-02：重复确认 MUST 返回成功并标注"已确认"，**MUST NOT 报错**（C15）。
                // 零副作用：MUST NOT 改台账、MUST NOT 追加历史（只回一条标注 idempotent 的通知）。
                ++c.metrics.idempotentHits;
                out.code = 0;
                out.status = ResultStatus::AlreadyApplied;
                out.idempotent = true;
                out.changed = false;
                out.message = "已是目标状态（幂等命中，零副作用）";
                out.alert = *rec;
                alert_engine::StateChange sc;
                sc.at = readClockObserved(c);
                sc.fromState = out.fromState;
                sc.toState = out.toState;
                sc.action = action;
                sc.operatorId = operatorId;
                sc.reason = reason;
                delivery = buildDelivery(c, *rec, "alert.acked", sc.at);
                delivery.payload = toJson(detail::ackedEventOf(*rec, sc, true));
                notify = true;
            } else {
                // 合法迁移：写前提交 → 提交内存 → 维护去重索引 → 构造 alert.acked
                AlertRecord updated = *rec;
                const int64_t now = readClockObserved(c);
                alert_engine::StateChange sc;
                sc.at = now;
                sc.fromState = out.fromState;
                sc.toState = out.toState;
                sc.action = action;
                sc.operatorId = operatorId;
                sc.reason = reason;
                updated.state = target;
                updated.active = (target != AlertState::Closed);
                if (target == AlertState::Acknowledged) {
                    updated.ackedBy = operatorId;
                    updated.ackedAt = now;
                } else if (target == AlertState::Closed) {
                    updated.closedBy = operatorId;
                    updated.closedAt = now;
                    updated.closeReason = reason;
                } else {  // reopen
                    updated.ackedBy.clear();
                    updated.ackedAt = 0;
                    updated.closedBy.clear();
                    updated.closedAt = 0;
                    updated.closeReason.clear();
                }
                updated.stateHistory.push_back(sc);  // ALT-ACK-03：操作者与时间 MUST 留痕

                if (!commitRecord(c, updated)) {
                    ++c.metrics.rejected;
                    out.code = 1005;
                    out.status = ResultStatus::Rejected;
                    out.message = "台账写入失败（写前提交），内存状态未变";
                    out.alert = *rec;
                } else {
                    c.alerts[updated.alertId] = updated;
                    const MonitorKey key{updated.ruleId, updated.entityId};
                    if (target == AlertState::Closed) {
                        auto it = c.dedup.openAlertId.find(key);
                        if (it != c.dedup.openAlertId.end() && it->second == updated.alertId) {
                            c.dedup.openAlertId.erase(it);
                        }
                        c.dedup.closedAlertId[key] = updated.alertId;
                    } else if (target == AlertState::Active) {
                        c.dedup.openAlertId[key] = updated.alertId;
                        auto it = c.dedup.closedAlertId.find(key);
                        if (it != c.dedup.closedAlertId.end() && it->second == updated.alertId) {
                            c.dedup.closedAlertId.erase(it);
                        }
                    }
                    out.code = 0;
                    out.status = ResultStatus::Ok;
                    out.changed = true;
                    out.message = "状态已迁移：" + out.fromState + " → " + out.toState;
                    out.alert = updated;
                    delivery = buildDelivery(c, updated, "alert.acked", now);
                    delivery.payload = toJson(detail::ackedEventOf(updated, sc, false));
                    notify = true;
                }
            }
        }
    }

    // 通知在**锁外**派发（protocol §6：MUST NOT 在锁内调用宿主回调）
    if (notify) {
        int64_t sinkErrors = 0;
        safeAcked(c.sink, delivery, sinkErrors);
        if (sinkErrors != 0) {
            std::lock_guard<std::mutex> lk(c.mutex);
            c.metrics.sinkErrors += sinkErrors;
        }
        safeAudit(c.log, action, alertId, delivery.payload);
    }
    return out;
}
ActionResult AlertEngine::acknowledge(const std::string& alertId, const std::string& operatorId,
                                      const std::string& reason) {
    return detailApplyTransition(*this, alertId, AlertState::Acknowledged, "acknowledge", operatorId,
                                 reason);
}

ActionResult AlertEngine::close(const std::string& alertId, const std::string& operatorId,
                                const std::string& reason) {
    return detailApplyTransition(*this, alertId, AlertState::Closed, "close", operatorId, reason);
}

ActionResult AlertEngine::reopen(const std::string& alertId, const std::string& operatorId,
                                 const std::string& reason) {
    return detailApplyTransition(*this, alertId, AlertState::Active, "reopen", operatorId, reason);
}

// ============================================================================
// 消音（ALT-ACK-04：有时限，到期自动恢复）
// ============================================================================

MuteResult AlertEngine::mute(const MuteRequest& req) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    MuteResult out;
    if (req.key.empty()) {
        ++c.metrics.rejected;
        out.code = 1000;
        out.message = "消音 key 不能为空（规则 id 或实体 id）";
        return out;
    }
    if (req.scope == MuteScope::Rule && c.rules.count(req.key) == 0) {
        ++c.metrics.notFound;
        out.code = 1004;
        out.message = "未知规则 id：" + req.key;
        return out;
    }
    if (!req.level.empty() && c.loaded && c.levelRank.count(req.level) == 0) {
        ++c.metrics.rejected;
        out.code = 1000;
        out.message = "未知级别取值：" + req.level;
        return out;
    }
    const int64_t now = readClockObserved(c);
    int64_t until = req.untilMs;
    if (until == 0) {
        int64_t duration = req.durationMs;
        if (duration == 0) duration = c.mutePolicy.defaultMs;
        if (duration <= 0) {
            ++c.metrics.rejected;
            out.code = 1000;
            out.message = "MUST 给出 durationMs、untilMs，或由规则包 mute.defaultMs 提供缺省时长";
            return out;
        }
        until = now + duration;
    }
    if (until <= now) {
        ++c.metrics.rejected;
        out.code = 1000;
        out.message = "消音到期时刻 MUST 晚于当前时间（限时消音才有意义）";
        return out;
    }
    if (c.mutePolicy.maxMs > 0 && (until - now) > c.mutePolicy.maxMs) {
        ++c.metrics.rejected;
        out.code = 1000;
        out.message = "消音时长超过规则包上限 mute.maxMs=" + std::to_string(c.mutePolicy.maxMs);
        return out;
    }
    MuteEntry m;
    m.muteId = c.nextMuteId();
    m.scope = req.scope;
    m.key = req.key;
    m.level = req.level;
    m.from = now;
    m.until = until;
    m.operatorId = req.operatorId;
    m.reason = req.reason;
    c.mutes[m.muteId] = m;
    safeAudit(c.log, "mute", m.muteId, toJson(m));
    out.code = 0;
    out.message = "已消音，到期自动恢复";
    out.mute = m;
    return out;
}

MuteResult AlertEngine::unmute(const std::string& muteId, const std::string& operatorId) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    MuteResult out;
    auto it = c.mutes.find(muteId);
    if (it == c.mutes.end()) {
        ++c.metrics.notFound;
        out.code = 1004;
        out.message = "未知消音 id：" + muteId;
        return out;
    }
    MuteEntry m = it->second;
    c.mutes.erase(it);
    safeAudit(c.log, "unmute", muteId, toJson(m));
    out.code = 0;
    out.message = "已取消消音";
    out.mute = m;
    (void)operatorId;
    return out;
}

std::vector<MuteEntry> AlertEngine::mutes() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    const int64_t now = readClock(c);
    std::vector<MuteEntry> out;
    for (const auto& kv : c.mutes) {
        MuteEntry m = kv.second;
        m.expired = (now >= m.until);  // 到期即视为已恢复（不再拦截告警）
        out.push_back(std::move(m));
    }
    return out;
}

// ============================================================================
// 台账查询（ALT-SUB-01 / ALT-SUB-02 / ALT-SUB-05 / ALT-GEN-05）
// ============================================================================

namespace {

/// 按筛选条件判定；`total` 计数与 `items` 收集共用同一判定，避免两套口径
bool queryMatch(const AlertQuery& q, const AlertRecord& r) {
    if (!q.includeRecovery && r.kind == "recovery") return false;
    if (!q.includeAggregate && r.kind == "aggregate") return false;
    if (q.activeOnly && !r.active) return false;
    if (!q.kindIn.empty() &&
        std::find(q.kindIn.begin(), q.kindIn.end(), r.kind) == q.kindIn.end()) {
        return false;
    }
    if (!q.levelIn.empty() &&
        std::find(q.levelIn.begin(), q.levelIn.end(), r.level) == q.levelIn.end()) {
        return false;
    }
    if (!q.entityIn.empty() &&
        std::find(q.entityIn.begin(), q.entityIn.end(), r.entityId) == q.entityIn.end()) {
        return false;
    }
    if (!q.ruleIn.empty() &&
        std::find(q.ruleIn.begin(), q.ruleIn.end(), r.ruleId) == q.ruleIn.end()) {
        return false;
    }
    if (!q.missionIn.empty() &&
        std::find(q.missionIn.begin(), q.missionIn.end(), r.missionId) == q.missionIn.end()) {
        return false;
    }
    if (!q.stateIn.empty()) {
        const std::string s = toString(r.state);
        if (std::find(q.stateIn.begin(), q.stateIn.end(), s) == q.stateIn.end()) return false;
    }
    const int64_t t = q.useFirstAt ? r.firstAt : r.lastAt;
    if (q.from != 0 && t < q.from) return false;
    if (q.to != 0 && t > q.to) return false;
    return true;
}

void refreshCountsForList(EngineCore& c) {
    AlertCounts& n = c.counts;
    n.alertCount = static_cast<int64_t>(c.alerts.size());
    n.openActive = 0;
    for (const auto& kv : c.alerts) {
        if (kv.second.active) ++n.openActive;
    }
}

}  // namespace

// ============================================================================
// 观测 → 告警（ALT-GEN / ALT-DEDUP）
// ============================================================================

AlertQueryResult AlertEngine::listAlerts(const AlertQuery& q) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    housekeep(c, readClock(c));

    AlertQueryResult out;
    out.limit = q.limit;
    out.offset = q.offset < 0 ? 0 : q.offset;
    std::vector<const AlertRecord*> matched;
    for (const auto& kv : c.alerts) {  // 有序 map ⇒ 按产生顺序，确定性
        if (queryMatch(q, kv.second)) matched.push_back(&kv.second);
    }
    out.total = static_cast<int>(matched.size());
    const int limit = q.limit <= 0 ? 0 : q.limit;
    for (int i = out.offset; i < out.total && static_cast<int>(out.items.size()) < limit; ++i) {
        out.items.push_back(*matched[static_cast<std::size_t>(i)]);
    }
    out.returned = static_cast<int>(out.items.size());
    const int skipped = std::min(out.offset, out.total);
    out.omitted = out.total - skipped - out.returned;
    if (out.omitted < 0) out.omitted = 0;
    // ALT-SUB-01：**超限截断不静默丢** —— 截断必须显式标注并给出可读原因
    if (out.omitted > 0) {
        out.truncated = true;
        out.truncationReason =
            "结果被截断：总命中 " + std::to_string(out.total) + " 条，offset=" +
            std::to_string(out.offset) + " limit=" + std::to_string(limit) + "，返回 " +
            std::to_string(out.returned) + " 条，省略 " + std::to_string(out.omitted) + " 条";
        ++c.metrics.truncations;
    }
    out.code = 0;
    out.message = out.truncated ? "截断返回（见 truncationReason）" : "ok";
    return out;
}

std::optional<AlertRecord> AlertEngine::getAlert(const std::string& alertId) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    const AlertRecord* r = impl_->findAlert(alertId);
    if (r == nullptr) return std::nullopt;  // MUST NOT 返回空对象当真值（protocol §2.3）
    return *r;
}

json AlertEngine::activeAlerts() const {
    // ALT-SUB-05：**一次调用返回全部活动告警**（不是逐条推送）。
    // 自持锁实现（不调用 listAlerts，避免递归加锁）；一次性拉取不做截断，
    // 但仍如实上报条数，调用方可据此判断是否需要用 listAlerts 分页。
    std::lock_guard<std::mutex> lk(impl_->mutex);
    AlertQueryResult res;
    for (const auto& kv : impl_->alerts) {
        if (kv.second.active) res.items.push_back(kv.second);
    }
    res.total = static_cast<int>(res.items.size());
    res.returned = res.total;
    res.limit = 0;  // 0 = 不限量
    res.offset = 0;
    res.code = 0;
    res.message = "ok";
    return res.toJson();
}

AlertCounts AlertEngine::counts() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    refreshCountsForList(*impl_);
    return impl_->counts;
}

// ============================================================================
// 订阅（ALT-SUB-03）
// ============================================================================

namespace {

/// 订阅请求的结果形状：复用 ActionResult 的 `code/message`（无状态迁移语义）
ActionResult subResult(int code, const std::string& message) {
    ActionResult r;
    r.code = code;
    r.message = message;
    r.status = code == 0 ? ResultStatus::Ok : ResultStatus::Rejected;
    return r;
}

}  // namespace

ActionResult AlertEngine::subscribe(const AlertSubscription& sub) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    if (!detail::isSubscriberId(sub.subscriberId)) {
        ++c.metrics.rejected;
        return subResult(1000, "subscriberId 非法（期望 ^[a-z][a-z0-9-]{0,63}$）：" + sub.subscriberId);
    }
    for (const auto& lv : sub.levels) {
        if (c.loaded && c.levelRank.count(lv) == 0) {
            ++c.metrics.rejected;
            return subResult(1000, "订阅声明的级别不在规则包 levels 目录内：" + lv);
        }
    }
    c.subscriptions[sub.subscriberId] = sub;  // 同 id 覆盖（可更新关心的集合）
    return subResult(0, "订阅已登记（只推匹配项）");
}

bool AlertEngine::unsubscribe(const std::string& subscriberId) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->subscriptions.erase(subscriberId) != 0;
}

std::vector<AlertSubscription> AlertEngine::subscriptions() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    std::vector<AlertSubscription> out;
    for (const auto& kv : impl_->subscriptions) out.push_back(kv.second);
    return out;
}

// ============================================================================
// 由宿主驱动时间前进（消音到期 / 台账清扫；**不产生观测**）
// ============================================================================

json AlertEngine::tick(int64_t nowMs) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    EngineCore& c = *impl_;
    int64_t now = nowMs;
    if (now <= 0) now = readClockObserved(c);
    if (c.lastKnownNow != 0 && now < c.lastKnownNow) ++c.metrics.clockRegressions;
    if (now > c.lastKnownNow) c.lastKnownNow = now;

    const int expired = reapMutes(c, now);
    const int64_t before = c.metrics.evicted;
    c.lastHousekeep = 0;  // 显式驱动：强制执行一次清扫
    housekeep(c, now);
    refreshCountsForList(c);

    json data = json::object();
    data["now"] = now;
    data["mutesExpired"] = expired;
    data["mutesActive"] = static_cast<int>(c.mutes.size());
    data["evicted"] = c.metrics.evicted - before;
    data["alerts"] = static_cast<int64_t>(c.alerts.size());
    data["openActive"] = c.counts.openActive;
    return data;
}

// ============================================================================
// 自述与观测
// ============================================================================

Capabilities AlertEngine::capabilities() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    const EngineCore& c = *impl_;
    Capabilities out;
    out.rulesLoaded = c.loaded;
    out.pureMemory = !c.storeInjectedFlag;
    out.storeInjected = c.storeInjectedFlag;
    out.clockInjected = c.clockInjectedFlag;
    out.sinkInjected = c.sinkInjectedFlag;
    out.logInjected = c.logInjectedFlag;
    out.ruleCount = static_cast<int>(c.rules.size());
    out.levelCount = static_cast<int>(c.levels.size());
    out.subscriptionCount = static_cast<int>(c.subscriptions.size());
    out.stormEnabled = c.storm.enabled ? 1 : 0;
    out.maxActiveAlerts = c.maxActiveAlerts;
    out.retentionMs = c.retentionMs;
    out.alertCountBasis = c.counts.basis;
    out.engineVersion = kEngineVersion;
    out.policiesNamespace = c.policiesNamespace;
    out.schemaVersion = c.schemaVersion;
    out.policiesDigest = c.digest;
    return out;
}

Metrics AlertEngine::metrics() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->metrics;
}

void AlertEngine::resetMetrics() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->metrics = Metrics{};
}

}  // namespace alert_engine

