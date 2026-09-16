// tests/selftest.cc —— 零依赖自测（手写断言，不引任何测试框架）
//
// 覆盖两套口径：
//   ① 需求专篇 docs/需求/alert-engine需求专篇.md 的 35 条
//      （ALT-RULE 5 / ALT-GEN 6 / ALT-DEDUP 6 / ALT-ACK 5 / ALT-SUB 6 / ALT-NFR 7）
//   ② 共享契约 ../phase-engine/docs/契约/protocol.md 的可机检清单
//      （P1/P6/P7/P8/P9/P10、§3.2 码表、§3.3 幂等、§4.4 事件负载、§5 规则包、C15/C16/C17）
//
// 全部用例**确定性**：时钟一律注入假时钟（ALT-NFR-02），时间戳全部由用例显式给出。
// 不依赖当前工作目录（规则包路径由 CMake 注入）、不需要外部服务、不联网。
//
// 运行： selftest                → 跑全部（人读输出）
//        selftest --list         → 只列用例名
//        selftest --json         → 机检输出（acceptance.ps1 读它做需求↔用例对账）
//        selftest <名字片段>      → 只跑名字里含该片段的用例
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "alert_engine/alert_engine.h"

using namespace alert_engine;

namespace {

// ---------------------------------------------------------------- 断言框架
int g_asserts = 0;
int g_failed = 0;
int g_cases = 0;
int g_casesFailed = 0;
std::string g_case;
std::vector<std::string> g_failures;

/// 逐用例元数据：给人看的常规输出不变，`--json` 时供验收脚本 machine-readable 地取值
struct CaseMeta {
    std::string name;
    std::vector<std::string> reqs;  // 覆盖的 ALT-* 需求编号
    int asserts = 0;
    int failed = 0;
    bool ok = false;
};
std::vector<CaseMeta> g_metas;
std::vector<std::string> g_reqs;
bool g_jsonMode = false;

template <typename... Args>
void requires_(Args... ids) {
    for (const char* id : {ids...}) g_reqs.push_back(id);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jsonArray(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"" + jsonEscape(v[i]) + "\"";
    }
    out += "]";
    return out;
}

/// 诊断输出（进度 / 性能数字）。
/// `--json` 模式下 stdout MUST 只有那一个 JSON 文档（否则验收脚本解析不了），故改走 stderr。
void note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_jsonMode ? stderr : stdout, fmt, args);
    va_end(args);
}

/// 让 CHECK_EQ 能比较不同整型宽度（int / int64_t / size_t），并让 enum class 能打印整数
template <typename T, typename = void>
struct Comparable {
    using type = T;
};
template <typename T>
struct Comparable<T, typename std::enable_if<std::is_integral<T>::value>::type> {
    using type = long long;
};
template <typename T>
struct Comparable<T, typename std::enable_if<std::is_enum<T>::value>::type> {
    using type = long long;
};
template <typename T>
typename Comparable<T>::type asComparable(const T& v) {
    return static_cast<typename Comparable<T>::type>(v);
}
template <typename T>
typename Comparable<T>::type asComparable(const std::atomic<T>& v) {
    return static_cast<typename Comparable<T>::type>(v.load());
}

/// 统一的相等判断：让 CHECK_EQ 能比较 json / 浮点 / 字符串 / 整型 / 枚举 / 原子量
bool equalValues(const json& a, const json& b) { return a == b; }
bool equalValues(bool a, const json& b) { return b.is_boolean() && a == b.get<bool>(); }
bool equalValues(const json& a, bool b) { return a.is_boolean() && a.get<bool>() == b; }
bool equalValues(double a, double b) {
    const double d = a - b;
    return (d < 1e-9) && (d > -1e-9);
}
template <typename A, typename B>
bool equalValues(const A& a, const B& b) {
    return asComparable(a) == asComparable(b);
}

void record(bool ok, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (ok) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "  (" << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

std::string show(const json& v) { return v.dump(); }
std::string show(bool v) { return v ? "true" : "false"; }
std::string show(const std::string& v) { return v; }
std::string show(const char* v) { return v ? v : "(null)"; }
template <typename T>
std::string show(const T& v) {
    std::ostringstream oss;
    oss << asComparable(v);
    return oss.str();
}

template <typename A, typename B>
void recordEq(const A& got, const B& want, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (equalValues(got, want)) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "：期望 " << show(want) << "，实际 " << show(got) << "  ("
        << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

}  // namespace

#define CHECK(cond) record((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) recordEq((got), (want), #got " == " #want, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) record((cond), (msg), __FILE__, __LINE__)

// ############################################################################
// 测试替身（宿主侧实现；引擎只认识反向接口 —— ALT-SUB-04 / ALT-NFR-01）
// ############################################################################

namespace {

class FakeClock : public IClock {
public:
    explicit FakeClock(int64_t start = 1750000000000LL) : now_(start) {}
    int64_t nowMs() const override { return now_; }
    void set(int64_t ms) { now_ = ms; }
    void advance(int64_t ms) { now_ += ms; }

private:
    int64_t now_;
};

/// 记录投递的 Sink 替身。引擎可能从多个线程并发调 Sink（ALT-NFR-04），故自带锁。
class SpySink : public IAlertSink {
public:
    struct Rec {
        std::string event;
        json payload = json::object();
        std::vector<std::string> subscribers;
        bool broadcast = false;
        int64_t ts = 0;
    };

    void onAlertRaised(const AlertDelivery& d) override {
        std::lock_guard<std::mutex> lk(m_);
        raised.push_back(snap(d));
    }
    void onAlertUpdated(const AlertDelivery& d) override {
        std::lock_guard<std::mutex> lk(m_);
        updated.push_back(snap(d));
    }
    void onAlertAcked(const AlertDelivery& d) override {
        std::lock_guard<std::mutex> lk(m_);
        acked.push_back(snap(d));
    }

    std::size_t raisedCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return raised.size();
    }
    std::size_t updatedCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return updated.size();
    }
    std::size_t ackedCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return acked.size();
    }
    std::vector<Rec> raisedCopy() const {
        std::lock_guard<std::mutex> lk(m_);
        return raised;
    }
    std::vector<Rec> ackedCopy() const {
        std::lock_guard<std::mutex> lk(m_);
        return acked;
    }
    std::vector<Rec> updatedCopy() const {
        std::lock_guard<std::mutex> lk(m_);
        return updated;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        raised.clear();
        updated.clear();
        acked.clear();
    }

    mutable std::mutex m_;
    std::vector<Rec> raised;
    std::vector<Rec> updated;
    std::vector<Rec> acked;

private:
    static Rec snap(const AlertDelivery& d) {
        Rec r;
        r.event = d.event;
        r.payload = d.payload;
        r.subscribers = d.subscriberIds;
        r.broadcast = d.broadcast;
        r.ts = d.ts;
        return r;
    }
};

/// 会抛异常的 Sink：验证"回调抛异常 MUST 被引擎吞掉并计入 sinkErrors，MUST NOT 影响结果"
class ThrowingSink : public IAlertSink {
public:
    void onAlertRaised(const AlertDelivery&) override { throw std::runtime_error("boom"); }
    void onAlertUpdated(const AlertDelivery&) override { throw std::runtime_error("boom"); }
    void onAlertAcked(const AlertDelivery&) override { throw std::runtime_error("boom"); }
};

/// 内存台账 Store（引擎不接触 SQL —— P3）
class MemStore : public IAlertStore {
public:
    bool save(const AlertRecord& rec) override {
        std::lock_guard<std::mutex> lk(m_);
        ++saves;
        if (failNext) {
            failNext = false;
            return false;
        }
        rows[rec.alertId] = rec;
        return true;
    }
    bool load(const std::string& alertId, AlertRecord& out) override {
        std::lock_guard<std::mutex> lk(m_);
        auto it = rows.find(alertId);
        if (it == rows.end()) return false;
        out = it->second;
        return true;
    }
    bool remove(const std::string& alertId) override {
        std::lock_guard<std::mutex> lk(m_);
        return rows.erase(alertId) != 0;
    }

    mutable std::mutex m_;
    std::map<std::string, AlertRecord> rows;
    int saves = 0;
    bool failNext = false;
};

class SpyLog : public ILogSink {
public:
    void log(int level, const std::string& event, const json& data) override {
        std::lock_guard<std::mutex> lk(m_);
        ++logs;
        (void)level;
        (void)event;
        (void)data;
    }
    void commandAudit(const std::string& action, const std::string& target,
                      const json& detail) override {
        std::lock_guard<std::mutex> lk(m_);
        ++audits;
        (void)target;
        (void)detail;
        auditActions.push_back(action);
    }

    mutable std::mutex m_;
    int logs = 0;
    int audits = 0;
    std::vector<std::string> auditActions;
};

// ---------------------------------------------------------------- 规则包夹具
//
// **规则由数据声明**（ALT-RULE-01 的证明方式之一：换一份 JSON 即换一套规则行为，
// 引擎代码零改动）。下面这些夹具全部是"测试数据"，不是引擎里的业务规则。

json levelCatalog() {
    return json::array({json{{"key", "info"}, {"name", "信息"}, {"rank", 1}},
                        json{{"key", "warn"}, {"name", "警告"}, {"rank", 2}},
                        json{{"key", "error"}, {"name", "严重"}, {"rank", 3}}});
}

json makePack(const json& items, const json& levels = json(), const json& storm = json(),
              const json& mute = json()) {
    json pkg = json::object();
    pkg["policiesNamespace"] = "mapapp";
    pkg["schemaVersion"] = "1.0.0";
    pkg["kind"] = "alertRules";
    pkg["levels"] = (levels.is_array() && !levels.empty()) ? levels : levelCatalog();
    pkg["items"] = items;
    if (!storm.is_null()) pkg["storm"] = storm;
    if (!mute.is_null()) pkg["mute"] = mute;
    return pkg;
}

json ruleDef(const std::string& key, const std::string& level, const json& condition,
             const json& extra = json::object()) {
    json r = json::object();
    r["key"] = key;
    r["version"] = "1.0.0";
    r["level"] = level;
    r["condition"] = condition;
    for (auto it = extra.begin(); it != extra.end(); ++it) r[it.key()] = it.value();
    return r;
}

json thresholdCond(const std::string& field, const std::string& op, double value) {
    return json{{"type", "threshold"}, {"field", field}, {"operator", op}, {"value", value}};
}

json durationCond(const json& inner, int64_t forMs) {
    return json{{"type", "duration"}, {"forMs", forMs}, {"condition", inner}};
}

/// 一个完整的测试环境：假时钟 + 记录型 Sink + 内存 Store + 规则包
struct Env {
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<SpySink> sink = std::make_shared<SpySink>();
    std::shared_ptr<MemStore> store = std::make_shared<MemStore>();
    std::shared_ptr<SpyLog> log = std::make_shared<SpyLog>();
    std::unique_ptr<AlertEngine> engine;

    explicit Env(const json& pack, bool withStore = true) {
        AlertEngineOptions opts;
        opts.clock = clock;
        opts.sink = sink;
        opts.log = log;
        if (withStore) opts.store = store;
        engine = std::make_unique<AlertEngine>(opts);
        const LoadResult r = engine->loadRules(pack);
        if (r.code != 0) note("      [装载失败] %s\n", r.toJson().dump().c_str());
    }
    AlertEngine& e() { return *engine; }
    Observation obs(const std::string& ruleId, const std::string& entity, double value, int64_t ts,
                    const std::string& metric = "signal") {
        Observation o;
        o.ruleId = ruleId;
        o.entityId = entity;
        o.metric = metric;
        o.value = value;
        o.ts = ts;
        return o;
    }
};

json loadPackFile() {
    std::ifstream in(std::string(ALERT_ENGINE_POLICY_DIR) + "/alertRules.json", std::ios::binary);
    if (!in) throw std::runtime_error("规则包文件读不到");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return json::parse(text);
}

std::unique_ptr<Env> envFromFile(bool withStore = true) {
    return std::make_unique<Env>(loadPackFile(), withStore);
}

}  // namespace

// ############################################################################
// ALT-RULE 告警规则（5 条）
// ############################################################################

/// ALT-RULE-01：规则由**数据**声明；引擎内 MUST NOT 内建任何业务规则
void rule01_rules_come_from_data_only() {
    requires_("ALT-RULE-01");
    // 同一份引擎代码，两份不同的规则数据 → 行为完全不同
    const json packA =
        makePack(json::array({ruleDef("alpha", "warn", thresholdCond("m", "gte", 0.5))}));
    const json packB =
        makePack(json::array({ruleDef("alpha", "error", thresholdCond("m", "gte", 0.8))}));

    Env a(packA);
    Env b(packB);
    CHECK_EQ(a.e().rulesInfo().ruleCount, 1);

    const AlertResult ra = a.e().observe(a.obs("alpha", "e1", 0.6, 1000, "m"));
    const AlertResult rb = b.e().observe(b.obs("alpha", "e1", 0.6, 1000, "m"));
    CHECK_EQ(ra.raised, true);
    CHECK_EQ(rb.raised, false);  // 同一输入、不同数据 → 不同结论
    CHECK_EQ(ra.alert.level, std::string("warn"));
    CHECK_EQ(rb.code, 0);

    const AlertResult rb2 = b.e().observe(b.obs("alpha", "e1", 0.9, 2000, "m"));
    CHECK_EQ(rb2.raised, true);
    CHECK_EQ(rb2.alert.level, std::string("error"));

    // 级别目录同样来自数据：换成 4 级 → 引擎照用（ALT-GEN-02 的"可扩展"）
    const json fourLevels = json::array({json{{"key", "info"}, {"name", "信息"}, {"rank", 1}},
                                         json{{"key", "warn"}, {"name", "警告"}, {"rank", 2}},
                                         json{{"key", "error"}, {"name", "严重"}, {"rank", 3}},
                                         json{{"key", "critical"}, {"name", "危急"}, {"rank", 4}}});
    Env c(makePack(json::array({ruleDef("alpha", "critical", thresholdCond("m", "gte", 0.5))}),
                   fourLevels));
    CHECK_EQ(c.e().levels().size(), std::size_t(4));
    const AlertResult rc = c.e().observe(c.obs("alpha", "e1", 0.6, 1000, "m"));
    CHECK_EQ(rc.raised, true);
    CHECK_EQ(rc.alert.level, std::string("critical"));

    // 规则包摘要随数据变化，且可从引擎导出（CTR-PL-06）
    CHECK(policiesDigest(packA) != policiesDigest(packB));
    CHECK_EQ(policiesDigest(packA), a.e().rulesInfo().digest);
    CHECK_EQ(a.e().rulesInfo().digest.size(), std::size_t(16));
}

/// ALT-RULE-02：四类条件（阈值 / 持续时间 / 布尔组合 / 恢复）各有一条用例通过
void rule02_four_condition_kinds() {
    requires_("ALT-RULE-02");
    // ① 阈值比较
    {
        Env env(makePack(
            json::array({ruleDef("t-threshold", "warn", thresholdCond("m", "gt", 10))})));
        CHECK_EQ(env.e().observe(env.obs("t-threshold", "e", 9, 1000, "m")).raised, false);
        CHECK_EQ(env.e().observe(env.obs("t-threshold", "e", 11, 2000, "m")).raised, true);
    }
    // ② 持续时间："持续 N 秒才报"（N=3000ms）：1000ms 不报，满 3000ms 才报
    {
        Env env(makePack(json::array(
            {ruleDef("t-duration", "warn", durationCond(thresholdCond("m", "gt", 10), 3000))})));
        const AlertResult r0 = env.e().observe(env.obs("t-duration", "e", 11, 1000, "m"));
        CHECK_EQ(r0.raised, false);
        CHECK_EQ(r0.durationPending, true);
        const AlertResult r = env.e().observe(env.obs("t-duration", "e", 11, 4000, "m"));
        CHECK_EQ(r.raised, true);
        CHECK_EQ(r.alert.firstAt, int64_t(4000));  // 产生时刻 = 满足持续时长的那一刻
        // 中断即重新计时：掉到阈值下再上来，必须重新累计
        Env env2(makePack(json::array(
            {ruleDef("t-duration", "warn", durationCond(thresholdCond("m", "gt", 10), 3000))})));
        CHECK_EQ(env2.e().observe(env2.obs("t-duration", "e", 11, 1000, "m")).raised, false);
        CHECK_EQ(env2.e().observe(env2.obs("t-duration", "e", 0, 3000, "m")).raised, false);
        CHECK_EQ(env2.e().observe(env2.obs("t-duration", "e", 11, 4000, "m")).raised, false);
        CHECK_EQ(env2.e().observe(env2.obs("t-duration", "e", 11, 7000, "m")).raised, true);
    }
    // ③ 布尔组合：allOf（两指标同时满足才报）
    {
        const json cond = json{{"type", "bool"},
                               {"logic", "allOf"},
                               {"conditions", json::array({thresholdCond("a", "gt", 1),
                                                           thresholdCond("b", "lt", 5)})}};
        Env env(makePack(json::array({ruleDef("t-bool", "warn", cond)})));
        Observation o = env.obs("t-bool", "e", 0, 1000, "a");
        o.values["a"] = 2;
        o.values["b"] = 9;
        CHECK_EQ(env.e().observe(o).raised, false);
        Observation o2 = env.obs("t-bool", "e", 0, 2000, "a");
        o2.values["a"] = 2;
        o2.values["b"] = 3;
        CHECK_EQ(env.e().observe(o2).raised, true);
        // ③b anyOf
        const json anyCond = json{{"type", "bool"},
                                  {"logic", "anyOf"},
                                  {"conditions", json::array({thresholdCond("a", "gt", 100),
                                                              thresholdCond("b", "gt", 1)})}};
        Env env2(makePack(json::array({ruleDef("t-any", "warn", anyCond)})));
        Observation o3 = env2.obs("t-any", "e", 0, 1000, "a");
        o3.values["a"] = 0;
        o3.values["b"] = 2;
        CHECK_EQ(env2.e().observe(o3).raised, true);
        // ③c not
        const json notCond = json{{"type", "bool"},
                                  {"logic", "not"},
                                  {"condition", thresholdCond("a", "gt", 10)}};
        Env env3(makePack(json::array({ruleDef("t-not", "warn", notCond)})));
        CHECK_EQ(env3.e().observe(env3.obs("t-not", "e", 5, 1000, "a")).raised, true);
    }
    // ③d 缺失条件（需求原文的"缺失"）+ in 集合算子
    {
        const json missingCond = json{{"type", "threshold"}, {"field", "rare"}, {"operator", "missing"}};
        Env env(makePack(json::array({ruleDef("t-missing", "warn", missingCond)})));
        CHECK_EQ(env.e().observe(env.obs("t-missing", "e", 0, 1000, "m")).raised, true);

        const json inCond = json{{"type", "threshold"},
                                 {"field", "state"},
                                 {"operator", "in"},
                                 {"values", json::array({"red", "yellow"})}};
        Env env2(makePack(json::array({ruleDef("t-in", "warn", inCond)})));
        Observation o = env2.obs("t-in", "e", 0, 1000, "state");
        o.context["state"] = 1;  // 数值上下文不命中字符串集合
        CHECK_EQ(env2.e().observe(o).raised, false);
        // 上下文按数值集合使用：values 支持数值字符串
        const json numIn = json{{"type", "threshold"},
                                {"field", "code"},
                                {"operator", "in"},
                                {"values", json::array({3, 7})}};
        Env env3(makePack(json::array({ruleDef("t-in2", "warn", numIn)})));
        Observation o2 = env3.obs("t-in2", "e", 0, 1000, "code");
        o2.context["code"] = 7;
        CHECK_EQ(env3.e().observe(o2).raised, true);
    }
    // ④ 恢复：原始条件不再满足即为真；引擎内版本带"曾经成立过"锚点
    {
        Env env(makePack(json::array(
            {ruleDef("t-recovery", "info",
                     json{{"type", "recovery"}, {"condition", thresholdCond("m", "gt", 10)}},
                     json{{"dedupWindowMs", 5000}})})));
        // 刚接入、条件从未成立过 → **不得**误报恢复（"曾经成立过"是必要条件）
        const AlertResult pre = env.e().observe(env.obs("t-recovery", "e", 0, 1000, "m"));
        CHECK_EQ(pre.raised, false);
        // 原始条件成立 → 恢复条件**不**成立（还没恢复），因此不产生告警
        const AlertResult hold = env.e().observe(env.obs("t-recovery", "e", 11, 2000, "m"));
        CHECK_EQ(hold.raised, false);
        // 原始条件不再满足 → 恢复条件成立 → 产生告警（"什么算恢复"由数据声明）
        const AlertResult r1 = env.e().observe(env.obs("t-recovery", "e", 0, 3000, "m"));
        CHECK_EQ(r1.raised, true);
        // 条件持续不成立 → 恢复条件持续成立；窗口内 → 归并为同一条并累加
        const AlertResult r2 = env.e().observe(env.obs("t-recovery", "e", 0, 4000, "m"));
        CHECK_EQ(r2.merged, true);
        CHECK_EQ(r2.alert.count, int64_t(2));
        // 跨窗口再观测一次（原始条件仍未满足）→ 新开一条
        const AlertResult r3 = env.e().observe(env.obs("t-recovery", "e", 0, 9000, "m"));
        CHECK_EQ(r3.raised, true);
        CHECK_EQ(env.e().listAlerts().total, 2);
    }
    // 公开纯函数入口与引擎内口径一致（供宿主预检）
    {
        Observation o;
        o.metric = "m";
        o.value = 15;
        Condition c;
        c.type = ConditionType::Threshold;
        c.op = CompareOp::Gt;
        c.value = 10;
        CHECK_EQ(evaluateCondition(c, o), true);
        Condition over = c;  // 独立副本：不给共享指针做别名，避免测出"改一处变两处"
        over.value = 20;
        CHECK_EQ(evaluateCondition(over, o), false);

        Condition d;
        d.type = ConditionType::Duration;
        d.forMs = 5000;
        d.inner = std::make_shared<Condition>(c);  // 内部条件 gt 10
        CHECK_EQ(evaluateCondition(d, o), true);   // 瞬时口径只看内部条件
        Condition d2;
        d2.type = ConditionType::Duration;
        d2.forMs = 5000;
        d2.inner = std::make_shared<Condition>(over);  // 内部条件 gt 20
        CHECK_EQ(evaluateCondition(d2, o), false);

        bool missing = true;
        evaluateCondition(c, o, &missing);
        CHECK_EQ(missing, false);
    }
}

