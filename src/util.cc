// src/util.cc —— 纯工具：标识符规则 / 摘要 / 规范化 JSON / 模板填充 / 错误码短名 /
//                枚举 ⇄ 字符串
//
// 契约依据：protocol.md §3.2（码表，1001 保留不用）/ §6（反向接口命名）；
//           冲突裁决 C15（幂等成功 = code 0 + data.idempotent）/ C16（1002 = 冲突拒绝）。
//
// 本文件不含任何业务取值（P6 / P7）。
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "internal.h"

namespace alert_engine {

// ============================================================================
// 时钟
// ============================================================================

int64_t SystemClock::nowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ============================================================================
// 错误码短名（码表逐值对齐 protocol.md §3.2）
// ============================================================================

const char* errorCodeName(int code) {
    switch (code) {
        case 0: return "ok";                   // 唯一成功码（含幂等命中，CTR-EC-01）
        case 1000: return "bad-request";       // 请求参数错误 / 规则包形状非法
        case 1002: return "conflict";          // 冲突拒绝（HTTP 409，C16）
        case 1003: return "unmet";             // 前置条件未满足：非法状态迁移
        case 1004: return "not-found";         // 未知告警 id / 未知规则 id
        case 1005: return "internal";          // 服务端执行失败（含 store 写失败）
        case 1006: return "version-mismatch";  // 规则包 MAJOR 不受支持
        default: return "unknown";
    }
}

const char* AlertEngine::errorCodeName(int code) { return alert_engine::errorCodeName(code); }

// ============================================================================
// 枚举 ⇄ 字符串（取值冻结；非法取值 → 空串 / nullopt，MUST NOT 猜）
// ============================================================================

const char* toString(ResultStatus s) {
    switch (s) {
        case ResultStatus::Ok: return "ok";
        case ResultStatus::Rejected: return "rejected";
        case ResultStatus::AlreadyApplied: return "already-applied";
    }
    return "";
}

const char* toString(AlertState s) {
    switch (s) {
        case AlertState::Active: return "active";
        case AlertState::Acknowledged: return "acknowledged";
        case AlertState::Closed: return "closed";
    }
    return "";
}

const char* toString(ConditionType t) {
    switch (t) {
        case ConditionType::Threshold: return "threshold";
        case ConditionType::Duration: return "duration";
        case ConditionType::Bool: return "bool";
        case ConditionType::Recovery: return "recovery";
    }
    return "";
}

const char* toString(CompareOp op) {
    switch (op) {
        case CompareOp::Eq: return "eq";
        case CompareOp::Ne: return "ne";
        case CompareOp::Gt: return "gt";
        case CompareOp::Gte: return "gte";
        case CompareOp::Lt: return "lt";
        case CompareOp::Lte: return "lte";
        case CompareOp::Exists: return "exists";
        case CompareOp::Missing: return "missing";
        case CompareOp::In: return "in";
        case CompareOp::NotIn: return "notIn";
    }
    return "";
}

const char* toString(MergeMode m) {
    switch (m) {
        case MergeMode::Accumulate: return "accumulate";
        case MergeMode::Latest: return "latest";
        case MergeMode::Max: return "max";
        case MergeMode::Keep: return "keep";
    }
    return "";
}

const char* toString(RecoveryMode m) {
    switch (m) {
        case RecoveryMode::None: return "none";
        case RecoveryMode::Record: return "record";
        case RecoveryMode::Close: return "close";
    }
    return "";
}

const char* toString(MuteScope s) {
    switch (s) {
        case MuteScope::Rule: return "rule";
        case MuteScope::Entity: return "entity";
    }
    return "";
}

std::optional<AlertState> alertStateFromString(const std::string& s) {
    if (s == "active") return AlertState::Active;
    if (s == "acknowledged") return AlertState::Acknowledged;
    if (s == "closed") return AlertState::Closed;
    return std::nullopt;
}

std::optional<CompareOp> compareOpFromString(const std::string& s) {
    if (s == "eq") return CompareOp::Eq;
    if (s == "ne") return CompareOp::Ne;
    if (s == "gt") return CompareOp::Gt;
    if (s == "gte") return CompareOp::Gte;
    if (s == "lt") return CompareOp::Lt;
    if (s == "lte") return CompareOp::Lte;
    if (s == "exists") return CompareOp::Exists;
    if (s == "missing") return CompareOp::Missing;
    if (s == "in") return CompareOp::In;
    if (s == "notIn") return CompareOp::NotIn;
    return std::nullopt;
}

std::optional<MergeMode> mergeModeFromString(const std::string& s) {
    if (s == "accumulate") return MergeMode::Accumulate;
    if (s == "latest") return MergeMode::Latest;
    if (s == "max") return MergeMode::Max;
    if (s == "keep") return MergeMode::Keep;
    return std::nullopt;
}

std::optional<RecoveryMode> recoveryModeFromString(const std::string& s) {
    if (s == "none") return RecoveryMode::None;
    if (s == "record") return RecoveryMode::Record;
    if (s == "close") return RecoveryMode::Close;
    return std::nullopt;
}

std::optional<ConditionType> conditionTypeFromString(const std::string& s) {
    if (s == "threshold") return ConditionType::Threshold;
    if (s == "duration") return ConditionType::Duration;
    if (s == "bool") return ConditionType::Bool;
    if (s == "recovery") return ConditionType::Recovery;
    return std::nullopt;
}

std::optional<MuteScope> muteScopeFromString(const std::string& s) {
    if (s == "rule") return MuteScope::Rule;
    if (s == "entity") return MuteScope::Entity;
    return std::nullopt;
}

namespace detail {

// ============================================================================
// 标识符规则（与 phase-engine 的 gate id 同口径）
// ============================================================================

bool isIdentifier(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool isSubscriberId(const std::string& s) { return isIdentifier(s); }

// ============================================================================
// FNV-1a 64 位摘要（零外部依赖；同一份字节 → 同一 digest —— CTR-PL-06）
// ============================================================================

std::string fnv1a64(const std::string& bytes) {
    uint64_t h = 1469598103934665603ULL;  // offset basis
    for (unsigned char c : bytes) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;  // prime
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

/// 规范化：对象键按 ASCII 升序、数组保序、无空白；数字用最短表示（整数不带 `.0`）。
static void canonicalInto(const json& v, std::string& out) {
    if (v.is_object()) {
        std::vector<std::string> keys;
        keys.reserve(v.size());
        for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        out += '{';
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i != 0) out += ',';
            out += json(keys[i]).dump();
            out += ':';
            canonicalInto(v[keys[i]], out);
        }
        out += '}';
        return;
    }
    if (v.is_array()) {
        out += '[';
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i != 0) out += ',';
            canonicalInto(v[i], out);
        }
        out += ']';
        return;
    }
    if (v.is_number_integer()) {
        out += std::to_string(v.get<int64_t>());
        return;
    }
    if (v.is_number_unsigned()) {
        out += std::to_string(v.get<uint64_t>());
        return;
    }
    if (v.is_number_float()) {
        // 最短表示：整数取值的浮点去掉 `.0`（同一份规则包 → 同一 digest）
        const double d = v.get<double>();
        if (d == static_cast<double>(static_cast<int64_t>(d))) {
            out += std::to_string(static_cast<int64_t>(d));
            return;
        }
        out += v.dump();
        return;
    }
    out += v.dump();
}

