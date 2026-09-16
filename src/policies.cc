// src/policies.cc —— 规则包装载与校验（protocol.md §5；ALT-RULE-01/03/04/05）
//
// 本引擎消费的 `kind` 是 protocol.md §5.3 登记给 alert-engine 的 **`"alertRules"`**。
// 该 kind 的清单段是 `items[]`（§5.3 的"清单段"表）；`levels` / `storm` / `mute` 是
// **同 kind 的兄弟段**（与 `linkThresholds` 的 `states`/`hysteresis`、`entityTypes` 的
// `numbering` 同一惯例：内容从属于某个已登记 kind，与之同版本演进）。
//
// 校验口径（CTR-PL-01..06）：
//   · 失败 MUST 拒绝**整包**并给出**逐条**可读原因（含条目下标与字段名）—— CTR-PL-02
//   · 未知字段 MUST 被忽略并计入告警统计，MUST NOT 导致失败 —— CTR-PL-03
//   · 缺失可选字段 MUST 取引擎内置缺省，且缺省 MUST 在契约文档写明 —— CTR-PL-04
//   · 缺失必填字段 MUST 拒绝装载，原因指向具体条目与字段 —— CTR-PL-05
//   · 规则包 MUST NOT 含可执行代码（纯数据）—— CTR-PL-07
//
// **本文件不含任何业务取值**：级别 key/name、阈值、持续时间、去重窗口、抑制关系、
// 风暴上限、消音时长全部来自规则包（P6 / P7 / ALT-RULE-01）。
#include <algorithm>
#include <set>
#include <string>

#include "internal.h"

