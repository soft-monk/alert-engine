# alert-engine · 告警与事件引擎契约（v0.1）

| 项 | 内容 |
|---|---|
| 文档编号 | `CTR-ALT-001` |
| 版本 | **v0.1（首版，随实现产出）** |
| 适用范围 | `alert-engine`：**后端 C++17 静态库**（无 Web 框架、无 SQL、无其它引擎内部依赖）。规则求值、告警产生与分级、去重合并与抖动抑制、升级、抑制关系、风暴保护、确认与生命周期、消音、台账查询、订阅式分发 |
| 本文件定位 | 本引擎对外接口的唯一基准。公开签名、字段名与取值、错误码、事件负载、规则包形状以本文件为准 |
| 需求依据 | [`../需求/alert-engine需求专篇.md`](../需求/alert-engine需求专篇.md)（`ALT-RULE/GEN/DEDUP/ACK/SUB/NFR`，共 **35** 条） |
| 上游共享契约 | [`protocol.md`](../../../phase-engine/docs/契约/protocol.md)（P1–P10、§3 错误码、§4 事件名、§5 policies schema、§6 反向接口命名） |
| 冲突裁决 | [`冲突裁决.md`](../../../phase-engine/docs/契约/冲突裁决.md)（**C15** 废弃 `1001`、**C16** 收窄 `1002`、**C17** 专篇与契约冲突时以 `protocol.md` 为准） |
| 一致性承诺 | §2 错误码**只取** protocol.md §3.2 的取值（不新增）；§3 事件名**只复用** protocol.md §4.4 已登记的三个；规则包 `kind:"alertRules"` 的清单段为 `items`（§5.3 已登记），`levels` / `storm` / `mute` 为**同 kind 兄弟段** |
| 状态 | 首版（已实现并自验）。破坏性变更 MUST 走 protocol.md §9 的修订流程 |

---

## 0. 这份契约解决什么

`alert-engine` 回答一句话：**现在有没有需要人看的事、多严重、看过没有**。

它**不推送**（`realtime-hub` 推）、**不画 UI**（宿主浮层）、**不含具体告警规则**（规则是数据）、
**不落库**（`IAlertStore` 由宿主实现）。

| 本引擎负责 | 不负责（归谁） |
|---|---|
| 四类条件求值与规则装载校验 | 规则的具体阈值、持续时间、字段名（规则包） |
| 告警产生、分级、幂等锚点 | 级别取值与名称（规则包 `levels`） |
| 去重窗口、合并、抖动抑制、升级 | 各规则的去重窗口与抖动参数（规则包） |
| 抑制关系框架、风暴保护 | 父子抑制表、风暴上限与汇总模板（规则包） |
| 确认 / 关闭 / 重开 / 消音与留痕 | 权限判断（宿主只把操作者 id 传进来） |
| 台账查询、订阅过滤、批量输出 | 广播与落库（宿主经 `IAlertSink` / `IAlertStore`） |
| 计数口径的**实现** | 报告侧取哪个口径（规则/宿主声明 `basis`） |

### 0.1 三条不可协商的口径

1. **引擎不认识任何业务规则。** 级别取值、阈值、持续时间、去重窗口、抑制表、风暴上限、
   消音时长、告警文案全部来自注入的规则包；引擎内 MUST NOT 出现任何业务取值（P6/P7）。
2. **引擎不落库、不广播、不取挂钟、不推 UI。** 出口为反向接口 `IAlertStore` / `IAlertSink` /
   `IClock` / `ILogSink`；未注入时引擎仍 MUST 可工作（P8/P9）。
3. **失败 MUST NOT 抛异常跨边界**（P10）。一切裁决走统一信封 `{code, message, data}`，
   `code` 只取 protocol.md §3.2 的取值；`1001` 保留不用（C15）。

### 0.2 不变量（任何操作序列后 MUST 成立）

| # | 不变量 | 依据 |
|---|---|---|
| 1 | `record.count == max(1, 合并输入次数)`（`merge=latest/max` 时口径见 §6.3） | ALT-GEN-03 |
| 2 | `firstAt` 一经产生 MUST NOT 被合并修改 | ALT-DEDUP-02 |
| 3 | `levelHistory[0].level == originLevel`；`level == levelHistory.back().level` | ALT-DEDUP-04 |
| 4 | `stateHistory` / `mergeHistory` / `levelHistory` 只追加、不改写 | ALT-ACK-03 |
| 5 | `counts.rawRaises == alertsRaised + recoveries + merged + folded + suppressed` | ALT-GEN-05 |
| 6 | 同输入 + 同假时钟 → `toJson()` 逐字节一致 | ALT-NFR-03 |

