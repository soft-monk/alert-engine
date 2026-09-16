# alert-engine · 告警与事件引擎

> **告警只是一个有上限的内存数组，没有规则、分级、去重与确认，却已有导航入口与报告指标。**
> 本仓把这个缺口补成一个**独立可复用引擎**：规则求值 → 产生分级 → 去重合并 → 抖动抑制 →
> 升级 → 抑制关系 → 风暴保护 → 确认与生命周期 → 台账查询与订阅分发。

`alert-engine` 回答一句话：**现在有没有需要人看的事、多严重、看过没有。**

- **后端 C++17 库**（CMake ≥ 3.20），仅依赖 `nlohmann/json`
- **不含任何业务规则**：级别取值、阈值、持续时间、去重窗口、抑制表、风暴上限、消音时长全部来自规则包 `policies/mapapp/alertRules.json`
- **出口全走反向接口**：`IAlertStore` / `IAlertSink` / `IClock` / `ILogSink` 由宿主注入；引擎不落库、不广播、不取挂钟、不推 UI

---

## 快速开始

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\selftest.exe                  # 零依赖自测（43 个用例 / 937 条断言）
powershell -ExecutionPolicy Bypass -File scripts/acceptance.ps1   # 独立验收（56 项检查，退出码 0/1）
```

最小用法（宿主装配形态）：

```cpp
#include "alert_engine/alert_engine.h"
using namespace alert_engine;

AlertEngineOptions opts;
opts.clock = myClock;   // IClock：让去重窗口与持续时间可复现（ALT-NFR-02）
opts.sink  = mySink;    // IAlertSink：把 alert.raised/updated/acked 落库 + 广播
opts.store = myStore;   // IAlertStore：写前提交（save 成功才算生效）
AlertEngine engine(opts);

engine.loadRulesFromDirectory("policies/mapapp");   // kind:"alertRules"

Observation o;
o.ruleId = "node-offline";  o.entityId = "node-1";  o.missionId = "mission-1";
o.metric = "online";        o.value = 0;            o.ts = clock->nowMs();
engine.observe(o);                                  // 结构化观测，不是句子