std::string canonicalJson(const json& v) {
    std::string out;
    canonicalInto(v, out);
    return out;
}

std::string policiesDigestOf(const json& pkg) { return fnv1a64(canonicalJson(pkg)); }

// ============================================================================
// 逐条问题 / 版本 / 模板
// ============================================================================

LoadIssue makeIssue(const std::string& path, const std::string& field, const std::string& reason) {
    LoadIssue i;
    i.path = path;
    i.field = field;
    i.reason = reason;
    return i;
}

bool parseSemver(const std::string& s, int& major, int& minor, int& patch) {
    int a = 0, b = 0, c = 0;
    char tail = '\0';
    if (std::sscanf(s.c_str(), "%d.%d.%d%c", &a, &b, &c, &tail) != 3) return false;
    if (a < 0 || b < 0 || c < 0) return false;
    major = a;
    minor = b;
    patch = c;
    return true;
}

std::string renderTemplate(const std::string& tpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tpl.size() + 32);
    std::size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] != '{') {
            out += tpl[i++];
            continue;
        }
        const std::size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos) {
            out += tpl.substr(i);  // 未闭合的 `{` 原样保留（MUST NOT 吞掉规则包文本）
            break;
        }
        const std::string key = tpl.substr(i + 1, close - i - 1);
        const auto it = vars.find(key);
        if (it == vars.end()) {
            out += tpl.substr(i, close - i + 1);  // 未知占位符原样保留（可读、可定位）
        } else {
            out += it->second;
        }
        i = close + 1;
    }
    return out;
}

std::string EngineCore::nextAlertId() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "a-%lld", static_cast<long long>(++nextAlertSeq));
    return std::string(buf);
}

std::string EngineCore::nextMuteId() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "m-%lld", static_cast<long long>(++nextMuteSeq));
    return std::string(buf);
}

void EngineCore::pruneMonitors(int64_t cutoff) {
    for (auto it = monitors.begin(); it != monitors.end();) {
        bool idle = true;
        for (const auto& kv : it->second.nodes) {
            if (kv.second.lastTs >= cutoff) {
                idle = false;
                break;
            }
        }
        if (idle) {
            it = monitors.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace detail

// ============================================================================
// ● 自由函数
// ============================================================================

std::string policiesDigest(const json& pkg) { return detail::policiesDigestOf(pkg); }

}  // namespace alert_engine