---

## 1. 公开入口

| 项 | 取值 |
|---|---|
| 语言 / 标准 | **C++17** |
| 构建 | **CMake ≥ 3.20**；目标名 `alert_engine`（静态库；示例/测试独立目标） |
| 唯一公开头 | `#include <alert_engine/alert_engine.h>`（公开面只有它） |
| 命名空间 | `alert_engine` |
| JSON 类型 | `nlohmann::ordered_json`（键序稳定 —— 确定性输出的前提） |
| 依赖 | **仅 `nlohmann/json`**（内置单头回落 `third_party/nlohmann/json.hpp`）；不依赖 Web 框架 / SQL / 其它引擎 |
| 对外字段命名 | **camelCase**（P4）；时间戳 **epoch 毫秒**（P5），类型 `int64_t` |
| 引擎不产出 | WS 信封 `{type,data,ts}`（由宿主 `realtime-hub` 侧包）；自然语言文案（只做规则模板占位符填充 —— 需求 D4） |

> 其余头文件（`src/internal.h` 等）为内部实现细节，**宿主 MUST NOT 包含**。

---

## 2. 错误码（protocol.md §3.2 的取值，逐值对齐）

| code | 含义 | 本引擎的触发点 |
|---|---|---|
| `0` | 成功（**唯一成功码**；幂等命中也是 `0` + `data.idempotent=true`） | 产生 / 合并 / 确认 / 装载成功 |
| `1000` | 请求参数错误 | 空 `ruleId` / 空 `mute.key` / 非法规则包形状 / 未知级别或算子 / 消音时长越界 |
| `1002` | **冲突拒绝**（HTTP 409） | 本引擎的同步原子路径**不产生**；保留语义边界，MUST NOT 用于幂等成功（C16） |
| `1003` | 前置条件未满足 | **非法状态迁移**（如对 `closed` 的告警再确认） |
| `1004` | 资源不存在 | 未知 `ruleId` / 未知 `alertId` / 未知 `muteId` |
| `1005` | 服务端执行失败 | `IAlertStore::save()` 返回 false 或抛异常（写前提交失败 → 内存状态不变） |
| `1006` | 版本不匹配 | 规则包 `schemaVersion` 的 `MAJOR` ≠ 引擎支持值（`1`） |
| ~~`1001`~~ | **保留不用**（C15） | —— |

**幂等与冲突的分工（C15 / C16 / C17）**

| 场景 | 返回 |
|---|---|
| 重复确认同一条告警 | `code=0`，`status="already-applied"`，`data.idempotent=true`，**零副作用** |
| 重复关闭 / 重复重开（已是目标状态） | 同上 |
| 他方持有 / 并发重入 / 宿主 gate 拒绝 | `1002` + `data.conflict=true`（HTTP 409） |

---

## 3. 事件

**引擎不发布事件，只调用 `IAlertSink`。** 三个负载即 protocol.md §4.4 已登记事件的 `data`。

### 3.1 `alert.raised`（告警产生 / 恢复记录 / 风暴汇总）

| 字段 | 类型 | 契约 | 说明 |
|---|---|---|---|
| `alertId` | string | ✅ 登记字段 | 台账 id（`a-<seq>`，确定性） |
| `ruleId` | string | ✅ | 规则 id（风暴汇总为 `$storm`） |
| `level` | string | ✅ | 当前级别取值（来自规则包） |
| `entityId` | string | ✅（可缺省） | **为空时省略该字段**（不写 null） |
| `missionId` | string | ✅ | 任务归属（ALT-SUB-02） |
| `firstAt` | int64 | ✅ | 首次触发时刻 |
| `count` | int64 | ✅ | 触发/合并计数 |
| `kind` | string | ➕ 只增 | `alert` \| `recovery` \| `aggregate` |
| `title` | string | ➕ | 规则模板填充后的文案 |
| `metric` / `value` | string / number | ➕ | 触发指标与数值 |
| `ruleVersion` | string | ➕ | 产生时的规则版本（ALT-RULE-05） |
| `originLevel` | string | ➕ | 首次触发时的级别（ALT-DEDUP-04） |
| `linkedAlertId` | string | ➕ | 恢复记录指向的原告警（ALT-GEN-04） |
| `foldedCount` | int64 | ➕ | 汇总告警：**被折叠条数**（ALT-DEDUP-06 如实上报） |
| `ts` | int64 | ➕ | 本次事件时间 |