namespace alert_engine {
namespace detail {
namespace {

constexpr const char* kKindAlertRules = "alertRules";

/// 已知顶层段（CTR-PL-03：其余计入 warnings）
bool knownTopLevel(const std::string& k) {
    static const std::set<std::string> known = {"policiesNamespace", "schemaVersion", "kind",
                                                "levels", "items", "storm", "mute"};
    return known.count(k) != 0;
}

/// 已知规则条目字段（含兄弟段）
bool knownRuleField(const std::string& k) {
    static const std::set<std::string> known = {
        "key",           "id",            "version",        "enabled",
        "level",         "missionId",     "title",          "condition",
        "dedupWindowMs", "merge",         "recoverOn",      "suppressChildren",
        "suppress",      "parent",        "confirmCount",   "minDwellMs",
        "cooldownMs",    "maxGapMs",      "muteDefaultMs",  "suppressChildrenOf"};
    return known.count(k) != 0;
}

bool knownLevelField(const std::string& k) {
    static const std::set<std::string> known = {"key", "name", "rank", "color"};
    return known.count(k) != 0;
}

bool knownStormField(const std::string& k) {
    static const std::set<std::string> known = {"enabled", "windowMs", "maxRaised", "level",
                                               "title", "countActive"};
    return known.count(k) != 0;
}

bool knownMuteField(const std::string& k) {
    static const std::set<std::string> known = {"defaultMs", "maxMs"};
    return known.count(k) != 0;
}

/// 规则条目允许的字符串数组字段
bool stringArray(const json& j, std::vector<std::string>& out) {
    if (!j.is_array()) return false;
    for (const auto& e : j) {
        if (!e.is_string()) return false;
        out.push_back(e.get<std::string>());
    }
    return true;
}

using Issue = LoadIssue;
using Issues = std::vector<LoadIssue>;

void collectUnknown(const json& obj, const std::string& path,
                    const std::function<bool(const std::string&)>& known, std::vector<std::string>& w,
                    int64_t& unknownCount) {
    if (!obj.is_object()) return;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (known(it.key())) continue;
        ++unknownCount;
        w.push_back(path + "." + it.key() + "：未知字段，已忽略（CTR-PL-03）");
    }
}

}  // namespace

// ============================================================================
// 编译规则包（纯函数；validateRules 与 loadRules 共用 —— 避免两套口径）
// ============================================================================

CompiledRules compileRules(const json& pkg) {
    CompiledRules out;
    Issues& issues = out.result.issues;
    int64_t unknownCount = 0;
    std::set<std::string> seenIds;  // 规则 id 唯一性（跨 items 段与引用检查共用）

    if (!pkg.is_object() || pkg.empty()) {
        issues.push_back(makeIssue("", "", "规则包 MUST 是非空对象（骨架见 protocol.md §5.1）"));
        out.result.code = 1000;
        out.result.message = "规则包形状非法";
        return out;
    }

    // ---- 骨架：policiesNamespace / schemaVersion / kind（CTR-PL-01） ----
    const std::string ns = pkg.value("policiesNamespace", std::string());
    if (ns.empty()) {
        issues.push_back(makeIssue("", "policiesNamespace", "缺少必填字段 policiesNamespace"));
    } else if (!isIdentifier(ns)) {
        issues.push_back(makeIssue("", "policiesNamespace",
                                   "policiesNamespace 非法（期望 ^[a-z][a-z0-9-]{0,63}$）：" + ns));
    }

    const std::string ver = pkg.value("schemaVersion", std::string());
    int major = 0, minor = 0, patch = 0;
    bool verOk = false;
    if (ver.empty()) {
        issues.push_back(makeIssue("", "schemaVersion", "缺少必填字段 schemaVersion"));
    } else if (!parseSemver(ver, major, minor, patch)) {
        issues.push_back(makeIssue("", "schemaVersion",
                                   "schemaVersion MUST 为 MAJOR.MINOR.PATCH，实际为 \"" + ver + "\""));
    } else {
        verOk = true;
    }

    const std::string kind = pkg.value("kind", std::string());
    if (kind.empty()) {
        issues.push_back(makeIssue("", "kind", "缺少必填字段 kind"));
    } else if (kind != kKindAlertRules) {
        issues.push_back(makeIssue("", "kind",
                                   "kind MUST 为 \"" + std::string(kKindAlertRules) +
                                       "\"（protocol.md §5.3 登记给 alert-engine 的取值），实际为 \"" +
                                       kind + "\""));
    }

    // MAJOR 不匹配 → 1006（MUST NOT 静默降级）。
    // 注意：**必须收集完全部问题再决定 code**（CTR-PL-02：不短路），
    // 但 MAJOR 是"版本级"结论，单独记下来优先返回。
    bool majorMismatch = verOk && (major != kSupportedPoliciesMajor);
    if (majorMismatch) {
        issues.push_back(makeIssue(
            "", "schemaVersion",
            "规则包 MAJOR=" + std::to_string(major) + " 不受支持（本引擎支持 MAJOR=" +
                std::to_string(kSupportedPoliciesMajor) + "）；MUST NOT 静默降级（protocol.md §5.2）"));
    }

    // ---- levels 段：级别目录（ALT-GEN-02：取值可扩展，MUST NOT 硬编码三级） ----
    if (!pkg.contains("levels")) {
        issues.push_back(makeIssue("", "levels",
                                   "缺少必填段 levels（级别目录：key/name/rank；取值与数量均可扩展）"));
    } else if (!pkg["levels"].is_array()) {
        issues.push_back(makeIssue("", "levels", "levels MUST 是数组"));
    } else {
        int i = -1;
        for (const auto& e : pkg["levels"]) {
            ++i;
            const std::string path = "levels[" + std::to_string(i) + "]";
            if (!e.is_object()) {
                issues.push_back(makeIssue(path, "", "级别条目 MUST 是对象"));
                continue;
            }
            collectUnknown(e, path, knownLevelField, out.warnings, unknownCount);
            AlertLevelDef def;
            def.key = e.value("key", std::string());
            def.name = e.value("name", std::string());
            def.color = e.value("color", std::string());
            if (def.key.empty()) {
                issues.push_back(makeIssue(path, "key", "缺少必填字段 key（级别取值）"));
                continue;
            }
            if (!isIdentifier(def.key)) {
                issues.push_back(makeIssue(path, "key",
                                           "级别 key 非法（期望 ^[a-z][a-z0-9-]{0,63}$）：" + def.key));
                continue;
            }
            if (def.name.empty()) {
                issues.push_back(makeIssue(path, "name", "缺少必填字段 name（展示名）"));
            }
            if (!e.contains("rank")) {
                issues.push_back(makeIssue(path, "rank", "缺少必填字段 rank（升级比较位次）"));
                continue;
            }
            if (!e["rank"].is_number_integer() && !e["rank"].is_number_unsigned()) {
                issues.push_back(makeIssue(path, "rank", "rank MUST 是整数"));
                continue;
            }
            def.rank = e["rank"].get<int>();
            if (out.levelRank.count(def.key) != 0) {
                issues.push_back(makeIssue(path, "key", "级别 key 重复：" + def.key));
                continue;
            }
            for (const auto& existing : out.levels) {
                if (existing.rank == def.rank) {
                    issues.push_back(makeIssue(path, "rank",
                                               "rank 重复（" + std::to_string(def.rank) +
                                                   "）：升级比较依赖位次唯一"));
                }
            }
            out.levelRank[def.key] = def.rank;
            out.levels.push_back(std::move(def));
        }
        // rank 非降序（确定性输出；升级语义依赖它）
        std::stable_sort(out.levels.begin(), out.levels.end(),
                         [](const AlertLevelDef& a, const AlertLevelDef& b) {
                             return a.rank < b.rank;
                         });
    }

    // ---- items 段：规则清单（protocol.md §5.3 的清单段） ----
    if (!pkg.contains("items")) {
        issues.push_back(makeIssue("", "items",
                                   "缺少必填段 items（protocol.md §5.3：alertRules 的清单段）"));
    } else if (!pkg["items"].is_array()) {
        issues.push_back(makeIssue("", "items", "items MUST 是数组"));
    } else if (pkg["items"].empty()) {
        issues.push_back(makeIssue("", "items", "items MUST 非空（没有任何规则的规则包无意义）"));
    } else {
        int i = -1;
        for (const auto& e : pkg["items"]) {
            ++i;
            const std::string path = "items[" + std::to_string(i) + "]";
            if (!e.is_object()) {
                issues.push_back(makeIssue(path, "", "规则条目 MUST 是对象"));
                continue;
            }
            collectUnknown(e, path, knownRuleField, out.warnings, unknownCount);

            RuleRuntime rt;
            AlertRuleDef& d = rt.def;
            // `key` 与 `id` 同义（protocol.md §5.5 的骨架用 `key`；需求专篇用"规则 id"）
            d.id = e.value("key", std::string());
            if (d.id.empty()) d.id = e.value("id", std::string());
            if (d.id.empty()) {
                issues.push_back(makeIssue(path, "key", "缺少必填字段 key（规则 id）"));
                continue;
            }
            if (!isIdentifier(d.id)) {
                issues.push_back(makeIssue(path, "key",
                                           "规则 id 非法（期望 ^[a-z][a-z0-9-]{0,63}$）：" + d.id));
                continue;
            }
            if (seenIds.count(d.id) != 0) {
                issues.push_back(makeIssue(path, "key", "规则 id 重复：" + d.id));
                continue;
            }
            seenIds.insert(d.id);

            // 汇总告警的保留 id MUST NOT 被规则包占用（否则汇总告警与业务规则冲突）
            if (d.id == kAggregateRuleId) {
                issues.push_back(makeIssue(path, "key",
                                           std::string("规则 id MUST NOT 使用保留 id \"") +
                                               kAggregateRuleId + "\"（风暴汇总告警专用）"));
                continue;
            }

            d.version = e.value("version", std::string());
            d.enabled = e.value("enabled", true);
            d.level = e.value("level", std::string());
            d.missionId = e.value("missionId", std::string());
            rt.titleTemplate = e.value("title", std::string());

            if (d.level.empty()) {
                issues.push_back(makeIssue(path, "level", "缺少必填字段 level（级别取值）"));
            } else if (out.levelRank.count(d.level) == 0) {
                issues.push_back(makeIssue(path, "level",
                                           "level \"" + d.level + "\" 不在 levels 目录内（取值由规则包"
                                           "声明，引擎 MUST NOT 内建级别）"));
            }

            // ---- 条件（ALT-RULE-02 四类） ----
            if (!e.contains("condition")) {
                issues.push_back(makeIssue(path, "condition", "缺少必填字段 condition"));
            } else {
                compileCondition(e["condition"], path + ".condition", "c", rt.condition, issues);
            }

            // ---- 去重窗口与合并策略（ALT-DEDUP-01/02） ----
            if (e.contains("dedupWindowMs")) {
                if (!e["dedupWindowMs"].is_number_integer() &&
                    !e["dedupWindowMs"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "dedupWindowMs", "dedupWindowMs MUST 是整数毫秒"));
                } else {
                    d.dedupWindowMs = e["dedupWindowMs"].get<int64_t>();
                    if (d.dedupWindowMs < 0) {
                        issues.push_back(
                            makeIssue(path, "dedupWindowMs", "dedupWindowMs MUST >= 0（0 = 不合并）"));
                    }
                }
            }
            if (e.contains("merge")) {
                if (!e["merge"].is_string()) {
                    issues.push_back(makeIssue(path, "merge", "merge MUST 是字符串"));
                } else {
                    const auto m = mergeModeFromString(e["merge"].get<std::string>());
                    if (!m) {
                        issues.push_back(makeIssue(path, "merge",
                                                   "未知合并策略 \"" + e["merge"].get<std::string>() +
                                                       "\"（取值：accumulate|latest|max|keep）"));
                    } else {
                        d.merge = *m;
                    }
                }
            }
            if (e.contains("recoverOn")) {
                if (!e["recoverOn"].is_string()) {
                    issues.push_back(makeIssue(path, "recoverOn", "recoverOn MUST 是字符串"));
                } else {
                    const auto m = recoveryModeFromString(e["recoverOn"].get<std::string>());
                    if (!m) {
                        issues.push_back(
                            makeIssue(path, "recoverOn",
                                      "未知恢复策略 \"" + e["recoverOn"].get<std::string>() +
                                          "\"（取值：none|record|close）"));
                    } else {
                        d.recoverOn = *m;
                    }
                }
            }

            // ---- 抑制关系（ALT-DEDUP-05） ----
            d.suppressChildren = e.value("suppressChildren", false);
            d.parent = e.value("parent", std::string());
            if (e.contains("suppress")) {
                if (!e["suppress"].is_boolean()) {
                    issues.push_back(makeIssue(path, "suppress", "suppress MUST 是布尔"));
                } else {
                    d.suppressChildren = e["suppress"].get<bool>();
                }
            }
            if (e.contains("suppressChildrenOf")) {
                std::vector<std::string> tmp;
                if (!stringArray(e["suppressChildrenOf"], tmp)) {
                    issues.push_back(
                        makeIssue(path, "suppressChildrenOf", "suppressChildrenOf MUST 是字符串数组"));
                } else {
                    d.suppressChildrenOf = std::move(tmp);
                }
            }
            if (!d.parent.empty() && !isIdentifier(d.parent)) {
                issues.push_back(makeIssue(path, "parent", "parent 非法（规则 id 形状）：" + d.parent));
            }

            // ---- 抖动抑制参数（ALT-DEDUP-03） ----
            if (e.contains("confirmCount")) {
                if (!e["confirmCount"].is_number_integer() && !e["confirmCount"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "confirmCount", "confirmCount MUST 是整数"));
                } else {
                    d.confirmCount = e["confirmCount"].get<int>();
                    if (d.confirmCount < 1) {
                        issues.push_back(makeIssue(path, "confirmCount", "confirmCount MUST >= 1"));
                    }
                }
            }
            if (e.contains("minDwellMs")) {
                if (!e["minDwellMs"].is_number_integer() && !e["minDwellMs"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "minDwellMs", "minDwellMs MUST 是整数毫秒"));
                } else {
                    d.minDwellMs = e["minDwellMs"].get<int>();
                    if (d.minDwellMs < 0) {
                        issues.push_back(makeIssue(path, "minDwellMs", "minDwellMs MUST >= 0"));
                    }
                }
            }
            if (e.contains("cooldownMs")) {
                if (!e["cooldownMs"].is_number_integer() && !e["cooldownMs"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "cooldownMs", "cooldownMs MUST 是整数毫秒"));
                } else {
                    d.cooldownMs = e["cooldownMs"].get<int64_t>();
                    if (d.cooldownMs < 0) {
                        issues.push_back(makeIssue(path, "cooldownMs", "cooldownMs MUST >= 0"));
                    }
                }
            }
            if (e.contains("maxGapMs")) {
                if (!e["maxGapMs"].is_number_integer() && !e["maxGapMs"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "maxGapMs", "maxGapMs MUST 是整数毫秒"));
                } else {
                    d.maxGapMs = e["maxGapMs"].get<int64_t>();
                    if (d.maxGapMs < 0) {
                        issues.push_back(makeIssue(path, "maxGapMs", "maxGapMs MUST >= 0（0 = 自动）"));
                    }
                }
            }
            if (e.contains("muteDefaultMs")) {
                // 兼容写法：字符串毫秒；段级缺省仍在 `mute.defaultMs`
                if (!e["muteDefaultMs"].is_number_integer() && !e["muteDefaultMs"].is_number_unsigned()) {
                    issues.push_back(makeIssue(path, "muteDefaultMs", "muteDefaultMs MUST 是整数毫秒"));
                }
            }

            rt.enabled = d.enabled;
            out.info.ruleIds.push_back(d.id);
            out.rules.push_back(std::move(rt));
        }
    }

    // ---- 引用完整性：parent / suppressChildrenOf 指向的规则 MUST 存在（CTR-PL-01/08） ----
    for (const auto& rt : out.rules) {
        const std::string path = "items[" + rt.def.id + "]";
        if (!rt.def.parent.empty() && seenIds.count(rt.def.parent) == 0) {
            issues.push_back(makeIssue(path, "parent",
                                       "parent 指向不存在的规则 id：" + rt.def.parent));
        }
        for (const auto& child : rt.def.suppressChildrenOf) {
            if (seenIds.count(child) == 0) {
                issues.push_back(makeIssue(path, "suppressChildrenOf",
                                           "指向不存在的规则 id：" + child));
            }
        }
    }

    // ---- 抑制图（父规则 → 子规则）：无环检查 ----
    std::map<std::string, std::vector<std::string>> edges;
    for (const auto& rt : out.rules) {
        const std::string& id = rt.def.id;
        if (!rt.def.parent.empty() && seenIds.count(rt.def.parent) != 0) {
            edges[rt.def.parent].push_back(id);
        }
        if (rt.def.suppressChildren) {
            for (const auto& other : out.rules) {
                if (other.def.id == id) continue;        // 自身不算子告警（否则自环）
                if (other.def.parent == id) continue;    // 已由 parent 声明
                edges[id].push_back(other.def.id);
            }
        }
        for (const auto& child : rt.def.suppressChildrenOf) {
            if (child == id) continue;                   // 自环同样忽略
            if (seenIds.count(child) != 0) edges[id].push_back(child);
        }
    }
    // 去重
    for (auto& kv : edges) {
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
    }
    // 环检测（DFS 三色）
    {
        std::map<std::string, int> color;  // 0=白 1=灰 2=黑
        std::vector<std::string> stack;
        std::function<bool(const std::string&)> dfs = [&](const std::string& n) -> bool {
            color[n] = 1;
            stack.push_back(n);
            for (const auto& m : edges[n]) {
                if (color[m] == 1) {
                    std::string chain;
                    bool in = false;
                    for (const auto& s : stack) {
                        if (s == m) in = true;
                        if (in) chain += s + " → ";
                    }
                    chain += m;
                    issues.push_back(makeIssue("suppress", "",
                                               "抑制关系存在环：" + chain +
                                                   "（父告警抑制子告警 MUST 是有向无环关系）"));
                    return true;
                }
                if (color[m] == 0 && dfs(m)) return true;
            }
            stack.pop_back();
            color[n] = 2;
            return false;
        };
        for (const auto& rt : out.rules) {
            if (color[rt.def.id] == 0 && dfs(rt.def.id)) break;
        }
    }
    out.suppressedBy = edges;

    // ---- storm 段（ALT-DEDUP-06） ----
    if (pkg.contains("storm")) {
        const json& s = pkg["storm"];
        if (!s.is_object()) {
            issues.push_back(makeIssue("storm", "", "storm MUST 是对象"));
        } else {
            collectUnknown(s, "storm", knownStormField, out.warnings, unknownCount);
            out.storm.policy.enabled = s.value("enabled", false);
            out.storm.policy.countActive = s.value("countActive", false);
            out.storm.policy.level = s.value("level", std::string());
            out.storm.policy.title = s.value("title", std::string());
            out.storm.policy.windowMs = s.value("windowMs", static_cast<int64_t>(0));
            out.storm.policy.maxRaised = s.value("maxRaised", 0);
            if (out.storm.policy.enabled) {
                if (out.storm.policy.windowMs <= 0) {
                    issues.push_back(makeIssue("storm", "windowMs",
                                               "storm.enabled=true 时 windowMs MUST > 0"));
                }
                if (out.storm.policy.maxRaised <= 0) {
                    issues.push_back(makeIssue("storm", "maxRaised",
                                               "storm.enabled=true 时 maxRaised MUST > 0"));
                }
                if (out.storm.policy.level.empty()) {
                    issues.push_back(makeIssue("storm", "level",
                                               "storm.enabled=true 时 MUST 给出汇总告警的 level"));
                } else if (out.levelRank.count(out.storm.policy.level) == 0) {
                    issues.push_back(makeIssue("storm", "level",
                                               "level \"" + out.storm.policy.level +
                                                   "\" 不在 levels 目录内"));
                }
                if (out.storm.policy.title.empty()) {
                    issues.push_back(makeIssue("storm", "title",
                                               "storm.enabled=true 时 MUST 给出汇总告警标题模板"));
                }
            }
            out.storm.enabled = out.storm.policy.enabled;
        }
    } else {
        out.storm.enabled = false;  // 缺省 = 不折叠（CTR-PL-04）
    }

    // ---- mute 段（ALT-ACK-04） ----
    if (pkg.contains("mute")) {
        const json& m = pkg["mute"];
        if (!m.is_object()) {
            issues.push_back(makeIssue("mute", "", "mute MUST 是对象"));
        } else {
            collectUnknown(m, "mute", knownMuteField, out.warnings, unknownCount);
            out.mutePolicy.defaultMs = m.value("defaultMs", static_cast<int64_t>(0));
            out.mutePolicy.maxMs = m.value("maxMs", static_cast<int64_t>(0));
            if (out.mutePolicy.defaultMs < 0) {
                issues.push_back(makeIssue("mute", "defaultMs", "mute.defaultMs MUST >= 0"));
            }
            if (out.mutePolicy.maxMs < 0) {
                issues.push_back(makeIssue("mute", "maxMs", "mute.maxMs MUST >= 0"));
            }
        }
    }

    // ---- 未知顶层段（CTR-PL-03：忽略 + 计入告警） ----
    collectUnknown(pkg, "", knownTopLevel, out.warnings, unknownCount);

    // ---- 收口 ----
    out.policiesNamespace = ns;
    out.schemaVersion = ver;
    out.digest = policiesDigestOf(pkg);
    out.ok = issues.empty();
    out.result.code = out.ok ? 0 : (majorMismatch ? 1006 : 1000);
    out.result.message = out.ok ? "ok" : (majorMismatch ? "规则包 MAJOR 不受支持" : "规则包校验失败");
    out.info.loaded = out.ok;
    out.info.policiesNamespace = ns;
    out.info.schemaVersion = ver;
    out.info.digest = out.digest;
    out.info.ruleCount = static_cast<int>(out.rules.size());
    out.info.levelCount = static_cast<int>(out.levels.size());
    out.info.stormEnabled = out.storm.enabled;
    out.info.muteDefaultMs = out.mutePolicy.defaultMs;
    out.info.warnings = out.warnings;
    out.info.suppressionEdges = 0;
    for (const auto& kv : edges) out.info.suppressionEdges += static_cast<int>(kv.second.size());
    for (const auto& l : out.levels) out.info.levelKeys.push_back(l.key);
    for (const auto& r : out.rules) {
        if (!r.def.enabled) ++out.info.disabledCount;
    }
    out.result.data = out.info;
    return out;
}

}  // namespace detail
}  // namespace alert_engine