/// ALT-RULE-03：装载时校验（重复 id、未知字段、非法窗口），失败给可读原因
void rule03_load_validation_reports_location() {
    requires_("ALT-RULE-03");
    // (a) 重复 id
    {
        const json pkg = makePack(json::array(
            {ruleDef("dup", "warn", thresholdCond("m", "gt", 1)),
             ruleDef("dup", "warn", thresholdCond("m", "gt", 2))}));
        const LoadResult r = validateRules(pkg);
        CHECK_EQ(r.code, 1000);
        CHECK(!r.issues.empty());
        CHECK(r.issues[0].path.find("items[1]") != std::string::npos);
        CHECK(r.issues[0].reason.find("重复") != std::string::npos);
    }
    // (b) 非法窗口（负数）
    {
        const json pkg = makePack(json::array({ruleDef("w", "warn", thresholdCond("m", "gt", 1),
                                                        json{{"dedupWindowMs", -5}})}));
        const LoadResult r = validateRules(pkg);
        CHECK_EQ(r.code, 1000);
        CHECK_EQ(r.issues[0].field, std::string("dedupWindowMs"));
        CHECK(r.issues[0].path.find("items[0]") != std::string::npos);
    }
    // (c) 未知字段 → **忽略 + 计入告警**，MUST NOT 导致装载失败（CTR-PL-03）
    {
        const json pkg = makePack(json::array({ruleDef("ok", "warn", thresholdCond("m", "gt", 1),
                                                         json{{"futureField", 42}})}));
        const LoadResult r = validateRules(pkg);
        CHECK_EQ(r.code, 0);
        CHECK(!r.data.warnings.empty());
    }
    // (d) 缺必填字段 → 拒绝且指向条目下标 + 字段名（CTR-PL-05）
    {
        json bad = json::object();
        bad["key"] = "nolevel";
        bad["condition"] = thresholdCond("m", "gt", 1);
        const LoadResult r = validateRules(makePack(json::array({bad})));
        CHECK_EQ(r.code, 1000);
        CHECK_EQ(r.issues[0].field, std::string("level"));
        CHECK(r.issues[0].path.find("items[0]") != std::string::npos);
    }
    // (e) 非法条件（未知算子 / 缺 forMs）→ 逐条可读、不短路
    {
        const json badCond = json{{"type", "threshold"}, {"field", "m"}, {"operator", "approximately"}};
        const LoadResult r = validateRules(makePack(json::array({ruleDef("c", "warn", badCond)})));
        CHECK_EQ(r.code, 1000);
        CHECK(r.issues[0].path.find("items[0].condition") != std::string::npos);
        CHECK_EQ(r.issues[0].field, std::string("operator"));

        const json noFor = json{{"type", "duration"}, {"condition", thresholdCond("m", "gt", 1)}};
        const LoadResult r2 = validateRules(makePack(json::array({ruleDef("d", "warn", noFor)})));
        CHECK_EQ(r2.code, 1000);
        CHECK_EQ(r2.issues[0].field, std::string("forMs"));
    }
    // (f) level 不在 levels 目录内 → 拒绝（取值由规则包声明）
    {
        const LoadResult r =
            validateRules(makePack(json::array({ruleDef("x", "fatal", thresholdCond("m", "gt", 1))})));
        CHECK_EQ(r.code, 1000);
        CHECK(r.issues[0].reason.find("levels") != std::string::npos);
    }
    // (g) kind 错 / MAJOR 不符 → 1000 / 1006（MUST NOT 静默降级）
    {
        json pkg = makePack(json::array({ruleDef("a", "warn", thresholdCond("m", "gt", 1))}));
        pkg["kind"] = "phases";
        CHECK_EQ(validateRules(pkg).code, 1000);
        json pkg2 = makePack(json::array({ruleDef("a", "warn", thresholdCond("m", "gt", 1))}));
        pkg2["schemaVersion"] = "2.1.0";
        const LoadResult r = validateRules(pkg2);
        CHECK_EQ(r.code, 1006);
        CHECK(r.issues[0].reason.find("MAJOR") != std::string::npos);
        json pkg3 = makePack(json::array({ruleDef("a", "warn", thresholdCond("m", "gt", 1))}));
        pkg3["schemaVersion"] = "1.9.9";
        CHECK_EQ(validateRules(pkg3).code, 0);
    }
    // (h) 抑制关系引用不存在的规则 / 成环 → 拒绝
    {
        const LoadResult r = validateRules(makePack(json::array(
            {ruleDef("a", "warn", thresholdCond("m", "gt", 1), json{{"parent", "ghost"}})})));
        CHECK_EQ(r.code, 1000);
        CHECK(r.issues[0].reason.find("ghost") != std::string::npos);

        const LoadResult r2 = validateRules(makePack(json::array(
            {ruleDef("a", "warn", thresholdCond("m", "gt", 1), json{{"parent", "b"}}),
             ruleDef("b", "warn", thresholdCond("m", "gt", 1), json{{"parent", "a"}})})));
        CHECK_EQ(r2.code, 1000);
        CHECK(r2.issues[0].reason.find("环") != std::string::npos);
    }
    // (i) 装载失败 → **原子替换**：保留上一次成功装载的规则
    {
        const json good = makePack(json::array({ruleDef("keep", "warn", thresholdCond("m", "gt", 1))}));
        Env env(good);
        json broken = good;
        broken["schemaVersion"] = "9.0.0";
        const LoadResult r = env.e().loadRules(broken);
        CHECK_EQ(r.code, 1006);
        CHECK_EQ(env.e().rulesInfo().loaded, true);
        CHECK_EQ(env.e().rulesInfo().digest, policiesDigest(good));
        // 引擎仍按旧规则工作
        CHECK_EQ(env.e().observe(env.obs("keep", "e", 5, 1000, "m")).raised, true);
    }
    // (j) 真实规则包通过校验（规则包自建；schema 遵守 protocol §5）
    {
        const LoadResult r = validateRules(loadPackFile());
        if (r.code != 0) note("      %s\n", r.toJson().dump().c_str());
        CHECK_EQ(r.code, 0);
        CHECK_EQ(r.data.loaded, true);
        CHECK_EQ(r.data.schemaVersion, std::string("1.0.0"));
        CHECK(r.data.ruleCount >= 5);
        CHECK_EQ(r.data.levelKeys.size(), std::size_t(3));
        CHECK(r.data.stormEnabled);
        CHECK_EQ(r.data.muteDefaultMs, int64_t(300000));
    }
    // (k) 坏规则的**位置**必须可读：条目标题 + 字段名同时给出
    {
        const json pkg = makePack(json::array(
            {ruleDef("good", "warn", thresholdCond("m", "gt", 1)),
             ruleDef("bad", "warn", json{{"type", "bool"}, {"logic", "xor"}, {"conditions", json::array()}})}));
        const LoadResult r = validateRules(pkg);
        CHECK_EQ(r.code, 1000);
        CHECK(r.issues[0].path.find("items[1]") != std::string::npos);
        CHECK_EQ(r.issues[0].field, std::string("logic"));
    }
}

/// ALT-RULE-04：规则可运行时启用/禁用，不影响已产生告警
void rule04_enable_disable_at_runtime() {
    requires_("ALT-RULE-04");
    Env env(makePack(json::array({ruleDef("r", "warn", thresholdCond("m", "gt", 1),
                                          json{{"dedupWindowMs", 0}})})));
    AlertEngine& e = env.e();
    const AlertResult r1 = e.observe(env.obs("r", "e1", 5, 1000, "m"));
    CHECK_EQ(r1.raised, true);
    const std::string firstId = r1.alert.alertId;

    CHECK_EQ(e.setRuleEnabled("r", false).code, 0);
    CHECK_EQ(e.isRuleEnabled("r"), false);
    // 禁用后不再产生新告警（另一个实体也不产生），并如实标注原因
    const AlertResult r2 = e.observe(env.obs("r", "e2", 5, 2000, "m"));
    CHECK_EQ(r2.raised, false);
    CHECK_EQ(r2.suppressed, true);
    CHECK_EQ(r2.suppressReason, std::string("rule-disabled"));
    CHECK_EQ(e.counts().alertsRaised, int64_t(1));
    // 历史告警仍可查（台账一字不动）
    CHECK(e.getAlert(firstId).has_value());
    CHECK_EQ(e.listAlerts().total, 1);
    CHECK_EQ(e.rulesInfo().disabledCount, 1);

    // 重新启用 → 恢复产生
    CHECK_EQ(e.setRuleEnabled("r", true).code, 0);
    const AlertResult r3 = e.observe(env.obs("r", "e2", 5, 3000, "m"));
    CHECK_EQ(r3.raised, true);
    CHECK_EQ(e.counts().alertsRaised, int64_t(2));

    // 未知 id → 1004（不猜、不静默成功）
    CHECK_EQ(e.setRuleEnabled("ghost", false).code, 1004);
}

/// ALT-RULE-05：规则版本化 —— 告警记录 MUST 携带产生时的规则版本，可反查
void rule05_alert_carries_rule_version() {
    requires_("ALT-RULE-05");
    json item = ruleDef("v", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 10000}});
    item["version"] = "1.4.2";
    Env env(makePack(json::array({item})));
    const AlertResult r = env.e().observe(env.obs("v", "e", 5, 1000, "m"));
    CHECK_EQ(r.raised, true);
    CHECK_EQ(r.alert.ruleVersion, std::string("1.4.2"));
    CHECK_EQ(r.alert.toJson()["ruleVersion"], std::string("1.4.2"));
    // 事件负载也带上它（**只增**字段，既有契约字段不动 —— CTR-EV-04）
    const auto raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(1));
    CHECK_EQ(raised[0].payload["ruleVersion"], std::string("1.4.2"));

    // 改规则版本重新装载 → **窗口内合并**沿用产生时的旧版本（可反查当时规则）
    item["version"] = "2.0.0";
    CHECK_EQ(env.e().loadRules(makePack(json::array({item}))).code, 0);
    const AlertResult r2 = env.e().observe(env.obs("v", "e", 5, 4000, "m"));
    CHECK_EQ(r2.merged, true);                             // 仍在 1000+10000 窗口内
    CHECK_EQ(r2.alert.ruleVersion, std::string("1.4.2"));  // 合并不改历史版本

    // 跨窗口（> 11000）→ 新开一条，带**新**版本
    const AlertResult r3 = env.e().observe(env.obs("v", "e", 5, 20000, "m"));
    CHECK_EQ(r3.raised, true);
    CHECK_EQ(r3.alert.ruleVersion, std::string("2.0.0"));
    CHECK_EQ(env.e().listAlerts().total, 2);
}

// ############################################################################
// ALT-GEN 产生与分级（6 条）
// ############################################################################

/// ALT-GEN-01：产生接口为**结构化输入**；引擎 MUST NOT 接受"成句文案"作为主要字段
void gen01_structured_input_only() {
    requires_("ALT-GEN-01");
    Env env(makePack(json::array({ruleDef("s", "warn", thresholdCond("m", "gt", 1),
                                          json{{"title", "实体 {entityId} 指标 {metric}={value}"}})})));
    // 结构化入参：{规则id, 实体?, 指标, 数值, 时间, 上下文}
    Observation o;
    o.ruleId = "s";
    o.entityId = "dev-1";
    o.missionId = "m-1";
    o.metric = "m";
    o.value = 5;
    o.ts = 1000;
    o.context["extra"] = 1;
    const AlertResult r = env.e().observe(o);
    CHECK_EQ(r.raised, true);

    // 文案来自**规则模板填充**（引擎不产出自然语言）；字段值逐项可核对
    CHECK_EQ(r.alert.title, std::string("实体 dev-1 指标 m=5"));
    // 结构化字段齐全：引擎消费的是字段而不是句子
    CHECK_EQ(r.alert.metric, std::string("m"));
    CHECK_EQ(r.alert.entityId, std::string("dev-1"));
    CHECK_EQ(r.alert.lastValue, 5.0);
    CHECK_EQ(r.alert.firstAt, int64_t(1000));

    // JSON 重载：只认结构化键；"成句文案"字段被忽略（未知字段 MUST NOT 影响行为）
    json j = json::object();
    j["ruleId"] = "s";
    j["entityId"] = "dev-2";
    j["metric"] = "m";
    j["value"] = 5;
    j["ts"] = 2000;
    j["text"] = "节点 dev-2 出问题了，快看一下";  // 成句文案：引擎 MUST NOT 依赖它
    j["message"] = "节点 dev-2 出问题了";
    const AlertResult r2 = env.e().observe(j);
    CHECK_EQ(r2.raised, true);
    CHECK_EQ(r2.alert.entityId, std::string("dev-2"));
    CHECK_EQ(r2.alert.metric, std::string("m"));  // 取值来自 value 字段，不是从句子猜的
    // 引擎不落自然语言：文案仍由模板填充（未给模板时为空，MUST NOT 编造）
    CHECK_EQ(r2.alert.title, std::string("实体 dev-2 指标 m=5"));
    Env env2(makePack(json::array({ruleDef("s", "warn", thresholdCond("m", "gt", 1))})));
    const AlertResult r3 = env2.e().observe(env2.obs("s", "dev", 5, 1000, "m"));
    CHECK_EQ(r3.alert.title, std::string(""));
}

/// ALT-GEN-02：级别由规则决定，取值可扩展；MUST NOT 硬编码三级
void gen02_levels_are_extensible() {
    requires_("ALT-GEN-02");
    // 目录只有 2 级也能跑；目录有 5 级也能跑 —— 数量不受引擎限制
    const json two = json::array({json{{"key", "low"}, {"name", "低"}, {"rank", 1}},
                                  json{{"key", "high"}, {"name", "高"}, {"rank", 2}}});
    Env env2(makePack(json::array({ruleDef("a", "low", thresholdCond("m", "gt", 1)),
                                   ruleDef("b", "high", thresholdCond("m", "gt", 1))}),
                      two));
    CHECK_EQ(env2.e().levels().size(), std::size_t(2));
    CHECK_EQ(env2.e().observe(env2.obs("a", "e", 5, 1000, "m")).alert.level, std::string("low"));
    CHECK_EQ(env2.e().observe(env2.obs("b", "e", 5, 1000, "m")).alert.level, std::string("high"));

    const json five = json::array({json{{"key", "l1"}, {"rank", 1}, {"name", "一"}},
                                   json{{"key", "l2"}, {"rank", 2}, {"name", "二"}},
                                   json{{"key", "l3"}, {"rank", 3}, {"name", "三"}},
                                   json{{"key", "l4"}, {"rank", 4}, {"name", "四"}},
                                   json{{"key", "l5"}, {"rank", 5}, {"name", "五"}}});
    Env env5(makePack(json::array({ruleDef("z", "l5", thresholdCond("m", "gt", 1))}), five));
    CHECK_EQ(env5.e().levels().size(), std::size_t(5));
    CHECK_EQ(env5.e().observe(env5.obs("z", "e", 5, 1000, "m")).alert.level, std::string("l5"));

    // rank 比较是引擎唯一使用的级别机制：未知级别 → nullopt（MUST NOT 猜）
    CHECK_EQ(env5.e().levelRank("l3").value(), 3);
    CHECK_EQ(env5.e().levelRank("nope").has_value(), false);
}

/// ALT-GEN-03：幂等锚点 —— 同一（规则+实体）在去重窗口内的重复触发归并为同一条并累加计数
void gen03_dedup_anchor_merges_and_counts() {
    requires_("ALT-GEN-03");
    Env env(makePack(json::array(
        {ruleDef("d", "warn", thresholdCond("m", "gt", 1),
                 json{{"dedupWindowMs", 10000}, {"merge", "accumulate"}})})));
    // **核心实测：3 秒内触发 10 次 → 一条告警、计数 10**
    int raised = 0, merged = 0;
    std::string id;
    for (int i = 0; i < 10; ++i) {
        const AlertResult r = env.e().observe(env.obs("d", "e", 5, 1000 + i * 300, "m"));
        if (r.raised) ++raised;
        if (r.merged) ++merged;
        if (!r.alert.alertId.empty()) id = r.alert.alertId;
    }
    CHECK_EQ(raised, 1);
    CHECK_EQ(merged, 9);
    CHECK_EQ(env.e().listAlerts().total, 1);
    const auto rec = env.e().getAlert(id).value();
    note("      去重实测：3 秒内触发 10 次 → 台账 %d 条、该条 count=%lld（firstAt=%lld / lastAt=%lld）\n",
         env.e().listAlerts().total, static_cast<long long>(rec.count),
         static_cast<long long>(rec.firstAt), static_cast<long long>(rec.lastAt));
    CHECK_EQ(rec.count, int64_t(10));
    CHECK_EQ(rec.firstAt, int64_t(1000));
    CHECK_EQ(rec.lastAt, int64_t(3700));  // 最近一次时间被更新
    CHECK_EQ(rec.firstValue, 5.0);        // 首次时间与首次数值保留

    // 锚点是（规则 + **实体**）：另一实体独立成一条（风险 R3：窗口不吞不同实体）
    const AlertResult other = env.e().observe(env.obs("d", "e2", 5, 4000, "m"));
    CHECK_EQ(other.raised, true);
    CHECK_EQ(env.e().listAlerts().total, 2);

    // 计数口径显式：rawRaises == alertsRaised + merged（ALT-GEN-05 的恒等式）
    const AlertCounts n = env.e().counts();
    CHECK_EQ(n.rawRaises, int64_t(11));
    CHECK_EQ(n.alertsRaised, int64_t(2));
    CHECK_EQ(n.merged, int64_t(9));
    // 计数口径恒等式：原始条数 = 新开 + 恢复 + 合并 + 折叠（ALT-GEN-05 的可复算口径）
    CHECK_EQ(n.rawRaises, n.alertsRaised + n.recoveries + n.merged + n.folded);
    CHECK_EQ(n.alertCount, int64_t(2));
    CHECK_EQ(n.basis, std::string("deduplicated"));
}