### 3.2 `alert.updated`（合并 / 升级）

契约字段：`{alertId, count, lastAt, level, upgraded}`；
只增：`{ruleId, entityId, missionId, fromLevel, originLevel, value, countAdded, foldedCount, ts}`。

### 3.3 `alert.acked`（确认 / 关闭 / 重开 / 自动关闭）

契约字段：`{alertId, state, operatorId, at}`；
只增：`{ruleId, level, entityId, missionId, action, fromState, reason, idempotent, ts}`。

`action` 取值：`acknowledge` \| `close` \| `reopen` \| `auto-close`（自动关闭）。
`state` 取值：`active` \| `acknowledged` \| `closed`。

### 3.4 `AlertDelivery`（投递信封）

`IAlertSink` 的每个方法收到 `AlertDelivery`：`{event, payload, subscriberIds, subscriberCount, broadcast, ts}`。
`subscriberIds` 是**命中该条告警的订阅者**（按 id 升序）；为空时 `broadcast=true`（无订阅者声明，宿主自决）。

> **兼容说明**：既有 `alert` 事件名**继续保留**（CTR-EV-03，MUST NOT 改名/删除）；本引擎新增的三个
> 事件是**只增**，宿主可选择把 `alert.raised` 映射成既有的 `alert` 负载，前端 `applyWs` 零改动。

---

## 4. 反向接口（宿主 MUST 实现）

```cpp
class IAlertSink {          // 立即返回，MUST NOT 阻塞（protocol §6）
  virtual void onAlertRaised(const AlertDelivery&) = 0;
  virtual void onAlertUpdated(const AlertDelivery&) = 0;
  virtual void onAlertAcked(const AlertDelivery&) = 0;
};
class IAlertStore {         // 引擎不接触 SQL（P3）
  virtual bool save(const AlertRecord&) = 0;
  virtual bool load(const std::string& alertId, AlertRecord& out) = 0;
  virtual bool remove(const std::string& alertId) = 0;
  virtual bool supportsList() const { return false; }
  virtual std::vector<AlertRecord> list(const AlertQuery&) { return {}; }
};
class IClock { virtual int64_t nowMs() const = 0; };                  // epoch 毫秒（P5）
class ILogSink { virtual void log(int, const std::string&, const json&) {} 
                 virtual void commandAudit(const std::string&, const std::string&, const json&) {} };
```

| 约束 | 说明 |
|---|---|
| 写前提交 | `save()` 成功返回之后引擎才提交内存并通知 Sink；`false`/抛异常 → 本次操作 `1005`，**内存状态不变** |
| 通知在锁外 | Sink 回调在引擎互斥区**之外**派发；回调抛异常被吞掉并计入 `metrics.sinkErrors`，MUST NOT 影响已生效状态 |
| `load()` 未命中 | MUST 返回 `false`（MUST NOT 用空记录当命中 —— protocol §2.3） |
| 未注入 | 纯内存 + 空 Sink + `SystemClock`；`capabilities()` 如实标注 |

### 4.1 时钟口径（ALT-NFR-02）

| 用途 | 时间源 |
|---|---|
| 持续时间累计、去重窗口、台账记录时间戳 | **观测自带的 `ts`**（完全可复现） |
| 消音到期、风暴窗口、`tick(nowMs)` | 注入的 `IClock` |
| 未注入 | 回落 `SystemClock`（引擎唯一的非确定性来源），`capabilities().clockInjected=false` |

---

## 5. 规则包（`kind: "alertRules"`）

骨架遵守 protocol.md §5.1；清单段为 `items`（§5.3 已登记），`levels` / `storm` / `mute` 为同 kind 兄弟段。

```jsonc
{
  "policiesNamespace": "mapapp",
  "schemaVersion": "1.0.0",
  "kind": "alertRules",
  "levels": [ { "key": "warn", "name": "警告", "rank": 2, "color": "yellow" } ],
  "items": [
    { "key": "node-offline", "version": "1.0.0", "enabled": true, "level": "error",
      "title": "节点 {entityId} 失联",
      "condition": { "type": "duration", "forMs": 3000,
                     "condition": { "type": "threshold", "field": "online",
                                    "operator": "eq", "value": false } },
      "dedupWindowMs": 10000, "merge": "accumulate", "recoverOn": "record",
      "suppressChildren": true, "confirmCount": 1, "minDwellMs": 0, "cooldownMs": 0 }
  ],
  "storm": { "enabled": true, "windowMs": 1000, "maxRaised": 50, "level": "error",
             "title": "告警风暴：{windowMs} 毫秒内超过 {maxRaised} 条，已折叠 {folded} 条" },
  "mute": { "defaultMs": 300000, "maxMs": 3600000 }
}
```

