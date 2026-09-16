// src/json.cc —— 序列化：台账记录 / 事件负载 / 结果信封 / 规则包视图 / 订阅匹配
//
// 契约依据（严格照做，MUST NOT 漂移）：
//   · protocol.md §3.1 统一信封 `{code, message, data}`
//   · protocol.md §4.4 已登记事件：
//       `alert.raised`  → {alertId, ruleId, level, entityId?, missionId, firstAt, count}
//       `alert.updated` → {alertId, count, lastAt, level, upgraded}
//       `alert.acked`   → {alertId, state, operatorId, at}
//   · CTR-EV-04 负载字段**只增不改**：契约字段在前且语义不变，只增字段在后
//   · CTR-EV-05 `data` MUST 是 JSON 对象、字段 camelCase
//   · realtime-hub/protocol.md §3：**缺失的可选字段省略**（不写 null）
//   · 需求 D5 / ALT-SUB-06：既有前端 `applyWs` 的 `alert` case 零改动可读
#include <string>

#include "internal.h"

namespace alert_engine {

namespace {

/// 可选字段：空串 → 省略（不写 null —— realtime-hub/protocol.md §3）
void putOpt(json& o, const char* key, const std::string& v) {
    if (!v.empty()) o[key] = v;
}

}  // namespace

// ============================================================================
// 台账记录
// ============================================================================

json toJson(const MergeEntry& v) {
    json o = json::object();
    o["at"] = v.at;
    o["countAdded"] = v.countAdded;
    if (v.hasValue) o["value"] = v.value;
    o["upgraded"] = v.upgraded;
    o["reopened"] = v.reopened;
    putOpt(o, "fromLevel", v.fromLevel);
    putOpt(o, "toLevel", v.toLevel);
    return o;
}

json toJson(const LevelChange& v) {
    json o = json::object();
    o["at"] = v.at;
    o["level"] = v.level;
    o["reason"] = v.reason;
    return o;
}

json toJson(const StateChange& v) {
    json o = json::object();
    o["at"] = v.at;
    o["fromState"] = v.fromState;
    o["toState"] = v.toState;
    o["action"] = v.action;
    putOpt(o, "operatorId", v.operatorId);
    putOpt(o, "reason", v.reason);
    return o;
}

namespace detail {

json alertRecordJson(const AlertRecord& r) {
    json o = json::object();
    o["alertId"] = r.alertId;
    o["ruleId"] = r.ruleId;
    putOpt(o, "ruleVersion", r.ruleVersion);
    o["kind"] = r.kind;
    o["level"] = r.level;
    o["originLevel"] = r.originLevel;
    putOpt(o, "entityId", r.entityId);
    putOpt(o, "missionId", r.missionId);
    putOpt(o, "metric", r.metric);
    o["state"] = toString(r.state);
    o["active"] = r.active;
    o["firstValue"] = r.firstValue;
    o["lastValue"] = r.lastValue;
    o["firstAt"] = r.firstAt;
    o["lastAt"] = r.lastAt;
    o["count"] = r.count;
    o["foldedCount"] = r.foldedCount;
    o["windowStart"] = r.windowStart;
    putOpt(o, "linkedAlertId", r.linkedAlertId);
    putOpt(o, "parentRuleId", r.parentRuleId);
    putOpt(o, "title", r.title);
    putOpt(o, "ackedBy", r.ackedBy);
    if (r.ackedAt != 0) o["ackedAt"] = r.ackedAt;
    putOpt(o, "closedBy", r.closedBy);
    if (r.closedAt != 0) o["closedAt"] = r.closedAt;
    putOpt(o, "closeReason", r.closeReason);

    json lh = json::array();
    for (const auto& e : r.levelHistory) lh.push_back(toJson(e));
    o["levelHistory"] = std::move(lh);

    json mh = json::array();
    for (const auto& e : r.mergeHistory) mh.push_back(toJson(e));
    o["mergeHistory"] = std::move(mh);

    json sh = json::array();
    for (const auto& e : r.stateHistory) sh.push_back(toJson(e));
    o["stateHistory"] = std::move(sh);
    return o;
}

}  // namespace detail

json AlertRecord::toJson() const { return detail::alertRecordJson(*this); }

// ============================================================================
// 事件负载（契约字段在前，只增字段在后 —— CTR-EV-04）
// ============================================================================

json toJson(const AlertRaisedEvent& v) {
    json o = json::object();
    // ---- protocol.md §4.4 登记的契约字段 ----
    o["alertId"] = v.alertId;
    o["ruleId"] = v.ruleId;
    o["level"] = v.level;
    putOpt(o, "entityId", v.entityId);
    o["missionId"] = v.missionId;
    o["firstAt"] = v.firstAt;
    o["count"] = v.count;
    // ---- 只增字段（CTR-EV-04） ----
    putOpt(o, "kind", v.kind);
    putOpt(o, "title", v.title);
    putOpt(o, "metric", v.metric);
    o["value"] = v.value;
    putOpt(o, "ruleVersion", v.ruleVersion);
    putOpt(o, "originLevel", v.originLevel);
    putOpt(o, "linkedAlertId", v.linkedAlertId);
    o["foldedCount"] = v.foldedCount;
    o["ts"] = v.ts;
    return o;
}

json toJson(const AlertUpdatedEvent& v) {
    json o = json::object();
    o["alertId"] = v.alertId;
    o["count"] = v.count;
    o["lastAt"] = v.lastAt;
    o["level"] = v.level;
    o["upgraded"] = v.upgraded;
    putOpt(o, "ruleId", v.ruleId);
    putOpt(o, "entityId", v.entityId);
    putOpt(o, "missionId", v.missionId);
    putOpt(o, "fromLevel", v.fromLevel);
    putOpt(o, "originLevel", v.originLevel);
    o["value"] = v.value;
    o["countAdded"] = v.countAdded;
    o["foldedCount"] = v.foldedCount;
    o["ts"] = v.ts;
    return o;
}

json toJson(const AlertAckedEvent& v) {
    json o = json::object();
    o["alertId"] = v.alertId;
    o["state"] = v.state;
    o["operatorId"] = v.operatorId;
    o["at"] = v.at;
    putOpt(o, "ruleId", v.ruleId);
    putOpt(o, "level", v.level);
    putOpt(o, "entityId", v.entityId);
    putOpt(o, "missionId", v.missionId);
    putOpt(o, "action", v.action);
    putOpt(o, "fromState", v.fromState);
    putOpt(o, "reason", v.reason);
    o["idempotent"] = v.idempotent;
    o["ts"] = v.at;
    return o;
}

json toJson(const AlertDelivery& v) {
    json o = json::object();
    o["event"] = v.event;
    o["payload"] = v.payload;
    json ids = json::array();
    for (const auto& s : v.subscriberIds) ids.push_back(s);
    o["subscriberIds"] = std::move(ids);
    o["subscriberCount"] = static_cast<int>(v.subscriberIds.size());
    o["broadcast"] = v.broadcast;
    o["ts"] = v.ts;
    return o;
}

namespace detail {

AlertRaisedEvent raisedEventOf(const AlertRecord& r) {
    AlertRaisedEvent e;
    e.alertId = r.alertId;
    e.ruleId = r.ruleId;
    e.level = r.level;
    e.entityId = r.entityId;
    e.missionId = r.missionId;
    e.firstAt = r.firstAt;
    e.count = r.count;
    e.kind = r.kind;
    e.title = r.title;
    e.metric = r.metric;
    e.value = r.lastValue;
    e.ruleVersion = r.ruleVersion;
    e.originLevel = r.originLevel;
    e.linkedAlertId = r.linkedAlertId;
    e.foldedCount = r.foldedCount;
    e.ts = r.lastAt;
    return e;
}

AlertUpdatedEvent updatedEventOf(const AlertRecord& r, const MergeEntry& m) {
    AlertUpdatedEvent e;
    e.alertId = r.alertId;
    e.count = r.count;
    e.lastAt = r.lastAt;
    e.level = r.level;
    e.upgraded = m.upgraded;
    e.ruleId = r.ruleId;
    e.entityId = r.entityId;
    e.missionId = r.missionId;
    e.fromLevel = m.fromLevel;
    e.originLevel = r.originLevel;
    e.value = r.lastValue;
    e.countAdded = m.countAdded;
    e.foldedCount = r.foldedCount;
    e.ts = m.at;
    return e;
}

AlertAckedEvent ackedEventOf(const AlertRecord& r, const StateChange& s, bool idempotent) {
    AlertAckedEvent e;
    e.alertId = r.alertId;
    e.state = toString(r.state);
    e.operatorId = s.operatorId;
    e.at = s.at;
    e.ruleId = r.ruleId;
    e.level = r.level;
    e.entityId = r.entityId;
    e.missionId = r.missionId;
    e.action = s.action;
    e.fromState = s.fromState;
    e.reason = s.reason;
    e.idempotent = idempotent;
    return e;
}

}  // namespace detail

// ============================================================================
// 订阅匹配（ALT-SUB-03：空集合 = 不筛该项；每个非空集合 MUST 命中）
// ============================================================================

namespace detail {

namespace {

bool setHits(const std::vector<std::string>& set, const std::string& v) {
    if (set.empty()) return true;  // 空 = 不筛
    for (const auto& s : set) {
        if (s == v) return true;
    }
    return false;
}

}  // namespace

bool subscriptionMatches(const AlertSubscription& s, const AlertRecord& r) {
    if (!s.includeRecovery && r.kind == "recovery") return false;
    if (!s.includeAggregate && r.kind == "aggregate") return false;
    if (!setHits(s.ruleIds, r.ruleId)) return false;
    if (!setHits(s.levels, r.level)) return false;
    if (!setHits(s.entityIds, r.entityId)) return false;
    if (!setHits(s.missionIds, r.missionId)) return false;
    return true;
}

}  // namespace detail

}  // namespace alert_engine