/// ALT-GEN-04：恢复语义 —— 条件不再满足时产生"恢复"记录或关闭告警（按规则声明），可关联
void gen04_recovery_semantics() {
    requires_("ALT-GEN-04");
    // (a) recoverOn=record：触发 → 恢复 → 再触发，三条记录关系正确
    {
        Env env(makePack(json::array(
            {ruleDef("rr", "warn", thresholdCond("m", "gt", 10),
                     json{{"dedupWindowMs", 0}, {"recoverOn", "record"}})})));
        const AlertResult r1 = env.e().observe(env.obs("rr", "e", 11, 1000, "m"));
        CHECK_EQ(r1.raised, true);
        const std::string a1 = r1.alert.alertId;
        const AlertResult r2 = env.e().observe(env.obs("rr", "e", 0, 2000, "m"));
        CHECK_EQ(r2.recovered, true);
        CHECK_EQ(r2.alert.kind, std::string("recovery"));
        CHECK_EQ(r2.alert.linkedAlertId, a1);  // 恢复记录关联原告警
        const AlertResult r3 = env.e().observe(env.obs("rr", "e", 11, 5000, "m"));
        CHECK_EQ(r3.raised, true);
        CHECK(r3.alert.alertId != a1);         // 再触发 → 新的一条
        CHECK_EQ(env.e().listAlerts().total, 3);
        // 三条记录的关系：告警(closed) ← 恢复(linkedAlertId) ；第二条告警独立
        const auto rec1 = env.e().getAlert(a1).value();
        CHECK_EQ(rec1.state, AlertState::Closed);
        const auto rec2 = env.e().getAlert(r2.alert.alertId).value();
        CHECK_EQ(rec2.linkedAlertId, a1);
        const auto rec3 = env.e().getAlert(r3.alert.alertId).value();
        CHECK_EQ(rec3.state, AlertState::Active);
        CHECK_EQ(rec3.kind, std::string("alert"));
        // 恢复记录也是一条台账记录（可查、可筛）
        AlertQuery q;
        q.kindIn = {"recovery"};
        CHECK_EQ(env.e().listAlerts(q).total, 1);
    }
    // (b) recoverOn=close：条件恢复时自动关闭并留痕
    {
        Env env(makePack(json::array(
            {ruleDef("rc", "warn", thresholdCond("m", "gt", 10), json{{"recoverOn", "close"}})})));
        const AlertResult r1 = env.e().observe(env.obs("rc", "e", 11, 1000, "m"));
        CHECK_EQ(r1.raised, true);
        const AlertResult r2 = env.e().observe(env.obs("rc", "e", 0, 2000, "m"));
        CHECK_EQ(r2.recovered, true);
        CHECK_EQ(r2.alert.state, AlertState::Closed);
        CHECK_EQ(r2.alert.active, false);
        CHECK_EQ(env.e().listAlerts().total, 1);  // 不新开记录，只关闭
        // 自动关闭留痕（action=auto-close，MUST NOT 静默 —— ALT-ACK-05）
        bool hasAuto = false;
        for (const auto& s : r2.alert.stateHistory) {
            if (s.action == "auto-close") hasAuto = true;
        }
        CHECK_EQ(hasAuto, true);
        CHECK_EQ(env.e().metrics().autoClosed, int64_t(1));
    }
    // (c) recoverOn=none：条件恢复**不产生任何动作**（按规则声明）
    {
        Env env(makePack(json::array(
            {ruleDef("rn", "warn", thresholdCond("m", "gt", 10), json{{"recoverOn", "none"}})})));
        CHECK_EQ(env.e().observe(env.obs("rn", "e", 11, 1000, "m")).raised, true);
        const AlertResult r2 = env.e().observe(env.obs("rn", "e", 0, 2000, "m"));
        CHECK_EQ(r2.recovered, false);
        CHECK_EQ(env.e().listAlerts().total, 1);
        CHECK_EQ(env.e().getAlert("a-1").value().state, AlertState::Active);  // 仍活动
    }
}

/// ALT-GEN-05：计数准确 —— `alert_count` 口径**显式**，供报告直接使用
void gen05_count_basis_is_explicit() {
    requires_("ALT-GEN-05");
    Env env(makePack(json::array(
        {ruleDef("c", "warn", thresholdCond("m", "gt", 1),
                 json{{"dedupWindowMs", 10000}, {"recoverOn", "close"}})})));
    AlertEngine& e = env.e();
    for (int i = 0; i < 10; ++i) e.observe(env.obs("c", "e", 5, 1000 + i * 100, "m"));
    e.observe(env.obs("c", "e", 0, 3000, "m"));  // 恢复 → 关闭
    for (int i = 0; i < 5; ++i) e.observe(env.obs("c", "e2", 5, 20000 + i * 100, "m"));

    const AlertCounts n = e.counts();
    // 口径①原始条数：每一次触发各算一条
    CHECK_EQ(n.rawRaises, int64_t(15));
    // 口径②去重后条数：台账记录条数（host 报告直接取这个）
    CHECK_EQ(n.alertCount, int64_t(2));
    CHECK_EQ(n.alertsRaised, int64_t(2));
    CHECK_EQ(n.merged, int64_t(13));
    // 计数口径恒等式：原始条数 = 新开 + 恢复 + 合并 + 折叠（ALT-GEN-05 的可复算口径）
    CHECK_EQ(n.rawRaises, n.alertsRaised + n.recoveries + n.merged + n.folded);  // 口径可复算
    CHECK_EQ(n.basis, std::string("deduplicated"));    // 缺省取去重后条数（需求 D7）
    CHECK_EQ(n.openActive, int64_t(1));
    // 从台账**复算**：alertCount == 台账记录数
    CHECK_EQ(n.alertCount, static_cast<int64_t>(e.listAlerts(AlertQuery{}).total));
    // 口径可切换（宿主可声明取原始条数）
    AlertEngineOptions opts2;
    opts2.alertCountBasis = "raw";
    AlertEngine e2(opts2);
    CHECK_EQ(e2.loadRules(makePack(json::array(
                 {ruleDef("c", "warn", thresholdCond("m", "gt", 1))}))).code, 0);
    CHECK_EQ(e2.counts().basis, std::string("raw"));
    CHECK_EQ(e2.capabilities().alertCountBasis, std::string("raw"));
    // JSON 里两个口径都在案（报告侧无需再猜）
    const json j = n.toJson();
    CHECK(j.contains("rawRaises"));
    CHECK(j.contains("alertCount"));
    CHECK_EQ(j["basis"], std::string("deduplicated"));
}

/// ALT-GEN-06：首次触发与后续累加可分 —— "第一次发生时间"与"最近一次时间"可分别读出
void gen06_first_and_last_are_separate() {
    requires_("ALT-GEN-06");
    // 两次观测（都在窗口内）→ 一条记录，但"第一次"与"最近一次"必须可分别读出
    Env env(makePack(json::array(
        {ruleDef("t", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 10000}})})));
    env.e().observe(env.obs("t", "e", 3, 1000, "m"));
    env.e().observe(env.obs("t", "e", 8, 4000, "m"));
    const auto rec = env.e().getAlert("a-1").value();
    CHECK_EQ(rec.count, int64_t(2));
    CHECK_EQ(rec.firstAt, int64_t(1000));
    CHECK_EQ(rec.lastAt, int64_t(4000));
    CHECK(rec.lastAt != rec.firstAt);
    CHECK_EQ(rec.firstValue, 3.0);
    CHECK_EQ(rec.lastValue, 8.0);
    // 两个时间都在台账 JSON 与事件负载里可读
    const json j = rec.toJson();
    CHECK_EQ(j["firstAt"], int64_t(1000));
    CHECK_EQ(j["lastAt"], int64_t(4000));
    // 跨窗口后新开的一条：它的 firstAt = lastAt（首次即最近），而老记录两个时间都不变
    const AlertResult r3 = env.e().observe(env.obs("t", "e", 9, 20000, "m"));
    CHECK_EQ(r3.raised, true);
    CHECK_EQ(r3.alert.firstAt, int64_t(20000));
    CHECK_EQ(r3.alert.lastAt, int64_t(20000));
    const auto old = env.e().getAlert("a-1").value();
    CHECK_EQ(old.firstAt, int64_t(1000));
    CHECK_EQ(old.lastAt, int64_t(4000));
}

// ############################################################################
// ALT-DEDUP 去重、合并与抑制（6 条）
// ############################################################################

/// ALT-DEDUP-01：去重窗口可配（按规则）；窗口内合并，窗口外新开一条
void dedup01_window_configurable_and_crossing_opens_new() {
    requires_("ALT-DEDUP-01");
    Env env(makePack(json::array(
        {ruleDef("w", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 5000}})})));
    const AlertResult r1 = env.e().observe(env.obs("w", "e", 5, 1000, "m"));
    CHECK_EQ(r1.raised, true);
    // 窗口内（1000 + 5000 = 6000 未到）→ 合并
    const AlertResult r2 = env.e().observe(env.obs("w", "e", 5, 4000, "m"));
    CHECK_EQ(r2.merged, true);
    // 跨窗口（>= 6000）→ 新开一条
    const AlertResult r3 = env.e().observe(env.obs("w", "e", 5, 6000, "m"));
    CHECK_EQ(r3.raised, true);
    CHECK(r3.alert.alertId != r1.alert.alertId);
    CHECK_EQ(env.e().listAlerts().total, 2);
    // 第二条自己也有窗口：6000+5000 内继续合并到第二条
    const AlertResult r4 = env.e().observe(env.obs("w", "e", 5, 9000, "m"));
    CHECK_EQ(r4.merged, true);
    CHECK_EQ(r4.alert.alertId, r3.alert.alertId);
    CHECK_EQ(r4.alert.count, int64_t(2));
    CHECK_EQ(env.e().listAlerts().total, 2);

    // 窗口按**规则**分别配置：同一份数据里两条规则窗口不同 → 行为不同
    Env env2(makePack(json::array(
        {ruleDef("fast", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 1000}}),
         ruleDef("slow", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 60000}})})));
    env2.e().observe(env2.obs("fast", "e", 5, 0, "m"));
    env2.e().observe(env2.obs("fast", "e", 5, 2000, "m"));
    env2.e().observe(env2.obs("slow", "e", 5, 0, "m"));
    env2.e().observe(env2.obs("slow", "e", 5, 2000, "m"));
    AlertQuery q;
    q.ruleIn = {"fast"};
    CHECK_EQ(env2.e().listAlerts(q).total, 2);   // 1s 窗口：跨窗口新开
    q.ruleIn = {"slow"};
    CHECK_EQ(env2.e().listAlerts(q).total, 1);   // 60s 窗口：仍合并

    // 窗口 = 0 → 不合并，每次触发各自成条（"不合并"是显式声明，不是缺省猜测）
    Env env3(makePack(json::array(
        {ruleDef("n", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}})})));
    env3.e().observe(env3.obs("n", "e", 5, 1000, "m"));
    env3.e().observe(env3.obs("n", "e", 5, 1001, "m"));
    CHECK_EQ(env3.e().listAlerts().total, 2);
}

/// ALT-DEDUP-02：合并时更新"计数 + 最近时间 + 最新数值"，不改首次时间与原始级别
void dedup02_merge_field_contract_and_merge_modes() {
    requires_("ALT-DEDUP-02");
    Env env(makePack(json::array(
        {ruleDef("m", "warn", thresholdCond("m", "gt", 1),
                 json{{"dedupWindowMs", 10000}, {"merge", "accumulate"}})})));
    env.e().observe(env.obs("m", "e", 3, 1000, "m"));
    env.e().observe(env.obs("m", "e", 7, 2000, "m"));
    env.e().observe(env.obs("m", "e", 9, 3000, "m"));
    const auto rec = env.e().getAlert("a-1").value();
    CHECK_EQ(rec.count, int64_t(3));         // 计数累加
    CHECK_EQ(rec.lastAt, int64_t(3000));     // 最近时间更新
    CHECK_EQ(rec.lastValue, 9.0);            // 最新数值更新
    CHECK_EQ(rec.firstAt, int64_t(1000));    // **首次时间不被合并修改**
    CHECK_EQ(rec.firstValue, 3.0);           // 首次数值同样保留
    CHECK_EQ(rec.originLevel, std::string("warn"));  // 原始级别保留
    CHECK_EQ(rec.levelHistory.size(), std::size_t(1));  // 未升级 → 历史只有创建那条
    CHECK_EQ(rec.level, std::string("warn"));
    CHECK_EQ(rec.windowStart, int64_t(1000));  // 窗口起点不动

    // 四种合并策略各跑一遍（口径显式）
    auto runMode = [](const std::string& mode) {
        Env e2(makePack(json::array(
            {ruleDef("x", "warn", thresholdCond("m", "gt", 1),
                     json{{"dedupWindowMs", 10000}, {"merge", mode}})})));
        e2.e().observe(e2.obs("x", "e", 3, 1000, "m"));
        e2.e().observe(e2.obs("x", "e", 7, 2000, "m"));
        e2.e().observe(e2.obs("x", "e", 5, 3000, "m"));
        return e2.e().getAlert("a-1").value();
    };
    const AlertRecord acc = runMode("accumulate");
    CHECK_EQ(acc.count, int64_t(3));
    CHECK_EQ(acc.lastValue, 5.0);
    const AlertRecord latest = runMode("latest");
    CHECK_EQ(latest.count, int64_t(1));       // 只更新最近值，不累加计数
    CHECK_EQ(latest.lastValue, 5.0);
    CHECK_EQ(latest.lastAt, int64_t(3000));
    const AlertRecord maxMode = runMode("max");
    CHECK_EQ(maxMode.lastValue, 7.0);         // 取历史最大
    const AlertRecord keep = runMode("keep");
    CHECK_EQ(keep.count, int64_t(1));
    CHECK_EQ(keep.lastValue, 3.0);            // 值不动
    CHECK_EQ(keep.lastAt, int64_t(3000));     // 最近时间仍刷新（ALT-GEN-06 可读）

    // 合并历史留痕：每次合并一条（可追溯）
    CHECK_EQ(acc.mergeHistory.size(), std::size_t(2));
    CHECK_EQ(acc.mergeHistory[0].at, int64_t(2000));
    CHECK_EQ(acc.mergeHistory[1].at, int64_t(3000));
}

/// ALT-DEDUP-03：抖动抑制 —— 阈值附近震荡 MUST NOT 产生告警风暴（参数可配）
void dedup03_flapping_suppression() {
    requires_("ALT-DEDUP-03");
    // ---- 主场景：确认次数 + 迟滞余量，全程 100 次震荡 → 0 条告警 ----
    const json flapCond = json{{"type", "bool"},
                               {"logic", "allOf"},
                               {"conditions", json::array({
                                    thresholdCond("v", "gt", 10.0),
                                    thresholdCond("v", "lt", 11.0)})}};
    json flapRule = ruleDef("flap", "warn", flapCond, json{{"dedupWindowMs", 1000}});
    flapRule["confirmCount"] = 2;
    const json bigStorm = json{{"enabled", true}, {"windowMs", 1000}, {"maxRaised", 10000},
                               {"level", "error"}, {"title", "风暴（{folded}）"}};
    Env env(makePack(json::array({flapRule}), json(), bigStorm));
    int raised = 0;
    for (int i = 0; i < 100; ++i) {
        const double v = (i % 2 == 0) ? 10.5 : 10.0;  // 10.5 满足、10.0 不满足 → 连续序列永远只有 1 次
        const AlertResult r = env.e().observe(env.obs("flap", "e", v, 1000 + i * 100, "v"));
        if (r.raised) ++raised;
    }
    CHECK_EQ(raised, 0);  // **无风暴**：100 次震荡，0 条告警
    CHECK_EQ(env.e().listAlerts().total, 0);
    CHECK_EQ(env.e().metrics().flappedBlocked, int64_t(50));  // 只有满足条件的 50 次才走到确认计数

    // ---- 对照：同样 100 次输入，但规则**没配**抖动抑制 → 每次触发各成一条 ----
    Env ctrl(makePack(json::array({ruleDef("flap", "warn", flapCond,
                                                  json{{"dedupWindowMs", 0}}) }),
                      json(), bigStorm));
    int ctrlRaised = 0;
    for (int i = 0; i < 100; ++i) {
        const AlertResult r =
            ctrl.e().observe(ctrl.obs("flap", "e", 10.5, 1000 + i * 100, "v"));
        if (r.raised) ++ctrlRaised;
    }
    note("      抖动抑制实测：100 次阈值附近震荡 → 抑制后 %d 条告警；同输入未抑制则 %d 条\n",
         raised, ctrlRaised);
    CHECK_EQ(ctrlRaised, 100);
    CHECK_EQ(raised, 0);  // 抑制后条数受控（0 条 = 完全没有风暴）

    // ---- 确认次数：连续 3 次才报（中间断一次即从头累计） ----
    {
        json conf = ruleDef("conf", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}});
        conf["confirmCount"] = 3;
        Env e2(makePack(json::array({conf})));
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 5, 1000, "m")).raised, false);
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 5, 2000, "m")).raised, false);
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 0, 2100, "m")).raised, false);  // 断开
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 5, 3000, "m")).raised, false);  // 重新计数第 1 次
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 5, 4000, "m")).raised, false);
        CHECK_EQ(e2.e().observe(e2.obs("conf", "e", 5, 5000, "m")).raised, true);   // 第 3 次
    }
    // ---- 最小驻留：满足后必须持续 minDwellMs 才报 ----
    {
        json dwell = ruleDef("dwell", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}});
        dwell["minDwellMs"] = 3000;
        Env e3(makePack(json::array({dwell})));
        CHECK_EQ(e3.e().observe(e3.obs("dwell", "e", 5, 1000, "m")).raised, false);
        CHECK_EQ(e3.e().observe(e3.obs("dwell", "e", 5, 3000, "m")).raised, false);
        CHECK_EQ(e3.e().observe(e3.obs("dwell", "e", 5, 4000, "m")).raised, true);
    }
    // ---- 阈值附近震荡 + 窗口内合并：8 次震荡（4 次满足）→ 1 条、计数 4 ----
    {
        json hyst = ruleDef("hyst", "warn", json{{"type", "threshold"}, {"field", "v"},
                                                 {"operator", "gt"}, {"value", 10.0}},
                            json{{"dedupWindowMs", 60000}, {"recoverOn", "none"}});
        Env e4(makePack(json::array({hyst})));
        int hystRaised = 0;
        const double seq[] = {10.01, 9.99, 10.02, 9.98, 10.03, 9.97, 10.04, 9.96};
        for (int i = 0; i < 8; ++i) {
            const AlertResult r = e4.e().observe(e4.obs("hyst", "e", seq[i], 1000 + i * 100, "v"));
            if (r.raised) ++hystRaised;
        }
        CHECK_EQ(hystRaised, 1);                    // 8 次震荡 → 1 条告警
        CHECK_EQ(e4.e().listAlerts().total, 1);
        CHECK_EQ(e4.e().getAlert("a-1").value().count, int64_t(4));  // 4 次满足被累加
    }
    // ---- 冷却期：告警关闭后 cooldownMs 内不新开，只并入原记录并重新打开 ----
    {
        json cool = ruleDef("cool", "warn", json{{"type", "threshold"}, {"field", "v"},
                                                 {"operator", "gt"}, {"value", 10.0}},
                            json{{"dedupWindowMs", 0}, {"cooldownMs", 30000}, {"recoverOn", "close"}});
        Env e5(makePack(json::array({cool})));
        CHECK_EQ(e5.e().observe(e5.obs("cool", "e", 11, 1000, "v")).raised, true);   // 新开
        CHECK_EQ(e5.e().observe(e5.obs("cool", "e", 0, 2000, "v")).recovered, true); // 关闭
        const AlertResult again = e5.e().observe(e5.obs("cool", "e", 11, 3000, "v"));  // 冷却期内
        CHECK_EQ(again.raised, false);
        CHECK_EQ(again.merged, true);              // 不新开，并入原记录
        CHECK_EQ(again.alert.state, AlertState::Active);  // 重新打开
        CHECK_EQ(again.alert.count, int64_t(2));
        CHECK_EQ(e5.e().listAlerts().total, 1);    // 仍是一条
        // 冷却期过后 → 新开一条
        const AlertResult later = e5.e().observe(e5.obs("cool", "e", 11, 40000, "v"));
        CHECK_EQ(later.raised, true);
        CHECK_EQ(e5.e().listAlerts().total, 2);
    }
}