### 5.1 条目字段

| 字段 | 必填 | 缺省 | 含义 |
|---|---|---|---|
| `key`（= `id`） | MUST | — | 规则 id，`^[a-z][a-z0-9-]{0,63}$`，唯一 |
| `version` | SHOULD | `""` | 规则版本（告警携带，可反查） |
| `enabled` | SHOULD | `true` | 初值；运行时可 `setRuleEnabled` |
| `level` | MUST | — | 级别取值，MUST ∈ `levels[].key` |
| `missionId` | MAY | `""` | 规则级任务归属；观测给的 `missionId` 覆盖它 |
| `title` | MAY | `""` | 文案模板，支持 `{ruleId}{entityId}{missionId}{metric}{level}{value}{ts}` |
| `condition` | MUST | — | 触发条件（§5.2） |
| `dedupWindowMs` | SHOULD | `0` | 去重窗口；`0` = 不合并（每次触发新开一条） |
| `merge` | SHOULD | `accumulate` | `accumulate` \| `latest` \| `max` \| `keep` |
| `recoverOn` | SHOULD | `none` | `none` \| `record` \| `close` |
| `parent` | MAY | `""` | 父规则 id：父告警活动时本条被抑制 |
| `suppressChildren` | MAY | `false` | 本条为父：抑制其余全部规则 |
| `suppressChildrenOf` | MAY | `[]` | 反向批量声明被本条抑制的子规则 id |
| `confirmCount` | SHOULD | `1` | 抖动抑制：连续 N 次成立才产生 |
| `minDwellMs` | SHOULD | `0` | 抖动抑制：判定成立后的最小驻留时长 |
| `cooldownMs` | SHOULD | `0` | 关闭后该时长内不新开，只并入原记录并重新打开 |
| `maxGapMs` | SHOULD | `0` | 持续时间连续性上限；**缺省不限**（避免时钟跳变静默丢计时） |

### 5.2 条件类型（四类机制 —— ALT-RULE-02）

```jsonc
{ "type": "threshold", "field": "signal", "operator": "lt", "value": 0.45 }
{ "type": "duration", "forMs": 5000, "condition": { /* 任意条件 */ } }
{ "type": "bool", "logic": "allOf", "conditions": [ /* … */ ] }        // 或 "not" + "condition"
{ "type": "recovery", "condition": { /* 被监视的原始条件 */ } }         // "条件不再满足" 即为真
```

| 算子 | 取值 |
|---|---|
| 数值 | `eq` `ne` `gt` `gte` `lt` `lte` |
| 存在性（**缺失**条件） | `exists` `missing` |
| 集合 | `in` `notIn`（配 `values` 数组） |

**字段解析顺序**：`values[field]` → `context[field]` → `obs.metric == field` 时的 `obs.value`；
都未命中即"字段缺失"（`missing` 成立、`exists` 不成立）。**字段名一律来自规则包**，引擎不认识任何具体指标。

**恢复语义的两条要点**

1. `recovery` 条件成立 ⟺ 其内部条件**曾经成立过**（持久锚点）且**现在不成立**；只看"当前不成立"
   会在监控刚接入时误报恢复。
2. 告警的"条件不再满足"（用于 `recoverOn`）以**台账记录**上的 `conditionHeld` 为准：
   只有"曾经成立过的 `kind=alert` 记录"才谈得上恢复（恢复记录与风暴汇总不再被判一次恢复）。

### 5.3 装载校验（CTR-PL-01..06）

| # | 校验 | 结果 |
|---|---|---|
| 1 | `kind != "alertRules"` / 缺 `policiesNamespace` / 非法 `schemaVersion` / 缺 `items` 或 `levels` | 拒绝（`1000`），逐条给 `path` + `field` + `reason` |
| 2 | `schemaVersion` 的 `MAJOR` ≠ 1 | 拒绝（`1006`），MUST NOT 静默降级 |
| 3 | 重复 `key` / 非法 id / `level` 不在目录 / `rank` 重复 / `parent` 或 `suppressChildrenOf` 指向不存在 / 抑制关系成环 | 拒绝（`1000`），不短路 |
| 4 | 未知字段 / 未知顶层段 | **忽略 + 计入 `warnings` 与 `metrics.unknownFields`**（CTR-PL-03），MUST NOT 失败 |
| 5 | 装载失败 | **原子替换**：保留上一次成功装载的规则 |

