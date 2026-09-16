// src/condition.cc —— 条件编译与求值（ALT-RULE-02 的四类机制）
//
//   · 阈值比较      `{"type":"threshold","field":…,"operator":…,"value":…}`
//   · 持续时间      `{"type":"duration","forMs":…,"condition":{…}}`   ← "持续 N 秒才报"
//   · 布尔组合      `{"type":"bool","logic":"allOf|anyOf|not","conditions":[…]|"condition":{…}}`
//   · 恢复          `{"type":"recovery","condition":{…}}`            ← "条件不再满足即为真"
//
// 三条实现口径：
//   1. **字段名一律来自规则包**：引擎只按 `values` → `context` → `metric` 的顺序取值，
//      不认识任何具体指标（P6 / P7 / ALT-RULE-01）。
//   2. **持续时间按节点累计**：每个节点有稳定路径 id（`c` / `c.0` / `c.0.1`），
//      连续性判定用观测时间戳（MUST NOT 取挂钟 —— ALT-NFR-02/03）。
//   3. **MUST NOT 抛异常跨边界**（P10）：非法条件在装载期即被拒绝（src/policies.cc），
//      求值期只做已编译结构的遍历。
#include <cmath>
#include <cstdlib>
#include <string>

#include "internal.h"

namespace alert_engine {
namespace detail {
namespace {

/// 数值比较的容差：浮点等值比较用（**不是业务阈值**，属比较机制的机器精度口径）
constexpr double kEpsilon = 1e-9;

bool numEq(double a, double b) { return std::fabs(a - b) <= kEpsilon; }

/// `values` 集合中是否含某数值（用于 in / notIn）
bool setContains(const std::vector<std::string>& values, double v) {
    for (const auto& s : values) {
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        if (end != nullptr && *end == '\0' && numEq(d, v)) return true;
    }
    return false;
}

bool setContainsText(const std::vector<std::string>& values, const std::string& v) {
    for (const auto& s : values) {
        if (s == v) return true;
    }
    return false;
}

/// 观测里该字段的**文本**取值（字符串比较用；数值字段转成最短表示）
std::string lookupText(const Observation& obs, const std::string& field, bool& found) {
    found = false;
    json v = json();
    if (obs.contextJson.empty()) return std::string();
    // contextJson 是保留位（结构化上下文的原始 JSON 文本）；当前实现只在其中查找字符串字段。
    try {
        v = json::parse(obs.contextJson);
    } catch (...) {
        return std::string();
    }
    if (!v.is_object() || !v.contains(field)) return std::string();
    found = true;
    const json& f = v[field];
    if (f.is_string()) return f.get<std::string>();
    return f.dump();
}

/// 阈值 / 恢复叶的算子判定
bool applyOp(const CompiledCondition& n, const Observation& obs) {
    const Condition& c = n.cond;
    double v = 0;
    bool found = false;
    const std::string field = c.field.empty() ? obs.metric : c.field;
    lookupField(obs, field, v, found);

    switch (c.op) {
        case CompareOp::Exists:
            return found;
        case CompareOp::Missing:
            return !found;
        case CompareOp::In:
            return found && setContains(c.values, v);
        case CompareOp::NotIn:
            return !found || !setContains(c.values, v);
        case CompareOp::Eq:
        case CompareOp::Ne: {
            if (c.hasTextValue) {
                bool tf = false;
                const std::string t = lookupText(obs, field, tf);
                const bool eq = tf && t == c.textValue;
                return c.op == CompareOp::Eq ? eq : !eq;
            }
            if (!found) return c.op == CompareOp::Ne;  // 缺失时 ne 视为成立（"不是这个值"）
            const bool eq = numEq(v, c.value);
            return c.op == CompareOp::Eq ? eq : !eq;
        }
        case CompareOp::Gt:
            return found && v > c.value && !numEq(v, c.value);
        case CompareOp::Gte:
            return found && (v > c.value || numEq(v, c.value));
        case CompareOp::Lt:
            return found && v < c.value && !numEq(v, c.value);
        case CompareOp::Lte:
            return found && (v < c.value || numEq(v, c.value));
    }
    return false;
}

}  // namespace

// ============================================================================
// 字段取值
// ============================================================================

bool lookupField(const Observation& obs, const std::string& field, double& out, bool& found) {
    found = false;
    if (field.empty()) return false;
    auto v = obs.values.find(field);
    if (v != obs.values.end()) {
        out = v->second;
        found = true;
        return true;
    }
    auto c = obs.context.find(field);
    if (c != obs.context.end()) {
        out = c->second;
        found = true;
        return true;
    }
    if (field == obs.metric) {
        out = obs.value;
        found = true;
        return true;
    }
    return false;
}

// ============================================================================
// 瞬时求值（公开纯函数用）
// ============================================================================

bool evalInstant(const CompiledCondition& n, const Observation& obs, bool& raw) {
    const Condition& c = n.cond;
    switch (c.type) {
        case ConditionType::Threshold:
            raw = applyOp(n, obs);
            return raw;
        case ConditionType::Recovery: {
            // 恢复 = 原始条件不再满足（`child === null` 时按自身算子判定）。
            // 瞬时口径：`raw` 上报"原始条件当下是否成立"，`met` 上报"是否应产生恢复"。
            // 纯函数无法知道"曾经是否成立过"，因此**只按当前状态**判定（引擎内的
            // 版本由 `evalTracked` 用持久锚点补齐 —— ALT-GEN-04）。
            if (n.child) {
                bool childRaw = false;
                raw = evalInstant(*n.child, obs, childRaw);
                return !raw;
            }
            raw = applyOp(n, obs);
            return raw;
        }
        case ConditionType::Duration: {
            // 瞬时口径：Duration 只要求内部条件**当下成立**（时长累计是引擎内部状态，
            // 与观测序列有关，不属于"单点求值"的语义）
            if (!n.inner) {
                raw = false;
                return false;
            }
            raw = evalInstant(*n.inner, obs, raw);
            return raw;
        }
        case ConditionType::Bool: {
            if (c.logic == "not") {
                bool childRaw = false;
                const bool v = n.child ? evalInstant(*n.child, obs, childRaw) : false;
                raw = v;
                return !v;
            }
            raw = (c.logic == "allOf");
            if (c.logic != "allOf" && c.logic != "anyOf") {
                raw = false;
                return false;
            }
            for (const auto& ch : n.children) {
                bool childRaw = false;
                const bool v = evalInstant(ch, obs, childRaw);
                if (c.logic == "allOf" && !v) {
                    raw = false;
                    return false;
                }
                if (c.logic == "anyOf" && v) {
                    raw = true;
                    return true;
                }
            }
            return raw;
        }
    }
    raw = false;
    return false;
}

}  // namespace detail

// ============================================================================
// ● 公开纯函数：条件求值（瞬时口径）
// ============================================================================

namespace detail {

/// 把公开的 `Condition` 递归包成内部 `CompiledCondition`（节点 id 固定为 "c"）
CompiledCondition packCondition(const Condition& src) {
    CompiledCondition out;
    out.cond = src;
    out.nodeId = "c";
    for (const auto& ch : src.children) out.children.push_back(packCondition(ch));
    if (src.child) out.child = std::make_shared<CompiledCondition>(packCondition(*src.child));
    if (src.inner) out.inner = std::make_shared<CompiledCondition>(packCondition(*src.inner));
    return out;
}

}  // namespace detail

bool evaluateCondition(const Condition& c, const Observation& obs, bool* missing) {
    const detail::CompiledCondition root = detail::packCondition(c);
    bool raw = false;
    const bool met = detail::evalInstant(root, obs, raw);
    if (missing != nullptr) {
        // 只对被求值的根节点上报（"根字段缺失"是调用方最关心的诊断）
        double tmp = 0;
        bool found = true;
        if (c.type == ConditionType::Threshold && c.op != CompareOp::Exists &&
            c.op != CompareOp::Missing) {
            detail::lookupField(obs, c.field.empty() ? obs.metric : c.field, tmp, found);
        }
        *missing = !found;
    }
    return met;
}

namespace detail {

// ============================================================================
// 带持续时间累计的求值（引擎内部）
// ============================================================================

namespace {

/// 连续性缺口上限：两次观测间隔超过它即认为"中间没被观察到"，持续时间重新计时。
///
/// **缺省 = 不限**（返回 0）。理由：持续时间条件是"该条件连续成立多久"的语义，
/// 而"多长间隔算断档"是**宿主采样节拍**的性质，不是引擎能猜的业务参数。
/// 猜一个缺省会在时钟跳变（或宿主换了采样节拍）时静默丢掉持续时间计时。
/// 需要防"断档被误判为持续"的宿主，可在规则里显式给 `maxGapMs`。
int64_t gapLimitOf(const CompiledCondition& n, int64_t maxGapMs) {
    (void)n;
    return maxGapMs > 0 ? maxGapMs : 0;
}

}  // namespace

bool evalTracked(const CompiledCondition& n, const Observation& obs, MonitorState& st,
                 int64_t maxGapMs, bool* everSatisfied) {
    const std::string& id = n.nodeId;
    ConditionState& cs = st.nodes[id];
    bool met = false;

    switch (n.cond.type) {
        case ConditionType::Threshold: {
            bool found = false;
            double v = 0;
            const std::string field = n.cond.field.empty() ? obs.metric : n.cond.field;
            lookupField(obs, field, v, found);
            cs.hasVal = found;
            if (found) cs.val = v;
            met = applyOp(n, obs);
            break;
        }
        case ConditionType::Recovery: {
            if (n.child) {
                const bool childMet = evalTracked(*n.child, obs, st, maxGapMs, nullptr);
                // "条件不再满足" = 该条件**曾经成立过**（子节点 `durSatisfied` 是持久锚点，
                // 内部条件不成立时也保持置位）且现在不成立。只看"当前不成立"会在监控
                // 刚接入、条件从未成立过时误报恢复 —— 故必须两条件同时成立（ALT-GEN-04）。
                auto it = st.nodes.find(n.child->nodeId);
                const bool everHeld = it != st.nodes.end() && it->second.everMet;
                met = everHeld && !childMet;
            } else {
                met = applyOp(n, obs);
            }
            break;
        }
        case ConditionType::Duration: {
            bool innerMet = false;
            bool innerRaw = false;
            if (n.inner) {
                innerMet = evalTracked(*n.inner, obs, st, maxGapMs, nullptr);
                innerRaw = st.nodes[n.inner->nodeId].raw;
            }
            const int64_t limit = gapLimitOf(n, maxGapMs);
            const bool gapOk = (cs.lastTs == 0) || (limit <= 0) || (obs.ts - cs.lastTs <= limit);
            if (!innerMet) {
                cs.durStarted = false;
                cs.durFrom = 0;
                // `durSatisfied` **不清零**：它是"该条件曾经成立过"的持久锚点
                // （恢复节点的判定依据 —— ALT-GEN-04）。当前这一轮是否满足由
                // `durStarted && reached` 表达，与历史锚点分开，互不干扰。
                met = false;
            } else {
                if (!cs.durStarted || !gapOk) {
                    cs.durStarted = true;
                    cs.durFrom = obs.ts;
                    cs.durSatisfiedAt = 0;
                }
                const bool reached = (n.cond.forMs <= 0) || (obs.ts - cs.durFrom >= n.cond.forMs);
                if (reached) {
                    cs.durSatisfied = true;   // 持久锚点：置位后不再清（除非整体淘汰）
                    if (cs.durSatisfiedAt == 0) cs.durSatisfiedAt = obs.ts;
                }
                met = reached;
            }
            cs.raw = innerRaw;  // 恢复判定读的是"内部条件"，不受时长门槛影响
            break;
        }
        case ConditionType::Bool: {
            if (n.cond.logic == "not") {
                bool childMet = false;
                if (n.child) childMet = evalTracked(*n.child, obs, st, maxGapMs, nullptr);
                met = !childMet;
            } else if (n.cond.logic == "allOf") {
                met = true;
                // **不短路**：一次观测 MUST 更新全部子节点状态（否则持续时间会漏计时）
                for (const auto& ch : n.children) {
                    if (!evalTracked(ch, obs, st, maxGapMs, nullptr)) met = false;
                }
            } else if (n.cond.logic == "anyOf") {
                met = false;
                for (const auto& ch : n.children) {
                    if (evalTracked(ch, obs, st, maxGapMs, nullptr)) met = true;
                }
            } else {
                met = false;
            }
            break;
        }
    }

    cs.lastTs = obs.ts;
    cs.raw = met;
    if (met) cs.everMet = true;  // 持久锚点：任何条件类型"曾经成立过"都置位，不清零
    if (everSatisfied != nullptr) *everSatisfied = met;
    return met;
}

bool evalRawState(const CompiledCondition& n, const MonitorState& st) {
    for (const auto& kv : st.nodes) {
        if (kv.first == n.nodeId) return kv.second.raw;
    }
    return false;
}

// ============================================================================
// 条件编译（装载期；逐条可读原因，含路径与字段名 —— CTR-PL-02）
// ============================================================================

bool compileCondition(const json& j, const std::string& path, const std::string& nodeId,
                      CompiledCondition& out, std::vector<LoadIssue>& issues) {
    out.nodeId = nodeId;
    if (!j.is_object()) {
        issues.push_back(makeIssue(path, "", "条件 MUST 是对象"));
        return false;
    }
    const std::size_t before = issues.size();

    // ---- type ----
    const std::string type = j.value("type", std::string());
    if (type.empty()) {
        issues.push_back(makeIssue(path, "type",
                                   "缺少必填字段 type（取值：threshold|duration|bool|recovery）"));
        return false;
    }
    const auto ct = conditionTypeFromString(type);
    if (!ct) {
        issues.push_back(makeIssue(path, "type",
                                   "未知条件类型 \"" + type +
                                       "\"（取值：threshold|duration|bool|recovery）"));
        return false;
    }
    out.cond.type = *ct;

    // ---- 算子（threshold / recovery 使用） ----
    if (j.contains("operator")) {
        if (!j["operator"].is_string()) {
            issues.push_back(makeIssue(path, "operator", "operator MUST 是字符串"));
        } else {
            const auto op = compareOpFromString(j["operator"].get<std::string>());
            if (!op) {
                issues.push_back(makeIssue(path, "operator",
                                           "未知算子 \"" + j["operator"].get<std::string>() +
                                               "\"（取值：eq|ne|gt|gte|lt|lte|exists|missing|in|"
                                               "notIn）"));
            } else {
                out.cond.op = *op;
            }
        }
    }
    if (j.contains("field")) {
        if (!j["field"].is_string()) {
            issues.push_back(makeIssue(path, "field", "field MUST 是字符串"));
        } else {
            out.cond.field = j["field"].get<std::string>();
        }
    }
    if (j.contains("value")) {
        if (j["value"].is_number()) {
            out.cond.value = j["value"].get<double>();
        } else if (j["value"].is_string()) {
            out.cond.textValue = j["value"].get<std::string>();
            out.cond.hasTextValue = true;
        } else if (j["value"].is_boolean()) {
            // 布尔取值按 0/1 参与数值比较（规则包可用 true/false 表达"是/否"）
            out.cond.value = j["value"].get<bool>() ? 1.0 : 0.0;
        } else {
            issues.push_back(makeIssue(path, "value", "value MUST 是数值、字符串或布尔"));
        }
    }
    if (j.contains("values")) {
        if (!j["values"].is_array()) {
            issues.push_back(makeIssue(path, "values", "values MUST 是数组"));
        } else {
            for (const auto& e : j["values"]) {
                if (e.is_string()) {
                    out.cond.values.push_back(e.get<std::string>());
                } else if (e.is_number()) {
                    out.cond.values.push_back(e.dump());
                } else {
                    issues.push_back(makeIssue(path, "values", "values 元素 MUST 是字符串或数值"));
                }
            }
        }
    }

    switch (out.cond.type) {
        case ConditionType::Threshold:
            if (out.cond.op == CompareOp::In || out.cond.op == CompareOp::NotIn) {
                if (out.cond.values.empty()) {
                    issues.push_back(
                        makeIssue(path, "values", "算子 in / notIn MUST 给出非空 values 数组"));
                }
            }
            break;

        case ConditionType::Duration: {
            if (!j.contains("forMs")) {
                issues.push_back(makeIssue(path, "forMs", "缺少必填字段 forMs（持续时间毫秒）"));
            } else if (!j["forMs"].is_number_integer() && !j["forMs"].is_number_unsigned()) {
                issues.push_back(makeIssue(path, "forMs", "forMs MUST 是整数毫秒"));
            } else {
                out.cond.forMs = j["forMs"].get<int64_t>();
                if (out.cond.forMs < 0) {
                    issues.push_back(makeIssue(path, "forMs", "forMs MUST >= 0"));
                }
            }
            if (!j.contains("condition")) {
                issues.push_back(
                    makeIssue(path, "condition", "缺少必填字段 condition（持续时间作用的内部条件）"));
            } else {
                auto inner = std::make_shared<CompiledCondition>();
                if (compileCondition(j["condition"], path + ".condition", nodeId + ".inner", *inner,
                                     issues)) {
                    out.inner = inner;
                }
            }
            break;
        }

        case ConditionType::Recovery: {
            if (!j.contains("condition")) {
                issues.push_back(makeIssue(path, "condition",
                                           "缺少必填字段 condition（被监视的原始条件）"));
            } else {
                auto child = std::make_shared<CompiledCondition>();
                if (compileCondition(j["condition"], path + ".condition", nodeId + ".inner", *child,
                                     issues)) {
                    out.child = child;
                }
            }
            break;
        }

        case ConditionType::Bool: {
            const std::string logic = j.value("logic", std::string());
            if (logic != "allOf" && logic != "anyOf" && logic != "not") {
                issues.push_back(makeIssue(path, "logic",
                                           "bool 条件 MUST 给出 logic ∈ {allOf, anyOf, not}，实际为 "
                                           "\"" + logic + "\""));
                break;
            }
            out.cond.logic = logic;
            if (logic == "not") {
                if (!j.contains("condition")) {
                    issues.push_back(makeIssue(path, "condition", "logic=not MUST 给出 condition"));
                } else {
                    auto child = std::make_shared<CompiledCondition>();
                    if (compileCondition(j["condition"], path + ".condition", nodeId + ".inner",
                                         *child, issues)) {
                        out.child = child;
                    }
                }
            } else {
                if (!j.contains("conditions")) {
                    issues.push_back(
                        makeIssue(path, "conditions", "logic=" + logic + " MUST 给出 conditions 数组"));
                } else if (!j["conditions"].is_array()) {
                    issues.push_back(makeIssue(path, "conditions", "conditions MUST 是数组"));
                } else if (j["conditions"].empty()) {
                    issues.push_back(makeIssue(path, "conditions", "conditions MUST 非空"));
                } else {
                    int i = -1;
                    for (const auto& e : j["conditions"]) {
                        ++i;
                        CompiledCondition ch;
                        const std::string childNode =
                            nodeId + "." + std::to_string(i);
                        if (compileCondition(e, path + ".conditions[" + std::to_string(i) + "]",
                                             childNode, ch, issues)) {
                            out.children.push_back(std::move(ch));
                        }
                    }
                }
            }
            break;
        }
    }

    return issues.size() == before;
}

}  // namespace detail
}  // namespace alert_engine