/// ALT-DEDUP-04：升级语义 —— 合并中级别升高 MUST 记录升级事件并保留原级别历史
void dedup04_upgrade_records_and_keeps_history() {
    requires_("ALT-DEDUP-04");
    const json base = ruleDef("lvl", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 10000}, {"merge", "accumulate"}});
    json escalated = base;
    escalated["level"] = "error";

    Env env(makePack(json::array({base})));
    const AlertResult r1 = env.e().observe(env.obs("lvl", "e", 5, 1000, "m"));
    CHECK_EQ(r1.raised, true);
    CHECK_EQ(r1.alert.level, std::string("warn"));
    CHECK_EQ(r1.alert.originLevel, std::string("warn"));

    // 规则包升级为更高一级后重新装载（同一规则 id，仍在去重窗口内）
    CHECK_EQ(env.e().loadRules(makePack(json::array({escalated}))).code, 0);
    env.sink->clear();
    const AlertResult r2 = env.e().observe(env.obs("lvl", "e", 5, 3000, "m"));
    CHECK_EQ(r2.merged, true);
    CHECK_EQ(r2.alert.alertId, r1.alert.alertId);            // 仍是同一条
    CHECK_EQ(r2.alert.level, std::string("error"));          // 级别升高
    CHECK_EQ(r2.alert.originLevel, std::string("warn"));     // **原级别保留**
    CHECK_EQ(r2.alert.levelHistory.size(), std::size_t(2));  // 历史两条
    CHECK_EQ(r2.alert.levelHistory[0].level, std::string("warn"));
    CHECK_EQ(r2.alert.levelHistory[0].reason, std::string("created"));
    CHECK_EQ(r2.alert.levelHistory[1].level, std::string("error"));
    CHECK_EQ(r2.alert.levelHistory[1].reason, std::string("upgraded"));
    CHECK_EQ(r2.alert.mergeHistory.back().upgraded, true);
    CHECK_EQ(r2.alert.mergeHistory.back().fromLevel, std::string("warn"));
    CHECK_EQ(r2.alert.mergeHistory.back().toLevel, std::string("error"));
    CHECK_EQ(env.e().metrics().upgrades, int64_t(1));
    // 升级在事件负载里可读（alert.updated 的 upgraded 字段 + fromLevel）
    const auto ups = env.sink->updatedCopy();
    CHECK_EQ(ups.size(), std::size_t(1));
    CHECK_EQ(ups[0].payload["upgraded"], true);
    CHECK_EQ(ups[0].payload["fromLevel"], std::string("warn"));
    CHECK_EQ(ups[0].payload["level"], std::string("error"));

    // 级别**降低**不构成升级（MUST NOT 记升级）
    json lower = base;
    lower["level"] = "info";
    CHECK_EQ(env.e().loadRules(makePack(json::array({lower}))).code, 0);
    const AlertResult r3 = env.e().observe(env.obs("lvl", "e", 5, 4000, "m"));
    CHECK_EQ(r3.merged, true);
    CHECK_EQ(r3.alert.level, std::string("error"));          // 保持
    CHECK_EQ(r3.alert.levelHistory.size(), std::size_t(2));  // 无新历史
    CHECK_EQ(env.e().metrics().upgrades, int64_t(1));
}

/// ALT-DEDUP-05：抑制关系 —— 父告警存在时抑制子告警；父告警关闭后子告警恢复产生
void dedup05_parent_suppresses_child_until_closed() {
    requires_("ALT-DEDUP-05");
    const json items = json::array({
        ruleDef("parent", "error", thresholdCond("online", "lt", 1),
                json{{"dedupWindowMs", 60000}, {"suppressChildren", true}}),
        ruleDef("child", "warn", thresholdCond("signal", "lt", 0.45),
                json{{"dedupWindowMs", 60000}, {"parent", "parent"}}),
    });
    Env env(makePack(items));
    AlertEngine& e = env.e();

    // 父先产生
    const AlertResult p = e.observe(env.obs("parent", "dev-1", 0, 1000, "online"));
    CHECK_EQ(p.raised, true);
    // 子被抑制（同一实体）
    const AlertResult c1 = e.observe(env.obs("child", "dev-1", 0.2, 2000, "signal"));
    CHECK_EQ(c1.raised, false);
    CHECK_EQ(c1.suppressed, true);
    CHECK(c1.suppressReason.find("parent") != std::string::npos);
    CHECK_EQ(e.counts().alertsRaised, int64_t(1));
    CHECK_EQ(e.metrics().suppressedByParent, int64_t(1));

    // 抑制是**按实体**的：另一实体的子告警照常产生
    const AlertResult c2 = e.observe(env.obs("child", "dev-9", 0.2, 3000, "signal"));
    CHECK_EQ(c2.raised, true);

    // 父告警关闭 → 子告警恢复产生
    CHECK_EQ(e.close(p.alert.alertId, "op-1", "已确认失联").code, 0);
    const AlertResult c3 = e.observe(env.obs("child", "dev-1", 0.2, 4000, "signal"));
    CHECK_EQ(c3.raised, true);
    CHECK_EQ(e.counts().alertsRaised, int64_t(3));

    // 反向批量声明（suppressChildrenOf）等价生效
    const json items2 = json::array({
        ruleDef("p2", "error", thresholdCond("online", "lt", 1), json{{"dedupWindowMs", 60000}}),
        ruleDef("c2", "warn", thresholdCond("signal", "lt", 0.45),
                json{{"dedupWindowMs", 60000}, {"parent", "p2"}}),
    });
    json items3 = items2;
    items3[0]["suppressChildrenOf"] = json::array({"c2"});
    Env env2(makePack(items3));
    CHECK_EQ(env2.e().observe(env2.obs("p2", "d", 0, 1000, "online")).raised, true);
    CHECK_EQ(env2.e().observe(env2.obs("c2", "d", 0.2, 2000, "signal")).suppressed, true);
}

/// ALT-DEDUP-06：风暴保护 —— 单位时间告警条数超上限时触发"汇总告警"并**如实上报被折叠条数**
void dedup06_storm_protection_reports_folded_count() {
    requires_("ALT-DEDUP-06");
    const json storm = json{{"enabled", true},
                            {"windowMs", 1000},
                            {"maxRaised", 50},
                            {"level", "error"},
                            {"title", "告警风暴：{windowMs}ms 内超过 {maxRaised} 条，已折叠 {folded} 条"}};
    // 每条观测来自**不同实体**（否则会被去重合并，测不到风暴）
    Env env(makePack(json::array(
        {ruleDef("s", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}})}), json(), storm));

    int raised = 0, folded = 0;
    for (int i = 0; i < 100; ++i) {
        const AlertResult r = env.e().observe(env.obs("s", "e" + std::to_string(i), 5, 1000 + i, "m"));
        if (r.raised) ++raised;
        if (r.folded) ++folded;
    }
    note("      风暴保护实测：同一秒内 100 条触发（上限 50）→ 新开 %d 条、折叠 %d 条、"
         "台账共 %d 条\n", raised, folded, env.e().listAlerts().total);

    CHECK_EQ(raised, 51);   // 50 条业务告警 + 1 条汇总告警
    CHECK_EQ(folded, 50);   // 第 51 条触发起被折叠（上限 50 → 后 50 条进汇总）
    CHECK_EQ(env.e().metrics().folded, int64_t(50));

    // 汇总告警存在，且**如实上报被折叠条数**
    AlertQuery q;
    q.kindIn = {"aggregate"};
    const AlertQueryResult agg = env.e().listAlerts(q);
    CHECK_EQ(agg.total, 1);
    CHECK_EQ(agg.items[0].ruleId, std::string("$storm"));
    CHECK_EQ(agg.items[0].level, std::string("error"));
    CHECK_EQ(agg.items[0].foldedCount, int64_t(50));
    CHECK_EQ(agg.items[0].count, int64_t(50));
    CHECK(agg.items[0].title.find("50") != std::string::npos);  // 标题模板也带上折叠条数

    // 台账条数 = 50 条业务 + 1 条汇总；业务告警本身没有被丢弃（只是不再新开）
    CHECK_EQ(env.e().listAlerts().total, 51);
    CHECK_EQ(env.e().counts().folded, int64_t(50));

    // 事件负载里也带 foldedCount（宿主与报告无需自己算）
    bool sawAggregate = false;
    for (const auto& rec : env.sink->raisedCopy()) {
        if (rec.payload.value("ruleId", std::string()) == "$storm") {
            sawAggregate = true;
            CHECK(rec.payload.contains("foldedCount"));
            CHECK(rec.payload["foldedCount"].get<int64_t>() >= 1);
        }
    }
    CHECK_EQ(sawAggregate, true);

    // 窗口滑过之后恢复正常产生
    const AlertResult later =
        env.e().observe(env.obs("s", "e-later", 5, 1000 + 5000, "m"));
    CHECK_EQ(later.raised, true);
    CHECK_EQ(later.folded, false);

    // 未启用风暴保护时：全部照常产生（同一输入）
    Env plain(makePack(json::array(
        {ruleDef("s", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}})})));
    int plainRaised = 0;
    for (int i = 0; i < 100; ++i) {
        if (plain.e().observe(plain.obs("s", "e" + std::to_string(i), 5, 1000 + i, "m")).raised) {
            ++plainRaised;
        }
    }
    CHECK_EQ(plainRaised, 100);
}

// ############################################################################
// ALT-ACK 确认与生命周期（5 条）
// ############################################################################

namespace {

/// 造一条活动告警，返回其 id
std::string raiseOne(Env& env, const std::string& rule = "r", const std::string& entity = "e",
                     int64_t ts = 1000) {
    const AlertResult r = env.e().observe(env.obs(rule, entity, 5, ts, "m"));
    return r.alert.alertId;
}

json oneRulePack(const json& extra = json::object()) {
    json item = ruleDef("r", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}});
    for (auto it = extra.begin(); it != extra.end(); ++it) item[it.key()] = it.value();
    return makePack(json::array({item}));
}

}  // namespace

/// ALT-ACK-01：状态机 `active → acknowledged → closed`，可 `reopen`；非法迁移 MUST 被拒绝
void ack01_state_machine_and_illegal_transitions() {
    requires_("ALT-ACK-01");
    Env env(oneRulePack());
    AlertEngine& e = env.e();
    const std::string id = raiseOne(env);
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Active);

    // 合法：active → acknowledged
    const ActionResult a1 = e.acknowledge(id, "op-1", "已看到");
    CHECK_EQ(a1.code, 0);
    CHECK_EQ(a1.changed, true);
    CHECK_EQ(a1.fromState, std::string("active"));
    CHECK_EQ(a1.toState, std::string("acknowledged"));
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Acknowledged);

    // 合法：acknowledged → closed
    const ActionResult c1 = e.close(id, "op-2", "已处置");
    CHECK_EQ(c1.code, 0);
    CHECK_EQ(c1.fromState, std::string("acknowledged"));
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Closed);

    // 合法：closed → active（reopen）
    const ActionResult r1 = e.reopen(id, "op-3", "问题复发");
    CHECK_EQ(r1.code, 0);
    CHECK_EQ(r1.changed, true);
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Active);

    // 非法：closed 的告警不可直接确认 —— 先关闭再试
    const ActionResult c2 = e.close(id, "op-4", "关闭");
    CHECK_EQ(c2.code, 0);
    const ActionResult bad = e.acknowledge(id, "op-5", "");
    CHECK_EQ(bad.code, 1003);
    CHECK_EQ(bad.status, ResultStatus::Rejected);
    CHECK(bad.message.find("非法状态迁移") != std::string::npos);
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Closed);  // 状态未变

    // 非法：未知 id → 1004
    const ActionResult ghost = e.acknowledge("a-999", "op", "");
    CHECK_EQ(ghost.code, 1004);
    // 非法：空 id → 1000
    CHECK_EQ(e.acknowledge("", "op", "").code, 1000);
    // 拒绝计数如实
    CHECK_EQ(e.metrics().rejected, int64_t(2));
    CHECK_EQ(e.metrics().notFound, int64_t(1));
}

/// ALT-ACK-02：确认幂等 —— 重复确认 MUST 返回成功并标注"已确认"，MUST NOT 报错
void ack02_acknowledge_is_idempotent() {
    requires_("ALT-ACK-02");
    Env env(oneRulePack());
    AlertEngine& e = env.e();
    const std::string id = raiseOne(env);
    env.sink->clear();
    // 另起一条（不同实体）用于后面的关闭 / 重开幂等检查，避免与上面的语义混在一起
    const std::string other = raiseOne(env, "r", "e2", 5000);

    const ActionResult a1 = e.acknowledge(id, "op-1", "看一下");
    CHECK_EQ(a1.code, 0);
    CHECK_EQ(a1.idempotent, false);
    CHECK_EQ(a1.changed, true);
    // **连点两次不报错**：第二次是幂等命中（code=0 + idempotent=true，零副作用）
    const ActionResult a2 = e.acknowledge(id, "op-1", "看一下");
    CHECK_EQ(a2.code, 0);
    CHECK_EQ(a2.idempotent, true);
    CHECK_EQ(a2.status, ResultStatus::AlreadyApplied);
    CHECK_EQ(a2.changed, false);
    CHECK_EQ(a2.conflict, false);  // 幂等成功 MUST NOT 被表达成冲突（C16）
    // 第三次、第四次仍然成功（不累积副作用）
    CHECK_EQ(e.acknowledge(id, "op-2", "").code, 0);
    CHECK_EQ(e.acknowledge(id, "op-3", "").idempotent, true);
    const auto rec = e.getAlert(id).value();
    CHECK_EQ(rec.state, AlertState::Acknowledged);
    CHECK_EQ(rec.ackedBy, std::string("op-1"));  // 首次确认者不被后续重复确认覆盖
    // 状态历史只追加了 1 条 acknowledge（零副作用 —— 幂等不得写历史）
    int ackEntries = 0;
    for (const auto& s : rec.stateHistory) {
        if (s.action == "acknowledge") ++ackEntries;
    }
    CHECK_EQ(ackEntries, 1);
    CHECK_EQ(e.metrics().idempotentHits, int64_t(3));
    // JSON 信封：code=0 且 data.idempotent=true（CTR-EC-01，前端 client.ts 不会抛错）
    const json j = a2.toJson();
    CHECK_EQ(j["code"], 0);
    CHECK_EQ(j["data"]["idempotent"], true);
    // 幂等命中仍投递一条标注 idempotent=true 的 alert.acked（调用方可据此对账）
    const auto acks = env.sink->ackedCopy();
    CHECK_EQ(acks.size(), std::size_t(4));  // 1 次成功 + 3 次幂等命中
    CHECK(acks[0].payload.is_object());
    CHECK_EQ(acks[0].payload["idempotent"], false);
    CHECK_EQ(acks[1].payload["idempotent"], true);

    // 已关闭的再次关闭 → 幂等命中（不是非法迁移）
    CHECK_EQ(e.close(other, "op-9", "结案").code, 0);
    const ActionResult c2 = e.close(other, "op-9", "结案");
    CHECK_EQ(c2.code, 0);
    CHECK_EQ(c2.idempotent, true);
    // 已活动的再次重开 → 幂等命中
    CHECK_EQ(e.reopen(other, "op-9", "").code, 0);
    const ActionResult r2 = e.reopen(other, "op-9", "");
    CHECK_EQ(r2.code, 0);
    CHECK_EQ(r2.idempotent, true);
}

/// ALT-ACK-03：操作留痕 —— 确认/关闭/重开 MUST 记录操作者与时间（可复原）
void ack03_audit_trail_is_recoverable() {
    requires_("ALT-ACK-03");
    Env env(oneRulePack());
    AlertEngine& e = env.e();
    env.clock->set(1750000001000LL);
    const std::string id = raiseOne(env);
    env.clock->set(1750000002000LL);
    CHECK_EQ(e.acknowledge(id, "operator-alpha", "收到").code, 0);
    // 确认后即读：操作者与时间已落台账（ALT-ACK-03）
    const auto acked = e.getAlert(id).value();
    CHECK_EQ(acked.ackedBy, std::string("operator-alpha"));
    CHECK_EQ(acked.ackedAt, int64_t(1750000002000LL));
    CHECK_EQ(acked.state, AlertState::Acknowledged);

    env.clock->set(1750000003000LL);
    CHECK_EQ(e.close(id, "operator-beta", "已恢复业务").code, 0);
    const auto closed = e.getAlert(id).value();
    CHECK_EQ(closed.closedBy, std::string("operator-beta"));
    CHECK_EQ(closed.closedAt, int64_t(1750000003000LL));
    CHECK_EQ(closed.closeReason, std::string("已恢复业务"));
    CHECK_EQ(closed.state, AlertState::Closed);

    // 重开是**重新开始**：确认/关闭字段被清空（新的一轮处置），但历史一条不少
    env.clock->set(1750000004000LL);
    CHECK_EQ(e.reopen(id, "operator-gamma", "再次出现").code, 0);
    const auto rec = e.getAlert(id).value();
    CHECK_EQ(rec.state, AlertState::Active);
    CHECK_EQ(rec.ackedBy, std::string(""));
    CHECK_EQ(rec.closedBy, std::string(""));
    // 状态历史逐条含 操作者 + 时间（审计可复原）
    CHECK_EQ(rec.stateHistory.size(), std::size_t(4));  // create / acknowledge / close / reopen
    CHECK_EQ(rec.stateHistory[0].action, std::string("create"));
    CHECK_EQ(rec.stateHistory[1].action, std::string("acknowledge"));
    CHECK_EQ(rec.stateHistory[1].operatorId, std::string("operator-alpha"));
    CHECK_EQ(rec.stateHistory[1].at, int64_t(1750000002000LL));
    CHECK_EQ(rec.stateHistory[1].fromState, std::string("active"));
    CHECK_EQ(rec.stateHistory[1].toState, std::string("acknowledged"));
    CHECK_EQ(rec.stateHistory[2].action, std::string("close"));
    CHECK_EQ(rec.stateHistory[2].operatorId, std::string("operator-beta"));
    CHECK_EQ(rec.stateHistory[3].action, std::string("reopen"));
    CHECK_EQ(rec.stateHistory[3].operatorId, std::string("operator-gamma"));
    // 落进台账 JSON（宿主可直接持久化/展示）
    const json j = rec.toJson();
    CHECK_EQ(j["stateHistory"].size(), std::size_t(4));
    CHECK_EQ(j["stateHistory"][1]["operatorId"], std::string("operator-alpha"));
    // 也走 ILogSink::commandAudit（可选放大器；未注入时留痕仍在台账里）
    CHECK(env.log->audits >= 3);
    // 台账快照被真实写入 Store（写前提交的落点）
    CHECK(env.store->saves >= 4);
}