---

## 6. 公开 API 摘要

### 6.1 规则包

`loadRules(json)` / `loadRulesFile(path)` / `loadRulesFromDirectory(dir)` /
`validateRules(json)`（静态纯函数）/ `rulesInfo()` / `setRuleEnabled(id, on)` / `isRuleEnabled(id)` /
`levels()` / `levelRank(level)`

### 6.2 产生与生命周期

```cpp
AlertResult observe(const Observation&);          // 唯一产生入口（结构化输入）
AlertResult observe(const json&);                 // 便利重载
json        tick(int64_t nowMs);                  // 由宿主驱动时间前进（消音到期 / 有界清扫）

ActionResult acknowledge(alertId, operatorId, reason);   // 幂等
ActionResult close(alertId, operatorId, reason);
ActionResult reopen(alertId, operatorId, reason);

MuteResult mute(MuteRequest);                     // 有时限，到期自动恢复
MuteResult unmute(muteId, operatorId);
std::vector<MuteEntry> mutes();
```

`Observation` = `{ruleId, entityId?, missionId, metric, value, ts, context{}, values{}}`。

### 6.3 合并策略口径

| `merge` | 计数 | 最近数值 | 典型用途 |
|---|---|---|---|
| `accumulate`（缺省） | 累加 | 更新 | "3 秒内触发 10 次 → 计数 10" |
| `latest` | 不累加 | 更新 | 持续型指标（取最新值即可） |
| `max` | 不累加 | 取最大 | 峰值型指标 |
| `keep` | 不累加 | 不变 | 只关心"仍存在"（`lastAt` 仍刷新，保证"最近一次时间"可读） |

### 6.4 台账查询（ALT-SUB-01/02/05）

```cpp
AlertQueryResult listAlerts(const AlertQuery& = {});   // 级别/状态/实体/规则/任务/类型/时间区间 + limit/offset
std::optional<AlertRecord> getAlert(const std::string& alertId) const;   // 不存在 → nullopt
json activeAlerts() const;      // 批量：一次拉取全部活动告警（limit=0 表示不限量）
AlertCounts counts() const;     // 计数口径（§7）
```

`AlertQueryResult` = `{total, returned, truncated, omitted, truncationReason, limit, offset, items[]}`；
缺省 `limit=100`，**超限截断时 `truncated=true` 且 `omitted` 如实上报**（MUST NOT 静默丢）。
时间区间缺省按 `lastAt` 判定，`useFirstAt=true` 时按 `firstAt`（口径显式）。

### 6.5 订阅（ALT-SUB-03）

```cpp
ActionResult subscribe(const AlertSubscription&);   // subscriberId 覆盖式注册
bool unsubscribe(const std::string& subscriberId);
std::vector<AlertSubscription> subscriptions() const;
```

`AlertSubscription` = `{subscriberId, ruleIds[], levels[], entityIds[], missionIds[], includeRecovery, includeAggregate}`；
**空集合 = 不筛该项**，每个非空集合 MUST 命中（AND）—— 引擎只推匹配项。

### 6.6 自述与观测

`capabilities()` / `metrics()` / `resetMetrics()` / `errorCodeName(code)`。

---

## 7. 计数口径（ALT-GEN-05）

```jsonc
{ "basis": "deduplicated",   // 报告侧 SHOULD 取哪个口径（D7 缺省去重后条数）
  "alertCount": 3,           // 去重后条数 = 台账记录条数
  "rawRaises": 12,           // 原始条数 = 每一次"条件满足的触发"
  "alertsRaised": 2, "merged": 8, "recoveries": 1, "aggregates": 0,
  "suppressed": 1, "folded": 0, "openActive": 2 }
```

**恒等式**：`rawRaises == alertsRaised + recoveries + merged + folded + suppressed`。
宿主 MUST 按 `basis` 取数，并把 `basis` 一并落进报告，否则口径会漂移。

---

## 8. 宿主装配约定（非规范建议）

