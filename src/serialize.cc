// src/serialize.cc —— 序列化：结果信封 / 台账记录 / 规则包视图 / 订阅 / 消音 / 自述
//
// 契约依据（严格照做，MUST NOT 漂移）：
//   · protocol.md §3.1 统一信封 `{code, message, data}`
//   · CTR-EV-05 `data` MUST 是 JSON 对象、字段 camelCase
//   · realtime-hub/protocol.md §3：缺失的可选字段**省略**（不写 null）
//   · 事件负载（alert.raised/updated/acked）在 src/json.cc
#include <string>

#include "internal.h"

namespace alert_engine {

namespace {

void putOpt(json& o, const char* key, const std::string& v) {
    if (!v.empty()) o[key] = v;
}

json strArray(const std::vector<std::string>& v) {
    json a = json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

using detail::alertRecordJson;  // 台账记录的序列化实现（src/json.cc）

}  // namespace

// ---- 结果信封 ----

json AlertResult::toJson() const {
    json data = json::object();
    data["status"] = toString(status);
    data["ruleId"] = ruleId;
    data["raised"] = raised;
    data["merged"] = merged;
    data["suppressed"] = suppressed;
    data["recovered"] = recovered;
    data["muted"] = muted;
    data["folded"] = folded;
    data["durationPending"] = durationPending;
    putOpt(data, "suppressReason", suppressReason);
    if (!alert.alertId.empty()) data["alert"] = alertRecordJson(alert);
    json env = json::object();
    env["code"] = code;
    env["message"] = message;
    env["data"] = std::move(data);
    return env;
}

json toJson(const AlertResult& v) { return v.toJson(); }

json ActionResult::toJson() const {
    json data = json::object();
    data["status"] = toString(status);
    data["action"] = action;
    data["idempotent"] = idempotent;
    data["conflict"] = conflict;
    putOpt(data, "operatorId", operatorId);
    data["fromState"] = fromState;
    data["toState"] = toState;
    data["changed"] = changed;
    if (!alert.alertId.empty()) data["alert"] = alertRecordJson(alert);
    json env = json::object();
    env["code"] = code;
    env["message"] = message;
    env["data"] = std::move(data);
    return env;
}

json toJson(const ActionResult& v) { return v.toJson(); }

json toJson(const AlertCounts& v) {
    json o = json::object();
    o["basis"] = v.basis;
    o["alertCount"] = v.alertCount;
    o["rawRaises"] = v.rawRaises;
    o["alertsRaised"] = v.alertsRaised;
    o["merged"] = v.merged;
    o["recoveries"] = v.recoveries;
    o["aggregates"] = v.aggregates;
    o["suppressed"] = v.suppressed;
    o["folded"] = v.folded;
    o["openActive"] = v.openActive;
    return o;
}

json AlertCounts::toJson() const { return alert_engine::toJson(*this); }

json AlertQueryResult::toJson() const {
    json o = json::object();
    o["total"] = total;
    o["returned"] = returned;
    o["truncated"] = truncated;
    o["omitted"] = omitted;
    putOpt(o, "truncationReason", truncationReason);
    o["limit"] = limit;
    o["offset"] = offset;
    json arr = json::array();
    for (const auto& r : items) arr.push_back(alertRecordJson(r));
    o["items"] = std::move(arr);
    return o;
}

json AlertQueryResult::toEnvelope() const {
    json env = json::object();
    env["code"] = code;
    env["message"] = message;
    env["data"] = toJson();
    return env;
}

json toJson(const AlertQueryResult& v) { return v.toJson(); }

json toJson(const LoadIssue& v) {
    json o = json::object();
    o["path"] = v.path;
    o["field"] = v.field;
    o["reason"] = v.reason;
    return o;
}

json LoadResult::toJson() const {
    json d = json::object();
    d["loaded"] = this->data.loaded;
    d["policiesNamespace"] = this->data.policiesNamespace;
    d["schemaVersion"] = this->data.schemaVersion;
    d["digest"] = this->data.digest;
    d["ruleCount"] = this->data.ruleCount;
    d["levelCount"] = this->data.levelCount;
    d["disabledCount"] = this->data.disabledCount;
    d["suppressionEdges"] = this->data.suppressionEdges;
    d["stormEnabled"] = this->data.stormEnabled;
    d["muteDefaultMs"] = this->data.muteDefaultMs;
    d["levelKeys"] = strArray(this->data.levelKeys);
    d["ruleIds"] = strArray(this->data.ruleIds);
    d["warnings"] = strArray(this->data.warnings);
    json issueArr = json::array();
    for (const auto& i : this->issues) issueArr.push_back(alert_engine::toJson(i));
    json env = json::object();
    env["code"] = code;
    env["message"] = message;
    env["data"] = std::move(d);
    env["issues"] = std::move(issueArr);  // 只增字段（CTR-PL-02 的逐条可读原因）
    return env;
}

json toJson(const LoadResult& v) { return v.toJson(); }

// ---- 规则包视图 ----

json toJson(const AlertLevelDef& v) {
    json o = json::object();
    o["key"] = v.key;
    o["name"] = v.name;
    o["rank"] = v.rank;
    putOpt(o, "color", v.color);
    return o;
}

json toJson(const StormPolicy& v) {
    json o = json::object();
    o["enabled"] = v.enabled;
    o["windowMs"] = v.windowMs;
    o["maxRaised"] = v.maxRaised;
    o["level"] = v.level;
    o["title"] = v.title;
    o["countActive"] = v.countActive;
    return o;
}

json toJson(const MutePolicy& v) {
    json o = json::object();
    o["defaultMs"] = v.defaultMs;
    o["maxMs"] = v.maxMs;
    return o;
}

json toJson(const AlertRulesInfo& v) {
    json o = json::object();
    o["loaded"] = v.loaded;
    o["policiesNamespace"] = v.policiesNamespace;
    o["schemaVersion"] = v.schemaVersion;
    o["digest"] = v.digest;
    o["ruleCount"] = v.ruleCount;
    o["levelCount"] = v.levelCount;
    o["disabledCount"] = v.disabledCount;
    o["suppressionEdges"] = v.suppressionEdges;
    o["stormEnabled"] = v.stormEnabled;
    o["muteDefaultMs"] = v.muteDefaultMs;
    o["levelKeys"] = strArray(v.levelKeys);
    o["ruleIds"] = strArray(v.ruleIds);
    o["warnings"] = strArray(v.warnings);
    return o;
}

// ---- 观测与条件 ----

json toJson(const Observation& v) {
    json o = json::object();
    o["ruleId"] = v.ruleId;
    putOpt(o, "entityId", v.entityId);
    putOpt(o, "missionId", v.missionId);
    o["metric"] = v.metric;
    o["value"] = v.value;
    o["ts"] = v.ts;
    return o;
}

json Observation::toJson() const { return alert_engine::toJson(*this); }

namespace detail {

json conditionJson(const Condition& c) {
    json o = json::object();
    o["type"] = toString(c.type);
    putOpt(o, "field", c.field);
    if (c.type == ConditionType::Threshold || c.type == ConditionType::Recovery) {
        o["operator"] = toString(c.op);
        if (c.hasTextValue) {
            o["value"] = c.textValue;
        } else if (c.op != CompareOp::Exists && c.op != CompareOp::Missing) {
            o["value"] = c.value;
        }
        if (!c.values.empty()) o["values"] = strArray(c.values);
    }
    if (c.type == ConditionType::Duration) {
        o["forMs"] = c.forMs;
        if (c.inner) o["condition"] = conditionJson(*c.inner);
    }
    if (c.type == ConditionType::Bool) {
        o["logic"] = c.logic;
        if (c.logic == "not") {
            if (c.child) o["condition"] = conditionJson(*c.child);
        } else {
            json arr = json::array();
            for (const auto& ch : c.children) arr.push_back(conditionJson(ch));
            o["conditions"] = std::move(arr);
        }
    }
    if (c.type == ConditionType::Recovery && c.child) o["condition"] = conditionJson(*c.child);
    return o;
}

}  // namespace detail

json Condition::toJson() const { return detail::conditionJson(*this); }

json toJson(const Condition& v) { return detail::conditionJson(v); }

// ---- 订阅与消音 ----

json toJson(const AlertSubscription& v) {
    json o = json::object();
    o["subscriberId"] = v.subscriberId;
    o["ruleIds"] = strArray(v.ruleIds);
    o["levels"] = strArray(v.levels);
    o["entityIds"] = strArray(v.entityIds);
    o["missionIds"] = strArray(v.missionIds);
    o["includeRecovery"] = v.includeRecovery;
    o["includeAggregate"] = v.includeAggregate;
    return o;
}

json toJson(const MuteEntry& v) {
    json o = json::object();
    o["muteId"] = v.muteId;
    o["scope"] = toString(v.scope);
    o["key"] = v.key;
    putOpt(o, "level", v.level);
    o["from"] = v.from;
    o["until"] = v.until;
    o["durationMs"] = v.until - v.from;
    putOpt(o, "operatorId", v.operatorId);
    putOpt(o, "reason", v.reason);
    o["expired"] = v.expired;
    return o;
}

json MuteEntry::toJson() const { return alert_engine::toJson(*this); }

json MuteResult::toJson() const {
    json data = json::object();
    data["mute"] = alert_engine::toJson(mute);
    json env = json::object();
    env["code"] = code;
    env["message"] = message;
    env["data"] = std::move(data);
    return env;
}

// ---- 自述与观测 ----

json toJson(const Capabilities& v) {
    json o = json::object();
    o["rulesLoaded"] = v.rulesLoaded;
    o["pureMemory"] = v.pureMemory;
    o["storeInjected"] = v.storeInjected;
    o["clockInjected"] = v.clockInjected;
    o["sinkInjected"] = v.sinkInjected;
    o["logInjected"] = v.logInjected;
    o["ruleCount"] = v.ruleCount;
    o["levelCount"] = v.levelCount;
    o["subscriptionCount"] = v.subscriptionCount;
    o["stormEnabled"] = v.stormEnabled;
    o["maxActiveAlerts"] = v.maxActiveAlerts;
    o["retentionMs"] = v.retentionMs;
    o["alertCountBasis"] = v.alertCountBasis;
    o["engineVersion"] = v.engineVersion;
    o["policiesNamespace"] = v.policiesNamespace;
    o["schemaVersion"] = v.schemaVersion;
    o["policiesDigest"] = v.policiesDigest;
    return o;
}

json Capabilities::toJson() const { return alert_engine::toJson(*this); }

json toJson(const Metrics& v) {
    json o = json::object();
    o["observations"] = v.observations;
    o["rawRaises"] = v.rawRaises;
    o["alertsRaised"] = v.alertsRaised;
    o["merged"] = v.merged;
    o["suppressed"] = v.suppressed;
    o["suppressedByParent"] = v.suppressedByParent;
    o["mutedSuppressed"] = v.mutedSuppressed;
    o["durationPending"] = v.durationPending;
    o["flappedBlocked"] = v.flappedBlocked;
    o["upgrades"] = v.upgrades;
    o["recoveries"] = v.recoveries;
    o["autoClosed"] = v.autoClosed;
    o["aggregates"] = v.aggregates;
    o["folded"] = v.folded;
    o["idempotentHits"] = v.idempotentHits;
    o["rejected"] = v.rejected;
    o["notFound"] = v.notFound;
    o["storeErrors"] = v.storeErrors;
    o["sinkErrors"] = v.sinkErrors;
    o["unknownFields"] = v.unknownFields;
    o["truncations"] = v.truncations;
    o["evicted"] = v.evicted;
    o["clockRegressions"] = v.clockRegressions;
    return o;
}

json Metrics::toJson() const { return alert_engine::toJson(*this); }

}  // namespace alert_engine