/// ALT-ACK-04：消音 —— 可临时抑制某规则或某实体（有时限），到期自动恢复
void ack04_mute_is_time_limited_and_auto_recovers() {
    requires_("ALT-ACK-04");
    const json mute = json{{"defaultMs", 10000}, {"maxMs", 60000}};
    const json items = json::array({
        ruleDef("r1", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
        ruleDef("r2", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
    });
    Env env(makePack(items, json(), json(), mute));
    AlertEngine& e = env.e();
    env.clock->set(1750000000000LL);

    // 按规则消音（缺省时长来自规则包 mute.defaultMs）
    const MuteResult m1 = e.mute(MuteRequest{MuteScope::Rule, "r1"});
    CHECK_EQ(m1.code, 0);
    CHECK_EQ(m1.mute.until - m1.mute.from, int64_t(10000));
    // 消音期间不产生
    const AlertResult s1 = e.observe(env.obs("r1", "e", 5, 1000, "m"));
    CHECK_EQ(s1.raised, false);
    CHECK_EQ(s1.muted, true);
    CHECK(s1.suppressReason.find("消音") != std::string::npos);
    CHECK_EQ(e.metrics().mutedSuppressed, int64_t(1));
    // 消音只影响被声明的规则
    CHECK_EQ(e.observe(env.obs("r2", "e", 5, 1000, "m")).raised, true);

    // 到期自动恢复：时钟推进到 until 之后
    env.clock->set(1750000010000LL);
    const json t = e.tick(0);  // 由宿主驱动时间前进
    CHECK_EQ(t["mutesExpired"], 1);
    CHECK_EQ(t["mutesActive"], 0);
    const AlertResult s2 = e.observe(env.obs("r1", "e", 5, 11000, "m"));
    CHECK_EQ(s2.raised, true);  // **到期后恢复产生**
    CHECK_EQ(s2.muted, false);

    // 按时长显式消音 + 到期前 `mutes()` 如实标注 expired
    CHECK_EQ(e.mute(MuteRequest{MuteScope::Rule, "r1", "", 5000, 0, "op", "维护"}).code, 0);
    std::vector<MuteEntry> ms = e.mutes();
    CHECK_EQ(ms.size(), std::size_t(1));
    CHECK_EQ(ms[0].expired, false);
    CHECK_EQ(ms[0].operatorId, std::string("op"));
    env.clock->set(1750000016001LL);
    ms = e.mutes();
    CHECK_EQ(ms[0].expired, true);

    // 按**实体**消音：只压住该实体
    Env env2(makePack(items, json(), json(), mute));
    env2.clock->set(1750000000000LL);
    CHECK_EQ(env2.e().mute(MuteRequest{MuteScope::Entity, "dev-1", "", 5000, 0, "op", ""}).code, 0);
    CHECK_EQ(env2.e().observe(env2.obs("r1", "dev-1", 5, 1000, "m")).muted, true);
    CHECK_EQ(env2.e().observe(env2.obs("r1", "dev-2", 5, 1000, "m")).raised, true);

    // 显式取消消音 → 立即恢复产生
    Env env3(makePack(items, json(), json(), mute));
    env3.clock->set(1750000000000LL);
    const MuteResult m3 = env3.e().mute(MuteRequest{MuteScope::Rule, "r1"});
    CHECK_EQ(env3.e().observe(env3.obs("r1", "e", 5, 1000, "m")).muted, true);
    CHECK_EQ(env3.e().unmute(m3.mute.muteId, "op").code, 0);
    CHECK_EQ(env3.e().observe(env3.obs("r1", "e", 5, 2000, "m")).raised, true);
    CHECK_EQ(env3.e().unmute("m-999", "op").code, 1004);

    // 非法请求：超过规则包上限 / 未给时长且规则包无缺省 / 未知规则
    CHECK_EQ(e.mute(MuteRequest{MuteScope::Rule, "r1", "", 999999, 0, "op", ""}).code, 1000);
    CHECK_EQ(e.mute(MuteRequest{MuteScope::Rule, "ghost"}).code, 1004);
    Env noDefault(makePack(items));
    CHECK_EQ(noDefault.e().mute(MuteRequest{MuteScope::Rule, "r1"}).code, 1000);
}

/// ALT-ACK-05：自动关闭 —— 条件恢复时按规则自动关闭并留痕
void ack05_auto_close_on_recovery_is_traced() {
    requires_("ALT-ACK-05");
    json item = ruleDef("ac", "warn", thresholdCond("m", "gt", 10),
                        json{{"recoverOn", "close"}, {"dedupWindowMs", 0}});
    Env env(makePack(json::array({item})));
    AlertEngine& e = env.e();
    env.clock->set(1750000005000LL);
    // 该规则的阈值是 `m > 10`，所以触发观测必须给 11（不是 raiseOne 缺省的 5）
    const AlertResult up = e.observe(env.obs("ac", "e", 11, 1000, "m"));
    CHECK_EQ(up.raised, true);
    const std::string id = up.alert.alertId;
    CHECK_EQ(e.getAlert(id).value().state, AlertState::Active);

    env.clock->set(1750000009000LL);
    const AlertResult rec = e.observe(env.obs("ac", "e", 0, 9000, "m"));
    CHECK_EQ(rec.recovered, true);
    const auto r = e.getAlert(id).value();
    CHECK_EQ(r.state, AlertState::Closed);
    // 自动关闭的时间戳取**观测时间**（与持续时间/去重窗口同一时间源 —— ALT-NFR-02 的确定性口径）
    CHECK_EQ(r.closedAt, int64_t(9000));
    CHECK_EQ(r.closeReason, std::string("condition-recovered"));
    // 留痕：action=auto-close，recovered 原因可读
    CHECK_EQ(r.stateHistory.back().action, std::string("auto-close"));
    CHECK_EQ(r.stateHistory.back().toState, std::string("closed"));
    CHECK_EQ(r.stateHistory.back().reason, std::string("condition-recovered"));
    // 自动关闭的事件也走 alert.acked（state=closed）
    const auto acks = env.sink->ackedCopy();
    CHECK_EQ(acks.size(), std::size_t(1));
    CHECK_EQ(acks[0].payload["state"], std::string("closed"));
    CHECK_EQ(acks[0].payload["action"], std::string("auto-close"));
    CHECK_EQ(e.metrics().autoClosed, int64_t(1));
}

// ############################################################################
// ALT-SUB 台账与订阅（6 条）
// ############################################################################

namespace {

/// 造一份多维度台账：3 条规则 × 3 个实体 × 2 个任务，供筛选用例共用
struct LedgerFixture {
    std::unique_ptr<Env> env;
    explicit LedgerFixture() {
        const json items = json::array({
            ruleDef("r-info", "info", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
            ruleDef("r-warn", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
            ruleDef("r-error", "error", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
        });
        env = std::make_unique<Env>(makePack(items));
        // t=1000..3000：任务 m-1；t=10000：任务 m-2（观测覆盖规则级任务）
        env->e().observe(obsOf("r-info", "dev-1", "m-1", 1000));
        env->e().observe(obsOf("r-warn", "dev-1", "m-1", 2000));
        env->e().observe(obsOf("r-error", "dev-1", "m-1", 3000));
        env->e().observe(obsOf("r-warn", "dev-2", "m-1", 4000));
        env->e().observe(obsOf("r-error", "dev-2", "m-2", 10000));
        env->e().observe(obsOf("r-info", "dev-3", "m-2", 11000));
    }
    Observation obsOf(const std::string& rule, const std::string& entity, const std::string& mission,
                      int64_t ts) {
        Observation o = env->obs(rule, entity, 5, ts, "m");
        o.missionId = mission;
        return o;
    }
};

}  // namespace

/// ALT-SUB-01：台账查询 —— 按级别/实体/时间区间/状态筛选，分页与**超限截断（不静默丢）**
void sub01_ledger_query_filters_and_truncation() {
    requires_("ALT-SUB-01");
    LedgerFixture f;
    AlertEngine& e = f.env->e();
    CHECK_EQ(e.listAlerts().total, 6);

    // 按级别
    AlertQuery q;
    q.levelIn = {"error"};
    CHECK_EQ(e.listAlerts(q).total, 2);
    q.levelIn = {"info", "warn"};
    CHECK_EQ(e.listAlerts(q).total, 4);
    // 按实体
    q = AlertQuery{};
    q.entityIn = {"dev-1"};
    CHECK_EQ(e.listAlerts(q).total, 3);
    q.entityIn = {"dev-1", "dev-2"};
    CHECK_EQ(e.listAlerts(q).total, 5);
    // 按规则
    q = AlertQuery{};
    q.ruleIn = {"r-warn"};
    CHECK_EQ(e.listAlerts(q).total, 2);
    // 按时间区间（缺省按 lastAt）
    q = AlertQuery{};
    q.from = 2000;
    q.to = 4000;
    CHECK_EQ(e.listAlerts(q).total, 3);
    q.useFirstAt = true;  // 口径可切换（首次时间）
    CHECK_EQ(e.listAlerts(q).total, 3);
    // 按状态
    CHECK_EQ(e.close(e.listAlerts().items[0].alertId, "op", "").code, 0);
    q = AlertQuery{};
    q.stateIn = {"closed"};
    CHECK_EQ(e.listAlerts(q).total, 1);
    q.stateIn = {"active"};
    CHECK_EQ(e.listAlerts(q).total, 5);
    q.stateIn = {"acknowledged"};
    CHECK_EQ(e.listAlerts(q).total, 0);
    q = AlertQuery{};
    q.activeOnly = true;
    CHECK_EQ(e.listAlerts(q).total, 5);
    // 组合筛选
    q = AlertQuery{};
    q.levelIn = {"error"};
    q.entityIn = {"dev-1"};
    q.from = 0;
    CHECK_EQ(e.listAlerts(q).total, 1);
    q.levelIn = {"error"};
    q.entityIn = {"dev-2"};
    CHECK_EQ(e.listAlerts(q).total, 1);

    // ---- 分页 + 超限截断（**不静默丢**） ----
    AlertQuery page;
    page.limit = 2;
    page.offset = 0;
    const AlertQueryResult p1 = e.listAlerts(page);
    CHECK_EQ(p1.total, 6);
    CHECK_EQ(p1.returned, 2);
    CHECK_EQ(p1.truncated, true);
    CHECK_EQ(p1.omitted, 4);
    CHECK(!p1.truncationReason.empty());
    CHECK(p1.truncationReason.find("截断") != std::string::npos);
    CHECK_EQ(p1.items.size(), std::size_t(2));
    page.offset = 4;
    const AlertQueryResult p2 = e.listAlerts(page);
    CHECK_EQ(p2.returned, 2);
    CHECK_EQ(p2.omitted, 0);
    CHECK_EQ(p2.truncated, false);
    // 两次分页拿到的 id 不重叠（真的翻页了，不是重复返回）
    CHECK(p1.items[0].alertId != p2.items[0].alertId);
    CHECK(p1.items[1].alertId != p2.items[1].alertId);
    // 超限截断次数如实计入观测
    CHECK_EQ(e.metrics().truncations, int64_t(1));

    // 缺省 limit = 100 → 不截断
    const AlertQueryResult all = e.listAlerts(AlertQuery{});
    CHECK_EQ(all.limit, 100);
    CHECK_EQ(all.returned, 6);
    CHECK_EQ(all.truncated, false);
    CHECK_EQ(all.omitted, 0);
    // 信封形状
    const json env = all.toEnvelope();
    CHECK_EQ(env["code"], 0);
    CHECK(env["data"].contains("truncated"));
    CHECK(env["data"].contains("omitted"));
    CHECK(env["data"]["items"].is_array());
    // 不存在的 id → nullopt（MUST NOT 返回空对象当真值 —— protocol §2.3）
    CHECK_EQ(e.getAlert("a-999").has_value(), false);
    // 空集合 = 不筛
    AlertQuery empty;
    empty.kindIn = {};
    CHECK_EQ(e.listAlerts(empty).total, 6);
}

/// ALT-SUB-02：与任务维度关联 —— 告警 MUST 可归属到任务；按任务筛选正确
void sub02_alerts_belong_to_mission() {
    requires_("ALT-SUB-02");
    // 规则级任务归属：观测未给 missionId 时回落规则声明的 missionId
    json ruleWithMission =
        ruleDef("rm", "warn", thresholdCond("m", "gt", 1), json{{"missionId", "mission-A"}});
    Env env(makePack(json::array({ruleWithMission})));
    const AlertResult r1 = env.e().observe(env.obs("rm", "e", 5, 1000, "m"));
    CHECK_EQ(r1.alert.missionId, std::string("mission-A"));

    // 观测给的 missionId **覆盖**规则缺省（宿主按任务分派）
    Observation o = env.obs("rm", "e2", 5, 2000, "m");
    o.missionId = "mission-B";
    const AlertResult r2 = env.e().observe(o);
    CHECK_EQ(r2.alert.missionId, std::string("mission-B"));

    AlertQuery q;
    q.missionIn = {"mission-A"};
    const AlertQueryResult qa = env.e().listAlerts(q);
    CHECK_EQ(qa.total, 1);
    CHECK_EQ(qa.items[0].alertId, r1.alert.alertId);
    q.missionIn = {"mission-B"};
    CHECK_EQ(env.e().listAlerts(q).total, 1);
    q.missionIn = {"mission-A", "mission-B"};
    CHECK_EQ(env.e().listAlerts(q).total, 2);
    q.missionIn = {"mission-C"};
    CHECK_EQ(env.e().listAlerts(q).total, 0);
    // 任务字段进入台账 JSON 与事件负载（供报告与 T7 复盘）
    CHECK_EQ(r2.alert.toJson()["missionId"], std::string("mission-B"));
    const auto raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(2));
    CHECK_EQ(raised[1].payload["missionId"], std::string("mission-B"));

    // 未声明任务归属且观测未给 → 字段省略（不写 null）；不会被任何任务筛选命中
    Env env2(makePack(json::array({ruleDef("rn", "warn", thresholdCond("m", "gt", 1))})));
    const AlertResult r3 = env2.e().observe(env2.obs("rn", "e", 5, 1000, "m"));
    CHECK_EQ(r3.alert.missionId, std::string(""));
    CHECK_EQ(r3.alert.toJson().contains("missionId"), false);
    CHECK_EQ(env2.e().listAlerts(AlertQuery{}).total, 1);
    AlertQuery q2;
    q2.missionIn = {"any"};
    CHECK_EQ(env2.e().listAlerts(q2).total, 0);
}

/// ALT-SUB-03：订阅式分发 —— 订阅者声明关心的规则/级别/实体，引擎**只推匹配项**
void sub03_subscription_filtering() {
    requires_("ALT-SUB-03");
    const json items = json::array({
        ruleDef("ra", "info", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
        ruleDef("rb", "error", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}}),
    });
    Env env(makePack(items));
    AlertEngine& e = env.e();

    AlertSubscription s1;
    s1.subscriberId = "ui-toast";
    s1.levels = {"error"};
    CHECK_EQ(e.subscribe(s1).code, 0);

    AlertSubscription s2;
    s2.subscriberId = "ops-dev1";
    s2.entityIds = {"dev-1"};
    CHECK_EQ(e.subscribe(s2).code, 0);

    AlertSubscription s3;
    s3.subscriberId = "audit-all";  // 空集合 = 全部
    CHECK_EQ(e.subscribe(s3).code, 0);
    CHECK_EQ(e.subscriptions().size(), std::size_t(3));

    // ① info + rb 规则 + dev-1 → 只有 ops-dev1 与 audit-all 收到
    e.observe(env.obs("ra", "dev-1", 5, 1000, "m"));
    auto raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(1));
    CHECK_EQ(raised[0].subscribers.size(), std::size_t(2));
    CHECK_EQ(raised[0].subscribers[0], std::string("audit-all"));  // 按注册顺序（有序）
    CHECK_EQ(raised[0].subscribers[1], std::string("ops-dev1"));
    CHECK_EQ(raised[0].broadcast, false);

    // ② error + rb + dev-2 → 只有 ui-toast 与 audit-all 收到
    env.sink->clear();
    e.observe(env.obs("rb", "dev-2", 5, 2000, "m"));
    raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(1));
    CHECK_EQ(raised[0].subscribers.size(), std::size_t(2));
    CHECK(raised[0].subscribers[0] == "audit-all");
    CHECK(raised[0].subscribers[1] == "ui-toast");

    // ③ 不匹配任何订阅者 → broadcast 标记，subscriberIds 为空（宿主自决）
    AlertEngine e3(AlertEngineOptions{});
    CHECK_EQ(e3.loadRules(makePack(json::array(
                 {ruleDef("rx", "warn", thresholdCond("m", "gt", 1))}))).code, 0);
    AlertSubscription s4;
    s4.subscriberId = "only-error";
    s4.levels = {"error"};
    CHECK_EQ(e3.subscribe(s4).code, 0);
    auto sink3 = std::make_shared<SpySink>();
    e3.setSink(sink3);
    e3.observe(Observation{"rx", "d", "", "m", 5, 3000, "", {}, {}});
    const auto r3 = sink3->raisedCopy();
    CHECK_EQ(r3.size(), std::size_t(1));
    CHECK_EQ(r3[0].subscribers.size(), std::size_t(0));  // 不匹配 → 不推给该订阅者
    CHECK_EQ(r3[0].broadcast, true);

    // 订阅覆盖 / 注销
    AlertSubscription s1b = s1;
    s1b.levels = {"info"};
    CHECK_EQ(e.subscribe(s1b).code, 0);
    CHECK_EQ(e.subscriptions().size(), std::size_t(3));
    CHECK_EQ(e.unsubscribe("ui-toast"), true);
    CHECK_EQ(e.unsubscribe("ui-toast"), false);
    env.sink->clear();
    e.observe(env.obs("rb", "dev-2", 5, 2100, "m"));
    raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(1));
    CHECK_EQ(raised[0].subscribers.size(), std::size_t(1));  // 只剩 audit-all
    // 非法 subscriberId / 未知级别 → 拒绝
    AlertSubscription bad;
    bad.subscriberId = "Bad-Id";
    CHECK_EQ(e.subscribe(bad).code, 1000);
    AlertSubscription bad2;
    bad2.subscriberId = "ok-id";
    bad2.levels = {"nonexistent"};
    CHECK_EQ(e.subscribe(bad2).code, 1000);
}

/// ALT-SUB-04：出口为反向接口 `IAlertSink`（引擎内无落库与广播）
void sub04_sink_is_the_only_output() {
    requires_("ALT-SUB-04");
    // 带窗口的规则：产生 → alert.raised；合并 → alert.updated；确认 → alert.acked
    const json pack = makePack(json::array(
        {ruleDef("sink", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 60000}})}));
    Env env(pack);
    AlertEngine& e = env.e();
    env.sink->clear();
    const AlertResult first = e.observe(env.obs("sink", "e", 5, 1000, "m"));
    CHECK_EQ(first.raised, true);
    // 产生 → onAlertRaised 被调（一次）
    CHECK_EQ(env.sink->raisedCount(), std::size_t(1));
    CHECK_EQ(env.sink->raisedCopy()[0].event, std::string("alert.raised"));
    // 合并 → onAlertUpdated
    const AlertResult second = e.observe(env.obs("sink", "e", 5, 2000, "m"));
    CHECK_EQ(second.merged, true);
    CHECK_EQ(env.sink->updatedCount(), std::size_t(1));
    // 确认 → onAlertAcked
    e.acknowledge(first.alert.alertId, "op", "");
    CHECK_EQ(env.sink->ackedCount(), std::size_t(1));
    CHECK_EQ(env.sink->ackedCopy()[0].event, std::string("alert.acked"));

    // 未注入 Sink 时引擎仍正常记账（MUST NOT 因此拒绝服务）
    AlertEngine bare;
    CHECK_EQ(bare.loadRules(oneRulePack()).code, 0);
    const AlertResult r = bare.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}});
    CHECK_EQ(r.raised, true);
    CHECK_EQ(bare.listAlerts().total, 1);
    CHECK_EQ(bare.capabilities().sinkInjected, false);
    CHECK_EQ(bare.capabilities().pureMemory, true);

    // 回调抛异常 → 被吞掉并计入 sinkErrors，MUST NOT 影响已生效的状态
    AlertEngine e2;
    CHECK_EQ(e2.loadRules(oneRulePack()).code, 0);
    e2.setSink(std::make_shared<ThrowingSink>());
    const AlertResult r2 = e2.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}});
    CHECK_EQ(r2.raised, true);
    CHECK_EQ(r2.code, 0);
    CHECK_EQ(e2.listAlerts().total, 1);
    CHECK_EQ(e2.metrics().sinkErrors, int64_t(1));

    // 写前提交：Store 写失败 → 本次操作整体失败 1005，**内存状态不变**
    AlertEngine e3;
    CHECK_EQ(e3.loadRules(oneRulePack()).code, 0);
    auto store = std::make_shared<MemStore>();
    e3.setStore(store);
    store->failNext = true;
    const AlertResult r3 = e3.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}});
    CHECK_EQ(r3.code, 1005);
    CHECK_EQ(r3.raised, false);
    CHECK_EQ(e3.listAlerts().total, 0);  // 内存未提交
    CHECK_EQ(e3.metrics().storeErrors, int64_t(1));
}