| 项 | 要求 |
|---|---|
| 依赖注入 | `AlertEngineOptions{store, clock, sink, log, maxActiveAlerts, retentionMs, alertCountBasis}` |
| 规则包装载 | 启动期读 `policies/<ns>/alertRules.json` → `loadRulesFromDirectory`；失败 MUST NOT 静默继续 |
| 事件广播 | 在 `IAlertSink` 回调里包 `realtime-hub` 信封 `{type, data, ts}`；`ts` MUST 用负载里的 `ts` |
| HTTP 路由（**非规范建议**） | `POST /api/v1/alerts/observations`、`GET /api/v1/alerts`、`GET /api/v1/alerts/active`、`POST /api/v1/alerts/{id}/ack｜close｜reopen`、`POST /api/v1/alerts/mutes`、`GET /api/v1/alerts/counts` |
| 观测采集 | 其它引擎的状态变更（`deviceHealth` / `telemetry.link.quality` / `entity-ledger` / `selfcheck` / `resource-alloc`）由**宿主**翻译成 `Observation` 后喂入 |

---

## 9. 可机检清单（本契约的验收）

| # | 断言 | 方式 |
|---|---|---|
| 1 | 引擎产物（`include/src/scripts/CMakeLists`）内业务词零命中，规则包内命中 > 0 | `acceptance.ps1` C07/C07b |
| 2 | 引擎产物内级别取值字面量与硬编码阈值零命中 | C08 / C09 |
| 3 | 跨仓 `#include` 与 Web/SQL/网络库依赖零命中 | C10 / C11 |
| 4 | 不产生保留码 `1001`；幂等成功走 `code=0 + idempotent` | C12 + selftest `proto_*` |
| 5 | 公开头声明四个反向接口且全部经 `AlertEngineOptions` 注入 | C14 / C15 |
| 6 | 唯一公开头；内部头标注宿主不可包含 | C16 / C17 |
| 7 | `alert.raised/updated/acked` 的契约字段齐备 | C18 + `proto_event_payload_shapes` |
| 8 | 四类条件类型在公开面齐全 | C19 + `rule02_four_condition_kinds` |
| 9 | 计数口径显式 / 截断如实上报 / 折叠条数上报 | C20 / C21 / C22 |
| 10 | 规则包骨架、级别目录可扩展、去重窗口按规则可配、抑制表与风暴声明 | C24–C28 |
| 11 | 35 条需求每条至少一个通过用例 | C05 / C06（selftest `--json` 对账） |
| 12 | 需求 §7 的 21 行验收清单逐条通过 | C③ 段 S07-01..S07-21 |

---

## 10. 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v0.1 | — | 首版：公开面（`AlertEngine` / 结果类型 / 观测与记录）、错误码（只取 protocol §3.2）、三个已登记事件负载、反向接口四个、规则包 `kind:"alertRules"`（`levels`/`items`/`storm`/`mute`）、四类条件、计数口径与六条不变量、可机检清单 12 条 |

---

## 11. 开放问题（需人类裁决，不阻塞实现）

| # | 问题 | 影响 | 现状 |
|---|---|---|---|
| 1 | 告警**级别取值与名称**（"信息/警告/严重"是否够用，是否要"提示"级） | ALT-GEN-02 | 级别目录已参数化；规则包给了 `info/warn/error` 三级 + `rank`，四级及以上只需改规则包 |
| 2 | `alert_count`（风险预警次数）在**报告侧**取哪个口径 | ALT-GEN-05 / D7 | 引擎两种口径同时在案并声明 `basis=deduplicated`；报告侧尚未接线 |
| 3 | 首批规则清单（哪些事件必须报警） | ALT-RULE-01 | 规则包给了 9 条（含 1 条默认禁用）；属演示集，需产品确认 |
| 4 | 【告警】页面形态（列表 / 筛选 / 确认按钮 / 历史） | ALT-SUB-01 | 引擎侧接口齐备（筛选 / 分页 / 截断 / 批量活动告警），前端未接线 |
| 5 | 是否需要通知渠道（短信/声光/邮件） | ALT-SUB-* | 本引擎不做通知渠道；如需由宿主在 `IAlertSink` 内转发 |
| 6 | 告警是否落 `biz-store` 实体表还是只落事件日志 | ALT-SUB-04 / R5 | 引擎只出 `AlertRecord`；落库形态由宿主决定（当前无该仓） |
| 7 | `maxGapMs` 缺省口径（本实现取"不限"） | ALT-RULE-02 | 若宿主采样节拍固定，建议在规则里显式给 `maxGapMs`，避免断档被误判为持续 |