engine.acknowledge("a-1", "operator-7", "已派人巡检");  // 重复确认是幂等成功
```

---

## 引擎与规则的划分

| 属引擎（可复用） | 属规则包（`policies/mapapp/alertRules.json`） |
|---|---|
| 四类条件求值框架（阈值 / 持续 / 布尔组合 / 恢复） | 每条规则的具体阈值、`forMs`、字段名 |
| 产生、分级机制、去重锚点 | 级别目录（`key` / `name` / `rank`）与各规则的级别 |
| 去重窗口、合并策略、抖动抑制、升级 | 各规则的去重窗口、`merge`、`confirmCount`、`minDwellMs`、`cooldownMs` |
| 抑制关系框架、风暴保护 | 父告警 → 子告警的抑制表、风暴窗口与上限、汇总告警模板 |
| 确认 / 关闭 / 重开状态机、消音 | 自动关闭策略（`recoverOn`）、消音缺省时长 |
| 台账查询、订阅过滤、批量输出 | 告警文案模板（`title`，引擎只做 `{占位符}` 填充） |
| 出口接口、计数口径的实现 | `alert_count` 口径取值（`basis`） |

**判据**（需求 §4）：引擎公开接口里出现"失联 3 秒"、"链路受限"、"高危区"即破线 —— 由
`scripts/acceptance.ps1` 的 C07/C08/C09 守卫逐条机检。

---

## 关键口径

| 主题 | 口径 |
|---|---|
| **事件名** | 引擎只调用 `IAlertSink`，负载即事件的 `data`：`alert.raised` / `alert.updated` / `alert.acked`（protocol.md §4.4 已登记）。信封 `{type,data,ts}` 由宿主（`realtime-hub`）包 |
| **幂等** | 重复确认 = **`code=0` + `data.idempotent=true`**，零副作用（不改台账、不追加历史）；`1002` 只用于互斥冲突（冲突裁决 C15/C16/C17） |
| **去重锚点** | （规则 + **实体**）；窗口内归并为同一条并累加计数，跨窗口新开；**首次时间一经产生不再被合并修改** |
| **升级** | 合并中级别升高按规则包 `levels[].rank` 比较，记 `levelHistory` 与 `mergeHistory`，**原级别保留** |
| **抖动抑制** | `confirmCount`（连续确认次数）+ `minDwellMs`（最小驻留）+ `cooldownMs`（关闭后冷却期不新开）三档可配 |
| **持续时间** | 按节点在观测序列上累计连续成立时长；`maxGapMs` 为可选的连续性上限（缺省不限，避免时钟跳变时静默丢计时） |
| **风暴保护** | 单位时间新开条数超上限 → 折叠进**汇总告警**（`ruleId="$storm"`，kind=`aggregate`）并**如实上报** `foldedCount` |
| **计数口径** | `AlertCounts{basis, alertCount, rawRaises, alertsRaised, merged, recoveries, aggregates, folded, suppressed}`；可复算恒等式：`rawRaises == alertsRaised + recoveries + merged + folded + suppressed` |
| **台账查询** | 超限**截断但不静默丢**：`truncated` / `omitted` / `truncationReason` 如实上报 |
| **有界台账** | `maxActiveAlerts` / `retentionMs` 可配，淘汰计入 `metrics.evicted`；有界清扫按节流周期执行 |
| **确定性** | 一切"现在"经 `IClock`；观测自带 `ts` 驱动持续时间与去重窗口；结果 `toJson()` 双跑逐字节一致 |

---

## 目录

```
include/alert_engine/alert_engine.h   唯一公开头（宿主只允许包含它）
src/                                  实现（internal.h / util.cc / condition.cc / policies.cc / engine.cc / json.cc / serialize.cc）
policies/mapapp/alertRules.json       规则包（kind:"alertRules"：levels / items / storm / mute）
examples/minimal                      最小示例：装载 → 触发 → 去重 → 确认 → 台账 → 计数口径
examples/full_flow                    全流程演示：14 段覆盖全部机制
tests/selftest.cc                     零依赖自测（43 用例 / 937 断言；--json 供验收对账）
scripts/acceptance.ps1                独立验收（需求 §7 逐条 + 结构守卫，退出码 0/1）
docs/契约/alert-engine契约.md          对外接口契约（公开面 / 错误码 / 事件 / 规则包 / 反向接口）
docs/需求/alert-engine需求专篇.md      需求专篇（唯一权威，35 条）
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/需求/alert-engine需求专篇.md`](docs/需求/alert-engine需求专篇.md) | 需求专篇（唯一权威）：35 条条目、验收标准、边界、决策记录、风险、验收清单 |
| [`docs/契约/alert-engine契约.md`](docs/契约/alert-engine契约.md) | 对外接口契约：公开类型与签名、错误码、事件负载、规则包 schema、反向接口、口径与不变量 |
| [共享契约 protocol.md](https://github.com/soft-monk/phase-engine/blob/main/docs/契约/protocol.md) | 十个引擎的冻结共享契约（P1–P10、错误码、事件名、policies schema） |
| [冲突裁决.md](https://github.com/soft-monk/phase-engine/blob/main/docs/契约/冲突裁决.md) | C15（废弃 1001）/ C16（收窄 1002）/ C17（专篇与契约冲突以契约为准） |

---

## 状态

- 需求：已冻结（35 条，见上表）
- 契约（接口）：已产出（`docs/契约/alert-engine契约.md`）
- 实现：已完成（43 用例 / 937 断言全绿；验收 56 项全绿）
- 待人类裁决：级别取值与名称、`alert_count` 权威口径的**报告侧**落地、首批规则清单、告警页面形态
  （见需求专篇 §9 与契约 §11）

## 定位

这是一个**业务引擎**，与其它模块**互不 import**，只通过注入的反向接口与宿主装配。
与业务相关的内容（级别名、阈值、持续时间、抑制表、文案）一律通过规则包注入，不写进本仓代码。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE)。