/// ALT-SUB-05：批量输出 —— 新客户端接入时可一次拉取当前活动告警（不是逐条推送）
void sub05_batch_active_alerts() {
    requires_("ALT-SUB-05");
    Env env(makePack(json::array(
        {ruleDef("a", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}})})));
    AlertEngine& e = env.e();
    for (int i = 0; i < 5; ++i) e.observe(env.obs("a", "d" + std::to_string(i), 5, 1000 + i, "m"));
    CHECK_EQ(e.close("a-1", "op", "").code, 0);  // 关掉一条

    const json batch = e.activeAlerts();
    CHECK_EQ(batch["total"], 4);       // 一次调用返回**全部**活动告警
    CHECK_EQ(batch["returned"], 4);
    CHECK_EQ(batch["items"].size(), std::size_t(4));
    CHECK_EQ(batch["limit"], 0);       // 0 = 不限量（批量的语义）
    CHECK_EQ(batch["truncated"], false);
    for (const auto& it : batch["items"]) {
        CHECK_EQ(it["active"], true);
        CHECK(it["state"] != "closed");
    }
    // 与逐条查询口径一致
    AlertQuery q;
    q.activeOnly = true;
    CHECK_EQ(e.listAlerts(q).total, 4);
}

/// ALT-SUB-06：`alert` 事件负载保持前端现有字段可读（前端零改动）
void sub06_event_payload_shape_and_compat() {
    requires_("ALT-SUB-06");
    // 用带窗口的规则：先产生（alert.raised），再合并（alert.updated），再确认（alert.acked）
    const json pack = makePack(json::array(
        {ruleDef("payload", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 60000}})}));
    Env env(pack);
    AlertEngine& e = env.e();
    env.sink->clear();
    const AlertResult first = e.observe(env.obs("payload", "e", 5, 1000, "m"));
    CHECK_EQ(first.raised, true);
    const std::string id = first.alert.alertId;
    const AlertResult second = e.observe(env.obs("payload", "e", 5, 2000, "m"));
    CHECK_EQ(second.merged, true);
    CHECK_EQ(e.acknowledge(id, "op-1", "收到").code, 0);

    // 引擎产生的事件名：protocol.md §4.4 已登记的三个（**引擎不包 WS 信封**，由宿主包）
    CHECK_EQ(env.sink->raisedCount(), std::size_t(1));
    CHECK_EQ(env.sink->updatedCount(), std::size_t(1));
    CHECK_EQ(env.sink->ackedCount(), std::size_t(1));
    CHECK_EQ(env.sink->raisedCopy()[0].event, std::string("alert.raised"));
    CHECK_EQ(env.sink->updatedCopy()[0].event, std::string("alert.updated"));
    CHECK_EQ(env.sink->ackedCopy()[0].event, std::string("alert.acked"));

    // `alert.raised` 的 data：契约字段逐字齐备
    const json raised = env.sink->raisedCopy()[0].payload;
    CHECK(raised.is_object());
    for (const char* k : {"alertId", "ruleId", "level", "entityId", "missionId", "firstAt",
                          "count"}) {
        CHECK_MSG(raised.contains(k), std::string("alert.raised 缺少契约字段 ") + k);
    }
    CHECK_EQ(raised["alertId"], id);
    CHECK_EQ(raised["ruleId"], std::string("payload"));
    CHECK_EQ(raised["level"], std::string("warn"));
    CHECK_EQ(raised["entityId"], std::string("e"));
    CHECK_EQ(raised["count"], int64_t(1));
    CHECK(raised["firstAt"].is_number_integer());
    // `alert.updated` 的契约字段
    const json updated = env.sink->updatedCopy()[0].payload;
    for (const char* k : {"alertId", "count", "lastAt", "level", "upgraded"}) {
        CHECK_MSG(updated.contains(k), std::string("alert.updated 缺少契约字段 ") + k);
    }
    CHECK_EQ(updated["alertId"], id);
    CHECK_EQ(updated["count"], int64_t(2));
    CHECK(updated["lastAt"].is_number_integer());
    CHECK(updated["upgraded"].is_boolean());
    // `alert.acked` 的契约字段
    const json acked = env.sink->ackedCopy()[0].payload;
    for (const char* k : {"alertId", "state", "operatorId", "at"}) {
        CHECK_MSG(acked.contains(k), std::string("alert.acked 缺少契约字段 ") + k);
    }
    CHECK_EQ(acked["state"], std::string("acknowledged"));
    CHECK_EQ(acked["operatorId"], std::string("op-1"));
    CHECK(acked["at"].is_number_integer());

    // 字段全 camelCase（CTR-EV-05），事件名符合 CTR-EV-01 且未占保留命名空间
    for (const json* j : {&raised, &updated, &acked}) {
        for (auto it = j->begin(); it != j->end(); ++it) {
            CHECK_MSG(it.key().find('_') == std::string::npos,
                      "事件字段名 MUST 为 camelCase：" + it.key());
        }
    }
    CHECK(env.sink->raisedCopy()[0].ts == raised["ts"].get<int64_t>());  // 同一 ts 同源
    // 缺失可选字段**省略**（不写 null）：entityId 为空时不出现
    AlertEngine e2;
    CHECK_EQ(e2.loadRules(makePack(json::array(
                 {ruleDef("bare", "warn", thresholdCond("m", "gt", 1))}))).code, 0);
    auto sink2 = std::make_shared<SpySink>();
    e2.setSink(sink2);
    e2.observe(Observation{"bare", "", "", "m", 5, 1000, "", {}, {}});
    const json r2 = sink2->raisedCopy()[0].payload;
    CHECK_EQ(r2.contains("entityId"), false);
    CHECK(r2.contains("alertId"));
}

// ############################################################################
// ALT-NFR 非功能性（7 条）
// ############################################################################

/// ALT-NFR-01：零外部依赖 —— 无 Web 框架、无 SQL；持久化走注入的 `IAlertStore`
void nfr01_zero_external_dependencies() {
    requires_("ALT-NFR-01");
    // 编译期即证明：本文件只 include <alert_engine/alert_engine.h> 与 C++ 标准库。
    // 运行期证明：四个出口全部由宿主注入，引擎不认识任何宿主实现。
    auto sink = std::make_shared<SpySink>();
    auto store = std::make_shared<MemStore>();
    auto clock = std::make_shared<FakeClock>();
    auto log = std::make_shared<SpyLog>();
    AlertEngineOptions opts;
    opts.store = store;
    opts.clock = clock;
    opts.sink = sink;
    opts.log = log;
    AlertEngine e(opts);
    CHECK_EQ(e.loadRules(oneRulePack()).code, 0);

    // 四个接口的签名逐条可用（编译期 + 运行期双重确认）
    IAlertSink& s = *sink;
    IAlertStore& st = *store;
    IClock& ck = *clock;
    ILogSink& lg = *log;
    AlertDelivery d;
    d.event = "alert.raised";
    s.onAlertRaised(d);
    s.onAlertUpdated(d);
    s.onAlertAcked(d);
    CHECK(st.save(AlertRecord{}));
    AlertRecord loaded;
    CHECK_EQ(st.load("nope", loaded), false);  // 未命中 → false（MUST NOT 当内部错误）
    CHECK(st.remove("nope") == false);
    CHECK(ck.nowMs() > 0);
    lg.log(0, "t", json::object());
    lg.commandAudit("acknowledge", "a-1", json::object());
    CHECK_EQ(sink->raisedCount(), std::size_t(1));

    // 未注入任何依赖 → 引擎仍可完整工作（纯内存 + 空 Sink + 系统时钟）
    AlertEngine bare;
    CHECK_EQ(bare.loadRules(oneRulePack()).code, 0);
    CHECK_EQ(bare.capabilities().pureMemory, true);
    CHECK_EQ(bare.capabilities().storeInjected, false);
    CHECK_EQ(bare.capabilities().clockInjected, false);
    CHECK_EQ(bare.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}}).raised, true);
    CHECK_EQ(bare.listAlerts().total, 1);
    CHECK_EQ(bare.acknowledge("a-1", "op").code, 0);
    // 能力自述如实区分注入状态
    const Capabilities caps = e.capabilities();
    CHECK_EQ(caps.storeInjected, true);
    CHECK_EQ(caps.clockInjected, true);
    CHECK_EQ(caps.sinkInjected, true);
    CHECK_EQ(caps.logInjected, true);
    CHECK_EQ(caps.pureMemory, false);
    CHECK_EQ(caps.engineVersion, std::string(kEngineVersion));
    CHECK_EQ(caps.ruleCount, 1);
    CHECK_EQ(caps.levelCount, 3);
}

/// ALT-NFR-02：时间可注入（`IClock`），使去重窗口与持续时间测试完全可复现
void nfr02_time_is_injectable() {
    requires_("ALT-NFR-02");
    // 持续时间：用观测时间戳（注入时钟推进）驱动 —— 真实等待 0 毫秒
    json item = ruleDef("d", "warn", durationCond(thresholdCond("m", "gt", 10), 3000),
                        json{{"dedupWindowMs", 0}});
    Env env(makePack(json::array({item})));
    CHECK_EQ(env.e().observe(env.obs("d", "e", 11, 1000, "m")).raised, false);
    // 只推进注入时钟（不 sleep）：持续时间按观测 ts 判定
    env.clock->advance(5000);
    CHECK_EQ(env.e().observe(env.obs("d", "e", 11, env.clock->nowMs(), "m")).raised, true);

    // 消音到期同样由注入时钟驱动：不 sleep 也能测"到期自动恢复"
    Env env2(makePack(json::array({ruleDef("r", "warn", thresholdCond("m", "gt", 1))}),
                      json(), json(), json{{"defaultMs", 1000}}));
    env2.clock->set(100000);
    CHECK_EQ(env2.e().mute(MuteRequest{MuteScope::Rule, "r"}).code, 0);
    CHECK_EQ(env2.e().observe(env2.obs("r", "e", 5, 101000, "m")).muted, true);
    env2.clock->set(101001);
    CHECK_EQ(env2.e().tick(0)["mutesExpired"], 1);
    CHECK_EQ(env2.e().observe(env2.obs("r", "e", 5, 101500, "m")).raised, true);

    // 台账保留期由注入时钟驱动（ALT-NFR-07 的可复现前提）
    {
        auto clock2 = std::make_shared<FakeClock>(1000000);
        AlertEngineOptions opts;
        opts.clock = clock2;
        opts.retentionMs = 5000;
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("k", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}, {"recoverOn", "close"}})}))).code, 0);
        CHECK_EQ(e.observe(Observation{"k", "e", "", "m", 5, 1000000, "", {}, {}}).raised, true);
        // 条件不再满足 → 按规则自动关闭（记录变 closed，才谈得上"保留期淘汰"）
        CHECK_EQ(e.observe(Observation{"k", "e", "", "m", 0, 1000001, "", {}, {}}).recovered, true);
        CHECK_EQ(e.listAlerts().total, 1);
        CHECK_EQ(e.tick(1002000)["evicted"], 0);  // 1999ms 未超过 5000ms 保留期 → 不淘汰
        CHECK_EQ(e.listAlerts().total, 1);
        CHECK_EQ(e.metrics().evicted, int64_t(0));
    }
    // 保留期边界：超过 retentionMs 才淘汰（上一条是"未超过"的对照）
    {
        auto clock2 = std::make_shared<FakeClock>(1000000);
        AlertEngineOptions opts;
        opts.clock = clock2;
        opts.retentionMs = 500;
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("k", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}, {"recoverOn", "close"}})}))).code, 0);
        e.observe(Observation{"k", "e", "", "m", 5, 1000000, "", {}, {}});
        e.observe(Observation{"k", "e", "", "m", 0, 1000001, "", {}, {}});
        clock2->set(1002000);  // 1999ms > 500ms
        CHECK_EQ(e.tick(0)["evicted"], 1);
        CHECK_EQ(e.metrics().evicted, int64_t(1));
    }

    // 时钟未注入 → 回落 SystemClock，且能力自述如实标注
    AlertEngine bare;
    CHECK_EQ(bare.capabilities().clockInjected, false);
    const json tk = bare.tick(0);
    CHECK(tk["now"].get<int64_t>() > 1600000000000LL);  // 真实墙钟（2020 之后）
}

/// ALT-NFR-03：确定性 —— 同输入序列同输出（含去重与合并）
void nfr03_determinism_byte_identical_double_run() {
    requires_("ALT-NFR-03");
    // 构造一段覆盖"产生 / 合并 / 升级 / 抑制 / 风暴 / 恢复 / 确认 / 消音"的序列
    auto runAll = [](const std::string& tag) {
        const json items = json::array({
            ruleDef("p", "error", thresholdCond("online", "lt", 1),
                    json{{"dedupWindowMs", 60000}, {"suppressChildren", true},
                         {"recoverOn", "close"}}),
            ruleDef("c", "warn", thresholdCond("m", "gt", 1),
                    json{{"dedupWindowMs", 5000}, {"parent", "p"}, {"confirmCount", 2}}),
            ruleDef("u", "warn", thresholdCond("m", "gt", 10),
                    json{{"dedupWindowMs", 5000}, {"recoverOn", "record"}}),
        });
        const json storm = json{{"enabled", true}, {"windowMs", 1000}, {"maxRaised", 5},
                                {"level", "error"}, {"title", "风暴 {folded}"}};
        AlertEngineOptions opts;
        opts.clock = std::make_shared<FakeClock>(100000);
        opts.sink = std::make_shared<SpySink>();
        opts.store = std::make_shared<MemStore>();
        opts.alertCountBasis = "deduplicated";
        AlertEngine e(opts);
        const LoadResult lr = e.loadRules(makePack(items, json(), storm, json{{"defaultMs", 500}}));
        if (lr.code != 0) return std::string("load-failed");
        auto obs = [](const std::string& r, const std::string& ent, double v, int64_t ts,
                      const std::string& metric) {
            Observation o;
            o.ruleId = r;
            o.entityId = ent;
            o.missionId = "m-1";
            o.metric = metric;
            o.value = v;
            o.ts = ts;
            return o;
        };
        std::string log;
        for (int i = 0; i < 20; ++i) {
            const AlertResult r = e.observe(obs("c", "d" + std::to_string(i % 4), 5, 1000 + i * 100, "m"));
            log += r.toJson().dump() + "\n";
        }
        log += e.observe(obs("p", "d0", 0, 4000, "online")).toJson().dump() + "\n";
        log += e.observe(obs("u", "d1", 20, 5000, "m")).toJson().dump() + "\n";
        log += e.observe(obs("u", "d1", 0, 6000, "m")).toJson().dump() + "\n";
        log += e.observe(obs("c", "d0", 5, 7000, "m")).toJson().dump() + "\n";
        log += e.acknowledge("a-1", "op", "r").toJson().dump() + "\n";
        log += e.close("a-1", "op", "r").toJson().dump() + "\n";
        log += e.reopen("a-1", "op", "r").toJson().dump() + "\n";
        log += e.mute(MuteRequest{MuteScope::Rule, "c", "", 0, 0, "op", "t"}).toJson().dump() + "\n";
        log += e.listAlerts(AlertQuery{}).toJson().dump() + "\n";
        log += e.counts().toJson().dump() + "\n";
        log += e.metrics().toJson().dump() + "\n";
        log += e.capabilities().toJson().dump() + "\n";
        log += e.tick(200000).dump() + "\n";
        (void)tag;
        return log;
    };
    const std::string first = runAll("A");
    const std::string second = runAll("B");
    if (first != second) {
        note("      确定性失败：两次运行输出长度 %zu / %zu\n", first.size(), second.size());
    }
    CHECK_EQ(first, second);  // 逐字节一致
    CHECK(first.size() > 1000);

    // 规则顺序无关性：同一规则集合、条目顺序不同 → 每条规则各自的结果一致
    auto runOrder = [](bool reverse) {
        std::vector<json> defs = {
            ruleDef("r1", "warn", thresholdCond("a", "gt", 1), json{{"dedupWindowMs", 1000}}),
            ruleDef("r2", "warn", thresholdCond("b", "gt", 1), json{{"dedupWindowMs", 1000}}),
            ruleDef("r3", "warn", thresholdCond("c", "gt", 1), json{{"dedupWindowMs", 1000}}),
        };
        if (reverse) std::reverse(defs.begin(), defs.end());
        json items = json::array();
        for (const auto& d : defs) items.push_back(d);
        Env env(makePack(items));
        env.e().observe(env.obs("r1", "e", 5, 1000, "a"));
        env.e().observe(env.obs("r2", "e", 5, 1000, "b"));
        env.e().observe(env.obs("r3", "e", 5, 1000, "c"));
        AlertQuery q;
        q.ruleIn = {"r2"};
        return env.e().listAlerts(q).items[0].toJson().dump();
    };
    CHECK_EQ(runOrder(false), runOrder(true));
}

/// ALT-NFR-04：线程安全 —— 多线程产生告警不丢不重
void nfr04_thread_safety_no_lost_or_duplicated() {
    requires_("ALT-NFR-04");
    // (a) 不同实体并发：每条各自成一条，计数精确
    {
        AlertEngineOptions opts;
        opts.clock = std::make_shared<FakeClock>(100000);
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}})}))).code, 0);
        constexpr int kThreads = 8;
        constexpr int kPerThread = 250;
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t) {
            th.emplace_back([&e, t, kPerThread]() {
                for (int i = 0; i < kPerThread; ++i) {
                    Observation o;
                    o.ruleId = "t";
                    o.entityId = "e" + std::to_string(t) + "-" + std::to_string(i);
                    o.metric = "m";
                    o.value = 5;
                    o.ts = 1000 + i;
                    e.observe(o);
                }
            });
        }
        for (auto& x : th) x.join();
        const int expected = kThreads * kPerThread;
        CHECK_EQ(static_cast<int>(e.listAlerts().total), expected);  // 不丢
        CHECK_EQ(e.counts().alertsRaised, static_cast<int64_t>(expected));
        CHECK_EQ(e.counts().rawRaises, static_cast<int64_t>(expected));
        CHECK_EQ(e.metrics().observations, static_cast<int64_t>(expected));
        // 不重：id 唯一
        std::set<std::string> ids;
        AlertQuery allQ;
    allQ.limit = 100000;  // 一次拉全（台账有 2000 条）
    for (const auto& r : e.listAlerts(allQ).items) ids.insert(r.alertId);
        CHECK_EQ(static_cast<int>(ids.size()), expected);
    }
    // (b) **同一实体并发**：窗口内合并 → 恰好 1 条，计数 == 观测数（不丢不重）
    {
        AlertEngineOptions opts;
        opts.clock = std::make_shared<FakeClock>(100000);
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 60000}})}))).code, 0);
        constexpr int kThreads = 8;
        constexpr int kPerThread = 200;
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t) {
            th.emplace_back([&e, t, kPerThread]() {
                for (int i = 0; i < kPerThread; ++i) {
                    Observation o;
                    o.ruleId = "t";
                    o.entityId = "same";
                    o.metric = "m";
                    o.value = 5;
                    o.ts = 1000 + t * 10 + i;  // 全在 60s 窗口内
                    e.observe(o);
                }
            });
        }
        for (auto& x : th) x.join();
        const int64_t expected = static_cast<int64_t>(kThreads) * kPerThread;
        CHECK_EQ(e.listAlerts().total, 1);
        CHECK_EQ(e.getAlert("a-1").value().count, expected);   // 不丢
        CHECK_EQ(e.counts().rawRaises, expected);
        CHECK_EQ(e.counts().alertsRaised, int64_t(1));          // 不重
        CHECK_EQ(e.getAlert("a-1").value().mergeHistory.size(), static_cast<std::size_t>(expected - 1));
    }
    // (c) 并发确认 / 关闭：状态最终一致，且幂等命中数如实
    {
        AlertEngine e;
        CHECK_EQ(e.loadRules(oneRulePack()).code, 0);
        CHECK_EQ(e.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}}).raised, true);
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t) {
            th.emplace_back([&e, t]() { e.acknowledge("a-1", "op" + std::to_string(t), ""); });
        }
        for (auto& x : th) x.join();
        CHECK_EQ(e.getAlert("a-1").value().state, AlertState::Acknowledged);
        CHECK_EQ(e.metrics().idempotentHits, int64_t(7));
        CHECK_EQ(e.metrics().sinkErrors, int64_t(0));
    }
}

/// ALT-NFR-05：独立交付 —— 独立构建 / 示例 / 测试 / 验收脚本（本用例即自检）
void nfr05_independent_delivery_smoke() {
    requires_("ALT-NFR-05");
    // 引擎在"空环境"（无规则包、无依赖）下即可构造并自述
    AlertEngine e;
    const Capabilities caps = e.capabilities();
    CHECK_EQ(caps.rulesLoaded, false);
    CHECK_EQ(caps.ruleCount, 0);
    CHECK_EQ(e.levels().empty(), true);
    CHECK_EQ(caps.levelCount, 0);
    // 未装载规则时的可读拒绝（不崩、不猜）
    const AlertResult r = e.observe(Observation{"any", "e", "", "m", 1, 1000, "", {}, {}});
    CHECK_EQ(r.code, 1004);
    CHECK(r.message.find("未知规则") != std::string::npos);
    const MuteResult m = e.mute(MuteRequest{MuteScope::Rule, "any"});
    CHECK_EQ(m.code, 1004);
    CHECK_EQ(e.tick(0)["alerts"], 0);
    CHECK_EQ(e.listAlerts().total, 0);
    // 规则包可以从**文件/目录**装载（便利方法，不依赖当前工作目录的值由 CMake 注入）
    AlertEngine e2;
    const LoadResult lr = e2.loadRulesFromDirectory(ALERT_ENGINE_POLICY_DIR);
    if (lr.code != 0) note("      %s\n", lr.toJson().dump().c_str());
    CHECK_EQ(lr.code, 0);
    CHECK_EQ(lr.data.policiesNamespace, std::string("mapapp"));
    CHECK_EQ(e2.capabilities().rulesLoaded, true);
    CHECK(e2.rulesInfo().ruleCount >= 5);
    // 读不到的文件 → 1000 + 可读原因（不抛异常）
    const LoadResult bad = e2.loadRulesFile("this-file-does-not-exist.json");
    CHECK_EQ(bad.code, 1000);
    CHECK(!bad.issues.empty());
    // 非法 JSON → 1000 + 可读原因
    const LoadResult bad2 = e2.loadRules(json::array());
    CHECK_EQ(bad2.code, 1000);
}

/// ALT-NFR-06：性能 —— 1000 条/秒产生压力下去重与分发 P95 ≤ 1 ms
void nfr06_performance_p95_under_1ms() {
    requires_("ALT-NFR-06");
    AlertEngineOptions opts;
    opts.clock = std::make_shared<FakeClock>(100000);
    opts.sink = std::make_shared<SpySink>();
    AlertEngine e(opts);
    CHECK_EQ(e.loadRules(makePack(json::array(
                 {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                          json{{"dedupWindowMs", 1000}})}))).code, 0);
    // 1000 条/秒的节拍：每条间隔 1ms，10 个实体轮转 → 窗口内反复合并（最坏路径之一）
    constexpr int kN = 1000;
    std::vector<double> us;
    us.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        Observation o;
        o.ruleId = "t";
        o.entityId = "e" + std::to_string(i % 10);
        o.metric = "m";
        o.value = 5;
        o.ts = 100000 + i;
        const auto t0 = std::chrono::steady_clock::now();
        e.observe(o);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(us.begin(), us.end());
    const double p50 = us[us.size() * 50 / 100];
    const double p95 = us[us.size() * 95 / 100];
    const double p99 = us[us.size() * 99 / 100];
    const double totalMs = std::accumulate(us.begin(), us.end(), 0.0) / 1000.0;
    note("      性能实测：1000 条（1000 条/秒节拍）去重+分发 P50=%.1fµs P95=%.1fµs P99=%.1fµs"
         " 合计 %.1fms\n", p50, p95, p99, totalMs);
    CHECK_MSG(p95 <= 1000.0, "P95 MUST ≤ 1 ms（1000 µs）");
    CHECK_MSG(totalMs <= 1000.0, "1000 条总耗时应 ≤ 1 秒（吞吐 ≥ 1000 条/秒）");
    CHECK_EQ(e.metrics().observations, int64_t(kN));
    CHECK_EQ(e.counts().rawRaises, int64_t(kN));
}

/// ALT-NFR-07：台账有界 —— 活动告警上限与历史保留期可配
void nfr07_ledger_is_bounded() {
    requires_("ALT-NFR-07");
    // (a) 活动上限：淘汰最旧的，并**如实计入** evicted
    //
    // 注意：上限是**显式策略**，且清扫是节流的（默认 1s 一次，避免千条/秒下的全表扫描）。
    // 因此下面按 1s 以上的间隔推进时钟，让有界策略在压力下也**逐次**生效。
    {
        AlertEngineOptions opts;
        auto clock = std::make_shared<FakeClock>(100000);
        opts.clock = clock;
        opts.maxActiveAlerts = 2;
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}})}))).code, 0);
        for (int i = 0; i < 10; ++i) {
            clock->set(100000 + i * 2000);
            const AlertResult r = e.observe(
                Observation{"t", "e" + std::to_string(i), "", "m", 5, 100000 + i * 2000, "", {}, {}});
            CHECK_EQ(r.raised, true);
        }
        // 上限是**逐次**生效的（每次观测后清扫），因此条数受控、淘汰数如实
        CHECK_EQ(e.counts().openActive, int64_t(3));   // 上限 2 → 上限 + 本次新增
        CHECK_EQ(e.listAlerts().total, 3);
        CHECK_EQ(e.metrics().evicted, int64_t(7));
        CHECK_EQ(e.capabilities().maxActiveAlerts, 2);
        CHECK_EQ(e.getAlert("a-1").has_value(), false);
        CHECK_EQ(e.getAlert("a-10").has_value(), true);
    }
    // (b) 历史保留期：关闭且超期的记录被淘汰，未超期保留
    {
        auto clock2 = std::make_shared<FakeClock>(1000000);
        AlertEngineOptions opts;
        opts.clock = clock2;
        opts.retentionMs = 500;
        AlertEngine e(opts);
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}, {"recoverOn", "close"}})}))).code, 0);
        e.observe(Observation{"t", "e1", "", "m", 5, 1000000, "", {}, {}});
        e.observe(Observation{"t", "e1", "", "m", 0, 1000001, "", {}, {}});  // 关闭
        e.observe(Observation{"t", "e2", "", "m", 5, 1000002, "", {}, {}});  // 仍活动
        clock2->set(1002000);
        CHECK_EQ(e.tick(0)["evicted"], 1);
        CHECK_EQ(e.listAlerts().total, 1);  // 只留活动那条
        CHECK_EQ(e.metrics().evicted, int64_t(1));
        CHECK_EQ(e.capabilities().retentionMs, int64_t(500));
    }
    // (c) 默认不限：不配即不淘汰（有界是**显式**策略，不是隐藏行为）
    {
        AlertEngine e;
        CHECK_EQ(e.loadRules(makePack(json::array(
                     {ruleDef("t", "warn", thresholdCond("m", "gt", 1),
                              json{{"dedupWindowMs", 0}})}))).code, 0);
        for (int i = 0; i < 50; ++i) {
            e.observe(Observation{"t", "e" + std::to_string(i), "", "m", 5, 1000 + i, "", {}, {}});
        }
        CHECK_EQ(e.listAlerts().total, 50);
        CHECK_EQ(e.metrics().evicted, int64_t(0));
        CHECK_EQ(e.capabilities().maxActiveAlerts, 0);
    }
}

// ############################################################################
// 共享契约 protocol.md 的可机检清单
// ############################################################################

/// §3.2 错误码：只取码表取值；1001 保留不用（C15）
void proto_error_codes_are_from_the_frozen_table() {
    requires_("ALT-RULE-03", "ALT-ACK-01");
    // 码的短名稳定，未知码 → "unknown"（MUST NOT 猜）
    CHECK_EQ(std::string(errorCodeName(0)), std::string("ok"));
    CHECK_EQ(std::string(errorCodeName(1000)), std::string("bad-request"));
    CHECK_EQ(std::string(errorCodeName(1002)), std::string("conflict"));
    CHECK_EQ(std::string(errorCodeName(1003)), std::string("unmet"));
    CHECK_EQ(std::string(errorCodeName(1004)), std::string("not-found"));
    CHECK_EQ(std::string(errorCodeName(1005)), std::string("internal"));
    CHECK_EQ(std::string(errorCodeName(1006)), std::string("version-mismatch"));
    CHECK_EQ(std::string(errorCodeName(1001)), std::string("unknown"));  // 保留不用
    CHECK_EQ(std::string(errorCodeName(42)), std::string("unknown"));
    CHECK_EQ(std::string(AlertEngine::errorCodeName(1003)), std::string("unmet"));

    // 引擎实际产生的码全部落在码表内
    std::set<int> seen;
    Env env(oneRulePack());
    seen.insert(env.e().observe(env.obs("r", "e", 5, 1000, "m")).code);          // 0
    seen.insert(env.e().observe(env.obs("", "e", 5, 1000, "m")).code);           // 1000
    seen.insert(env.e().observe(env.obs("ghost", "e", 5, 1000, "m")).code);      // 1004
    seen.insert(env.e().close("a-1", "op", "").code);                            // 0
    seen.insert(env.e().acknowledge("a-1", "op", "").code);                      // 1003（已关闭）
    seen.insert(validateRules(json::array()).code);                              // 1000
    json pkg = oneRulePack();
    pkg["schemaVersion"] = "5.0.0";
    seen.insert(validateRules(pkg).code);                                        // 1006
    for (int code : seen) {
        CHECK_MSG(code == 0 || code == 1000 || code == 1002 || code == 1003 || code == 1004 ||
                      code == 1005 || code == 1006,
                  "码不在 protocol §3.2 冻结表内：" + std::to_string(code));
        CHECK_MSG(code != 1001, "1001 保留不用（C15）");
    }
    // 信封形状恰为 {code, message, data}
    const json j = env.e().observe(env.obs("r", "e2", 5, 2000, "m")).toJson();
    CHECK_EQ(j.size(), std::size_t(3));
    CHECK(j.contains("code") && j.contains("message") && j.contains("data"));
    CHECK(j["data"].is_object());
}

/// §5 规则包：schemaVersion 版本语义 + kind + 清单段（CTR-PL-01..06）
void proto_policies_schema_and_versioning() {
    requires_("ALT-RULE-01", "ALT-RULE-03");
    const json base = makePack(json::array({ruleDef("a", "warn", thresholdCond("m", "gt", 1))}));
    CHECK_EQ(validateRules(base).code, 0);
    // 缺骨架字段
    for (const char* f : {"policiesNamespace", "schemaVersion", "kind"}) {
        json p = base;
        p.erase(f);
        const LoadResult r = validateRules(p);
        CHECK_MSG(r.code == 1000, std::string("缺 ") + f + " MUST 拒绝");
        CHECK(!r.issues.empty());
    }
    // 缺清单段 items / levels
    json noItems = base;
    noItems.erase("items");
    CHECK_EQ(validateRules(noItems).code, 1000);
    json noLevels = base;
    noLevels.erase("levels");
    CHECK_EQ(validateRules(noLevels).code, 1000);
    // 版本语义：MAJOR → 1006；MINOR/PATCH → 正常装载
    json major = base;
    major["schemaVersion"] = "2.0.0";
    CHECK_EQ(validateRules(major).code, 1006);
    json minor = base;
    minor["schemaVersion"] = "1.5.0";
    CHECK_EQ(validateRules(minor).code, 0);
    json patch = base;
    patch["schemaVersion"] = "1.0.7";
    CHECK_EQ(validateRules(patch).code, 0);
    // 形状非法
    json badVer = base;
    badVer["schemaVersion"] = "v1";
    CHECK_EQ(validateRules(badVer).code, 1000);
    json badNs = base;
    badNs["policiesNamespace"] = "Bad_NS";
    CHECK_EQ(validateRules(badNs).code, 1000);
    // 校验失败 MUST 返回**全部**问题（不短路 —— CTR-PL-02）
    json many = base;
    many["items"] = json::array({ruleDef("a", "warn", thresholdCond("m", "gt", 1)),
                                 ruleDef("b", "warn", thresholdCond("m", "gt", 1))});
    many["items"][0].erase("level");
    many["items"][1].erase("condition");
    many.erase("levels");
    const LoadResult all = validateRules(many);
    CHECK_EQ(all.code, 1000);
    CHECK_MSG(all.issues.size() >= 3, "一次 MUST 返回全部问题（含条目下标与字段名）");
    std::set<std::string> paths;
    for (const auto& i : all.issues) paths.insert(i.path);
    CHECK(paths.count("items[0]") != 0);
    CHECK(paths.count("items[1]") != 0);
    // 未装载时的引擎仍可自述（永远可调用）
    AlertEngine bare;
    CHECK_EQ(bare.rulesInfo().loaded, false);
    CHECK_EQ(bare.levels().size(), std::size_t(0));
    CHECK_EQ(bare.levelRank("warn").has_value(), false);
}

/// §3.3 / C15 / C16 / C17：幂等成功与冲突拒绝必须分开表达
void proto_idempotent_vs_conflict_are_separate() {
    requires_("ALT-ACK-02");
    Env env(oneRulePack());
    AlertEngine& e = env.e();
    const std::string id = raiseOne(env);
    CHECK_EQ(e.acknowledge(id, "op", "").code, 0);
    // 幂等命中 MUST NOT 发明非零 code；MUST NOT 报错（C15 / CTR-EC-01/02）
    const ActionResult again = e.acknowledge(id, "op", "");
    CHECK_EQ(again.code, 0);
    CHECK_EQ(again.idempotent, true);
    CHECK_EQ(again.conflict, false);
    CHECK_EQ(again.toJson()["data"]["idempotent"], true);
    CHECK_EQ(again.toJson()["data"]["conflict"], false);
    // 1002 只用于互斥冲突：本引擎的同步原子路径不产生它；若产生则必然 conflict=true
    CHECK(again.code != 1002);
    // 1001 保留不用：引擎任何返回值都不含 1001
    for (int code : {e.observe(env.obs("r", "z", 5, 3000, "m")).code,
                     e.observe(env.obs("", "z", 5, 3000, "m")).code,
                     e.observe(env.obs("nope", "z", 5, 3000, "m")).code,
                     e.listAlerts().code,
                     e.mute(MuteRequest{}).code}) {
        CHECK(code != 1001);
    }
}

/// §6 反向接口命名与形状：IAlertStore / IAlertSink / IClock / ILogSink（+ PhaseContext 镜像）
void proto_reverse_interfaces_naming() {
    requires_("ALT-SUB-04", "ALT-NFR-01");
    // 三个 MUST 注入项 + 1 个可选，全部经 AlertEngineOptions 或 setXxx 注入；
    // 编译期即证明签名与契约 §6 一致（下面每行都是一次真实调用）。
    AlertEngineOptions opts;
    auto store = std::make_shared<MemStore>();
    auto clock = std::make_shared<FakeClock>();
    auto sink = std::make_shared<SpySink>();
    auto log = std::make_shared<SpyLog>();
    AlertEngine e;
    e.setStore(store);
    e.setClock(clock);
    e.setSink(sink);
    e.setLog(log);
    CHECK_EQ(e.capabilities().storeInjected, true);
    CHECK_EQ(e.capabilities().clockInjected, true);
    CHECK_EQ(e.capabilities().sinkInjected, true);
    CHECK_EQ(e.capabilities().logInjected, true);
    CHECK_EQ(e.loadRules(oneRulePack()).code, 0);
    CHECK_EQ(e.observe(Observation{"r", "e", "", "m", 5, 1000, "", {}, {}}).raised, true);
    CHECK_EQ(sink->raisedCount(), std::size_t(1));
    CHECK(store->saves >= 1);
    // IAlertStore 的 load 是"完整记录"语义（引擎不提供增量字段 —— CTR-PHE-ST-02 同口径）
    AlertRecord out;
    CHECK_EQ(store->load("a-1", out), true);
    CHECK_EQ(out.alertId, std::string("a-1"));
    CHECK_EQ(out.stateHistory.size(), std::size_t(1));
    // setXxx(nullptr) → 回落缺省实现，引擎仍可工作
    e.setStore(nullptr);
    e.setSink(nullptr);
    e.setClock(nullptr);
    CHECK_EQ(e.capabilities().pureMemory, true);
    CHECK_EQ(e.observe(Observation{"r", "e2", "", "m", 5, 2000, "", {}, {}}).raised, true);
    // 可空注入也可经构造函数一次给出
    AlertEngine e2(opts);
    CHECK_EQ(e2.capabilities().pureMemory, true);
    CHECK_EQ(e2.capabilities().clockInjected, false);
    CHECK(e2.tick(0)["now"].get<int64_t>() > 0);
}

/// §4.4 事件负载形状与命名规范（CTR-EV-01/02/05 + 只增不改）
void proto_event_payload_shapes() {
    requires_("ALT-SUB-06");
    // 引擎产出的三个事件名符合 `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)+$` 且不占 sys./up.
    const char* names[] = {"alert.raised", "alert.updated", "alert.acked"};
    for (const char* n : names) {
        const std::string s = n;
        CHECK(s.find('.') != std::string::npos);
        CHECK(s.rfind("sys.", 0) != 0);
        CHECK(s.rfind("up.", 0) != 0);
        for (char ch : s) {
            CHECK_MSG((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.',
                      std::string("事件名只允许小写与点分层：") + s);
        }
    }
    // 三个负载结构可直接构造并序列化为 JSON 对象（宿主包信封用）
    AlertRaisedEvent r;
    r.alertId = "a-1";
    r.ruleId = "r";
    r.level = "warn";
    r.entityId = "e";
    r.missionId = "m";
    r.firstAt = 1000;
    r.count = 3;
    r.ts = 1000;
    const json jr = toJson(r);
    CHECK(jr.is_object());
    for (const char* k : {"alertId", "ruleId", "level", "entityId", "missionId", "firstAt",
                          "count", "ts"}) {
        CHECK_MSG(jr.contains(k), std::string("alert.raised 缺字段 ") + k);
    }
    AlertUpdatedEvent u;
    u.alertId = "a-1";
    u.count = 3;
    u.lastAt = 2000;
    u.level = "warn";
    u.ts = 2000;
    const json ju = toJson(u);
    for (const char* k : {"alertId", "count", "lastAt", "level", "upgraded", "ts"}) {
        CHECK_MSG(ju.contains(k), std::string("alert.updated 缺字段 ") + k);
    }
    AlertAckedEvent a;
    a.alertId = "a-1";
    a.state = "closed";
    a.operatorId = "op";
    a.at = 3000;
    const json ja = toJson(a);
    for (const char* k : {"alertId", "state", "operatorId", "at"}) {
        CHECK_MSG(ja.contains(k), std::string("alert.acked 缺字段 ") + k);
    }
    // AlertDelivery 形状（宿主据此知道推给了谁）
    AlertDelivery d;
    d.event = "alert.raised";
    d.payload = jr;
    d.subscriberIds = {"s1", "s2"};
    d.ts = 1000;
    const json jd = toJson(d);
    CHECK_EQ(jd["event"], std::string("alert.raised"));
    CHECK_EQ(jd["subscriberCount"], 2);
    CHECK_EQ(jd["broadcast"], false);
    CHECK_EQ(toString(AlertState::Acknowledged), std::string("acknowledged"));
    CHECK_EQ(alertStateFromString("closed").value(), AlertState::Closed);
    CHECK_EQ(alertStateFromString("nope").has_value(), false);
    CHECK_EQ(toString(MergeMode::Accumulate), std::string("accumulate"));
    CHECK_EQ(toString(RecoveryMode::Close), std::string("close"));
    CHECK_EQ(toString(MuteScope::Entity), std::string("entity"));
    CHECK_EQ(toString(ConditionType::Duration), std::string("duration"));
    CHECK_EQ(toString(CompareOp::Missing), std::string("missing"));
    CHECK_EQ(toString(ResultStatus::AlreadyApplied), std::string("already-applied"));
}

// ############################################################################
// 联合口径（跨需求的一致性锁定）
// ############################################################################

/// 真实规则包端到端：装载 → 阈值/持续/恢复/抑制 → 台账筛选 → 订阅 → 计数口径
void join_real_policy_pack_end_to_end() {
    requires_("ALT-RULE-01", "ALT-GEN-03", "ALT-DEDUP-05", "ALT-SUB-01", "ALT-SUB-03");
    Env env(loadPackFile());
    AlertEngine& e = env.e();
    env.clock->set(1750000000000LL);
    CHECK_EQ(e.rulesInfo().ruleCount, 9);
    CHECK_EQ(e.levels().size(), std::size_t(3));
    // node-offline 声明 suppressChildren=true → 抑制其余全部规则（8 条边）
    CHECK_EQ(e.rulesInfo().suppressionEdges, 8);

    // 订阅：只看 error 级别
    AlertSubscription sub;
    sub.subscriberId = "ops-error";
    sub.levels = {"error"};
    CHECK_EQ(e.subscribe(sub).code, 0);

    // 节点失联（持续 3 秒）：2 秒不报，3 秒报
    Observation offline;
    offline.ruleId = "node-offline";
    offline.entityId = "node-1";
    offline.missionId = "mission-1";
    offline.metric = "online";
    offline.value = 0;
    offline.ts = 1000;
    CHECK_EQ(e.observe(offline).raised, false);
    offline.ts = 4000;
    const AlertResult off = e.observe(offline);
    CHECK_EQ(off.raised, true);
    CHECK_EQ(off.alert.level, std::string("error"));
    CHECK_EQ(off.alert.missionId, std::string("mission-1"));
    CHECK(off.alert.title.find("node-1") != std::string::npos);  // 模板填充（规则包里的文案）
    // 订阅过滤：error 级 → 命中
    CHECK_EQ(env.sink->raisedCopy().back().subscribers.size(), std::size_t(1));

    // 链路质量差：被父告警抑制（同一实体）。
    // 注意 link-quality 声明了 `confirmCount=2`（抖动抑制），所以第 1 次观测被**确认计数**
    // 拦下、第 2 次才走到抑制判定 —— 两条机制的顺序是"确认 → 抑制"，此处正好把顺序钉住。
    Observation link = offline;
    link.ruleId = "link-quality";
    link.metric = "signal";
    link.value = 0.2;
    link.ts = 5000;
    const AlertResult conf1 = e.observe(link);
    CHECK_EQ(conf1.raised, false);
    CHECK_EQ(conf1.durationPending, true);   // 第 1 次：确认计数未满（不是被抑制）
    link.ts = 5600;                          // 同时满足 confirmCount=2 与 minDwellMs=500
    const AlertResult sup = e.observe(link); // 第 2 次
    CHECK_EQ(sup.suppressed, true);          // 第 2 次：确认已满 → 被父告警抑制
    CHECK_EQ(sup.raised, false);
    CHECK(sup.suppressReason.find("规则 node-offline") != std::string::npos);
    CHECK_EQ(env.sink->raisedCount(), std::size_t(1));  // 仍只有节点失联那一条推给订阅者

    // 失联恢复 → 自动关闭 + 恢复记录（规则包声明 recoverOn=record）
    offline.ts = 6000;
    offline.value = 1;
    const AlertResult rec = e.observe(offline);
    CHECK_EQ(rec.recovered, true);
    CHECK_EQ(rec.alert.kind, std::string("recovery"));
    CHECK_EQ(rec.alert.linkedAlertId, off.alert.alertId);

    // 父告警关闭后，子告警恢复产生。
    // 注意抖动抑制是**按（规则+实体）连续计数**的：5600 那次虽然被抑制、但条件确实成立，
    // 所以连续计数不归零；到 7000 已满足 confirmCount=2 → 正常产生。
    link.ts = 7000;
    const AlertResult child = e.observe(link);
    CHECK_EQ(child.raised, true);
    CHECK_EQ(child.alert.parentRuleId, std::string(""));  // 产生时父已关闭 → 无抑制关系留痕
    // 8000 仍在 10s 去重窗口内 → 归并（窗口内合并，不是新开）
    link.ts = 8000;
    const AlertResult merged = e.observe(link);
    CHECK_EQ(merged.merged, true);
    CHECK_EQ(e.counts().merged, int64_t(1));  // 一次窗口内合并（link-quality 的 merge=latest：计数不累加）
    // 原始条数 5 = 新开 2（node-offline + link-quality）+ 恢复 1 + 被抑制 1 + 合并 1
    CHECK_EQ(e.counts().rawRaises, int64_t(5));
    CHECK_EQ(e.counts().suppressed, int64_t(1));

    // 台账筛选与计数口径
    AlertQuery q;
    q.missionIn = {"mission-1"};
    CHECK_EQ(e.listAlerts(q).total, 3);
    q.levelIn = {"error"};
    CHECK_EQ(e.listAlerts(q).total, 2);
    q = AlertQuery{};
    q.kindIn = {"recovery"};
    CHECK_EQ(e.listAlerts(q).total, 1);
    const AlertCounts n = e.counts();
    // 计数口径恒等式（ALT-GEN-05 的可复算口径）：
    //   原始条数 = 新开 + 恢复 + 合并 + 折叠 + 被抑制
    // （被抑制的触发同样是"条件满足的一次触发"，必须计入原始条数，否则口径会少算）
    CHECK_EQ(n.rawRaises, n.alertsRaised + n.recoveries + n.merged + n.folded + n.suppressed);
    CHECK(n.alertCount >= 3);
    CHECK_EQ(n.basis, std::string("deduplicated"));

    // 并发压测一遍（真实规则包下不丢不重）
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t) {
        th.emplace_back([&e, t]() {
            for (int i = 0; i < 100; ++i) {
                Observation o;
                o.ruleId = "progress-lag";
                o.entityId = "m" + std::to_string(t) + "-" + std::to_string(i);
                o.missionId = "mission-1";
                o.metric = "ratePerMin";
                o.value = 0.1;
                o.ts = 20000 + i;
                e.observe(o);
            }
        });
    }
    for (auto& x : th) x.join();
    CHECK_EQ(e.metrics().observations, int64_t(7 + 100 * 4));
    // 并发压测段的时间戳都落在同一秒内（20000..20099），而规则包声明了风暴保护
    // （1000ms 内最多 50 条新开）→ 前 50 条照常产生、其余被折叠进汇总告警。
    // 这里正好把"风暴保护在高压下自动接管"钉住（ALT-DEDUP-06 的实战形态）。
    CHECK_EQ(e.counts().alertsRaised, int64_t(52));  // 2（前段）+ 50（上限内）
    CHECK_EQ(e.counts().recoveries, int64_t(1));
    CHECK_EQ(e.counts().merged, int64_t(1));         // 只有 8000 那一次是窗口内合并
    CHECK_EQ(e.counts().aggregates, int64_t(1));     // **同一个风暴窗口内只开一条汇总**（后续折叠并入它）
    CHECK_EQ(e.counts().folded, int64_t(350));       // 400 - 50 条被折叠，如实上报
    CHECK_EQ(e.counts().rawRaises, int64_t(405));    // n.rawRaises(5) + 400 次并发触发，一次不少
    CHECK_EQ(e.counts().rawRaises, e.counts().alertsRaised + e.counts().recoveries +
                                       e.counts().merged + e.counts().folded +
                                       e.counts().suppressed);
    // 汇总告警里能读出被折叠条数（报告与界面都无需自己算）
    AlertQuery aggQ;
    aggQ.kindIn = {"aggregate"};
    const AlertQueryResult aggs = e.listAlerts(aggQ);
    CHECK_EQ(aggs.total, 1);
    CHECK_EQ(aggs.items[0].foldedCount, int64_t(350));
    CHECK_EQ(aggs.items[0].ruleId, std::string("$storm"));
    // 台账总条数 = 前段 2（node-offline + link-quality）+ 恢复 1 + 风暴段 50（上限内）+ 汇总 1 = 54
    CHECK_EQ(e.listAlerts().total, 54);
}

/// 订阅匹配的纯函数口径（顺序无关、空集合 = 全部）
void join_subscription_filter_is_order_independent() {
    requires_("ALT-SUB-03");
    Env env(makePack(json::array(
        {ruleDef("s", "warn", thresholdCond("m", "gt", 1), json{{"dedupWindowMs", 0}})})));
    AlertSubscription a;
    a.subscriberId = "zzz";
    a.levels = {"warn", "error"};
    AlertSubscription b;
    b.subscriberId = "aaa";
    b.entityIds = {"e"};
    CHECK_EQ(env.e().subscribe(a).code, 0);
    CHECK_EQ(env.e().subscribe(b).code, 0);
    env.e().observe(env.obs("s", "e", 5, 1000, "m"));
    const auto raised = env.sink->raisedCopy();
    CHECK_EQ(raised.size(), std::size_t(1));
    // 命中的订阅者按注册顺序（有序 map），与注册调用的先后无关地稳定
    CHECK_EQ(raised[0].subscribers.size(), std::size_t(2));
    CHECK_EQ(raised[0].subscribers[0], std::string("aaa"));
    CHECK_EQ(raised[0].subscribers[1], std::string("zzz"));
    // 只关心规则集合的订阅者：不匹配时不推
    AlertEngine e2;
    CHECK_EQ(e2.loadRules(makePack(json::array(
                 {ruleDef("s", "warn", thresholdCond("m", "gt", 1))}))).code, 0);
    auto sink2 = std::make_shared<SpySink>();
    e2.setSink(sink2);
    AlertSubscription c;
    c.subscriberId = "only-other-rule";
    c.ruleIds = {"another"};
    CHECK_EQ(e2.subscribe(c).code, 0);
    e2.observe(Observation{"s", "e", "", "m", 5, 1000, "", {}, {}});
    CHECK_EQ(sink2->raisedCopy()[0].subscribers.size(), std::size_t(0));
    CHECK_EQ(sink2->raisedCopy()[0].broadcast, true);
}

/// 边界与鲁棒性：空值、未知字段、超界、重复装载
void join_edge_cases() {
    requires_("ALT-RULE-03", "ALT-SUB-01");
    // 空规则包 / 非对象 / 空数组
    CHECK_EQ(validateRules(json()).code, 1000);
    CHECK_EQ(validateRules(json::array()).code, 1000);
    CHECK_EQ(validateRules(json::object()).code, 1000);
    // 空 items
    json empty = makePack(json::array());
    CHECK_EQ(validateRules(empty).code, 1000);
    // 观测 JSON 未知字段被忽略（前向兼容）
    Env env(oneRulePack());
    json o = json::object();
    o["ruleId"] = "r";
    o["entityId"] = "e";
    o["metric"] = "m";
    o["value"] = 5;
    o["ts"] = 1000;
    o["futureField"] = json::object();
    o["another"] = 12;
    CHECK_EQ(env.e().observe(o).raised, true);
    // 缺 ts → 0（不崩；由宿主负责给时间戳）
    json o2 = o;
    o2.erase("ts");
    CHECK_EQ(env.e().observe(o2).code, 0);
    // limit = 0 的查询：返回 0 条但 total 如实
    AlertQuery q;
    q.limit = 0;
    const AlertQueryResult r = env.e().listAlerts(q);
    CHECK_EQ(r.total, 2);
    CHECK_EQ(r.returned, 0);
    CHECK_EQ(r.truncated, true);
    CHECK_EQ(r.omitted, 2);
    // offset 超出范围：不崩、如实
    q.limit = 10;
    q.offset = 99;
    const AlertQueryResult r2 = env.e().listAlerts(q);
    CHECK_EQ(r2.returned, 0);
    CHECK_EQ(r2.total, 2);
    CHECK_EQ(r2.omitted, 0);
    // 重复装载同一份规则包：幂等（内容一致）
    CHECK_EQ(env.e().loadRules(oneRulePack()).code, 0);
    CHECK_EQ(env.e().rulesInfo().ruleCount, 1);
}

// ############################################################################
// 用例注册表
// ############################################################################

namespace {

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    // ---- ALT-RULE 告警规则（5）----
    {"rule01_rules_come_from_data_only", rule01_rules_come_from_data_only},
    {"rule02_four_condition_kinds", rule02_four_condition_kinds},
    {"rule03_load_validation_reports_location", rule03_load_validation_reports_location},
    {"rule04_enable_disable_at_runtime", rule04_enable_disable_at_runtime},
    {"rule05_alert_carries_rule_version", rule05_alert_carries_rule_version},
    // ---- ALT-GEN 产生与分级（6）----
    {"gen01_structured_input_only", gen01_structured_input_only},
    {"gen02_levels_are_extensible", gen02_levels_are_extensible},
    {"gen03_dedup_anchor_merges_and_counts", gen03_dedup_anchor_merges_and_counts},
    {"gen04_recovery_semantics", gen04_recovery_semantics},
    {"gen05_count_basis_is_explicit", gen05_count_basis_is_explicit},
    {"gen06_first_and_last_are_separate", gen06_first_and_last_are_separate},
    // ---- ALT-DEDUP 去重、合并与抑制（6）----
    {"dedup01_window_configurable_and_crossing_opens_new",
     dedup01_window_configurable_and_crossing_opens_new},
    {"dedup02_merge_field_contract_and_merge_modes",
     dedup02_merge_field_contract_and_merge_modes},
    {"dedup03_flapping_suppression", dedup03_flapping_suppression},
    {"dedup04_upgrade_records_and_keeps_history", dedup04_upgrade_records_and_keeps_history},
    {"dedup05_parent_suppresses_child_until_closed",
     dedup05_parent_suppresses_child_until_closed},
    {"dedup06_storm_protection_reports_folded_count",
     dedup06_storm_protection_reports_folded_count},
    // ---- ALT-ACK 确认与生命周期（5）----
    {"ack01_state_machine_and_illegal_transitions", ack01_state_machine_and_illegal_transitions},
    {"ack02_acknowledge_is_idempotent", ack02_acknowledge_is_idempotent},
    {"ack03_audit_trail_is_recoverable", ack03_audit_trail_is_recoverable},
    {"ack04_mute_is_time_limited_and_auto_recovers",
     ack04_mute_is_time_limited_and_auto_recovers},
    {"ack05_auto_close_on_recovery_is_traced", ack05_auto_close_on_recovery_is_traced},
    // ---- ALT-SUB 台账与订阅（6）----
    {"sub01_ledger_query_filters_and_truncation", sub01_ledger_query_filters_and_truncation},
    {"sub02_alerts_belong_to_mission", sub02_alerts_belong_to_mission},
    {"sub03_subscription_filtering", sub03_subscription_filtering},
    {"sub04_sink_is_the_only_output", sub04_sink_is_the_only_output},
    {"sub05_batch_active_alerts", sub05_batch_active_alerts},
    {"sub06_event_payload_shape_and_compat", sub06_event_payload_shape_and_compat},
    // ---- ALT-NFR 非功能性（7）----
    {"nfr01_zero_external_dependencies", nfr01_zero_external_dependencies},
    {"nfr02_time_is_injectable", nfr02_time_is_injectable},
    {"nfr03_determinism_byte_identical_double_run", nfr03_determinism_byte_identical_double_run},
    {"nfr04_thread_safety_no_lost_or_duplicated", nfr04_thread_safety_no_lost_or_duplicated},
    {"nfr05_independent_delivery_smoke", nfr05_independent_delivery_smoke},
    {"nfr06_performance_p95_under_1ms", nfr06_performance_p95_under_1ms},
    {"nfr07_ledger_is_bounded", nfr07_ledger_is_bounded},
    // ---- 共享契约 protocol.md ----
    {"proto_error_codes_are_from_the_frozen_table",
     proto_error_codes_are_from_the_frozen_table},
    {"proto_policies_schema_and_versioning", proto_policies_schema_and_versioning},
    {"proto_idempotent_vs_conflict_are_separate", proto_idempotent_vs_conflict_are_separate},
    {"proto_reverse_interfaces_naming", proto_reverse_interfaces_naming},
    {"proto_event_payload_shapes", proto_event_payload_shapes},
    // ---- 联合口径 ----
    {"join_real_policy_pack_end_to_end", join_real_policy_pack_end_to_end},
    {"join_subscription_filter_is_order_independent",
     join_subscription_filter_is_order_independent},
    {"join_edge_cases", join_edge_cases},
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // 控制台切 UTF-8，否则中文用例名会乱码。
    // **只在真有控制台时切**：stdout 被重定向（验收脚本 / CI / ctest 抓输出）时
    // `chcp` 会以 EBUSY 失败并让 std::system 抛异常 —— 那是环境噪声，不是测试失败。
    if (argc < 2) {
        try {
            std::system("chcp 65001 > nul");
        } catch (...) {
        }
    }
#endif
    std::string filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--json") {
            g_jsonMode = true;
        } else {
            filter = arg;
        }
    }
    if (listOnly) {
        for (const auto& c : kCases) std::printf("%s\n", c.name);
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : kCases) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        g_case = c.name;
        g_reqs.clear();
        const int failedBefore = g_failed;
        const int assertsBefore = g_asserts;
        ++g_cases;
        try {
            c.fn();
        } catch (const std::exception& ex) {
            record(false, std::string("用例抛出异常：") + ex.what(), __FILE__, __LINE__);
        } catch (...) {
            record(false, "用例抛出未知异常", __FILE__, __LINE__);
        }
        const bool ok = (g_failed == failedBefore);
        if (!ok) ++g_casesFailed;
        CaseMeta meta;
        meta.name = c.name;
        meta.reqs = g_reqs;
        meta.asserts = g_asserts - assertsBefore;
        meta.failed = g_failed - failedBefore;
        meta.ok = ok;
        g_metas.push_back(std::move(meta));
        if (!g_jsonMode) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
            for (int fi = failedBefore; fi < g_failed; ++fi) {
                std::printf("       ! %s\n", g_failures[static_cast<std::size_t>(fi)].c_str());
            }
            std::fflush(stdout);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!g_failures.empty() && g_jsonMode) {
        // 机器可读的失败明细（验收脚本出错时用来定位；--json 下仍打到 stderr）
        for (const auto& f : g_failures) std::fprintf(stderr, "FAIL %s\n", f.c_str());
    }

    if (g_jsonMode) {
        std::string out = "{\n";
        out += "  \"engineVersion\": \"" + jsonEscape(kEngineVersion) + "\",\n";
        out += "  \"cases\": " + std::to_string(g_cases) + ",\n";
        out += "  \"casesFailed\": " + std::to_string(g_casesFailed) + ",\n";
        out += "  \"asserts\": " + std::to_string(g_asserts) + ",\n";
        out += "  \"assertsFailed\": " + std::to_string(g_failed) + ",\n";
        out += "  \"elapsedMs\": " + std::to_string(ms) + ",\n";
        out += "  \"result\": \"" + std::string(g_failed == 0 ? "ALL GREEN" : "FAILED") + "\",\n";
        out += "  \"details\": [";
        for (std::size_t i = 0; i < g_metas.size(); ++i) {
            const CaseMeta& m = g_metas[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "    {\"name\": \"" + jsonEscape(m.name) + "\", \"ok\": " +
                   (m.ok ? "true" : "false") + ", \"asserts\": " + std::to_string(m.asserts) +
                   ", \"failed\": " + std::to_string(m.failed) +
                   ", \"reqs\": " + jsonArray(m.reqs) + "}";
        }
        out += "\n  ],\n";
        out += "  \"failures\": " + jsonArray(g_failures) + "\n";
        out += "}\n";
        std::fputs(out.c_str(), stdout);
        return g_failed == 0 ? 0 : 1;
    }

    std::printf("\n================ alert-engine selftest ================\n");
    std::printf("用例 %d 个（失败 %d ）｜断言 %d 条（失败 %d ）｜耗时 %.1f ms\n", g_cases,
                g_casesFailed, g_asserts, g_failed, ms);
    if (!g_failures.empty()) {
        std::printf("\n---- 失败明细 ----\n");
        for (const auto& f : g_failures) std::printf("  %s\n", f.c_str());
    }    std::printf("结果： %s\n", g_failed == 0 ? "ALL GREEN" : "FAILED");
    std::printf("========================================================\n");
    return g_failed == 0 ? 0 : 1;
}
