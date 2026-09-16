# scripts/acceptance.ps1 · alert-engine 独立验收脚本（退出码 0/1）
#
# 权威依据：docs/需求/alert-engine需求专篇.md
#   ① §7 验收清单 21 行 —— 逐行变成一条可执行检查（S07-01..S07-21）
#   ② §3 功能需求 35 条（ALT-RULE 5 / ALT-GEN 6 / ALT-DEDUP 6 / ALT-ACK 5 / ALT-SUB 6 / ALT-NFR 7）
#   ③ §1.4 硬约束 + 上游共享契约 ../phase-engine/docs/契约/protocol.md
#      （P1/P6/P7/P8/P9/P10、§3.2 码表与 1001 保留不用、§4.4 事件负载、§5 规则包 schema）
#   ④ 冲突裁决 ../phase-engine/docs/契约/冲突裁决.md（C15 / C16 / C17）
#
# 设计：**引擎行为一律由 tests/selftest 断言**（selftest --json 输出逐用例结果与需求编号），
# 本脚本只做三件它做不了的事：构建生命周期、结构纪律检索（全仓/跨仓/业务词/硬编码数值）、
# 需求↔用例对账。这样"行为口径"只有一个来源，不会出现脚本与单测两套断言的漂移。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/acceptance.ps1
#   ... -SkipBuild                只跑自测与检查（复用已有构建产物）
#   ... -Config Debug             换构建配置
#   ... -Generator "Ninja"        换生成器（单配置生成器也可）
#
# 兼容 Windows PowerShell 5.1（不依赖 pwsh / PS7 语法）。
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64",
    [switch]$SkipBuild
)

# 控制台按 UTF-8 读脚本与写输出（PS 5.1 的默认代码页会把中文读坏）
[void][System.Reflection.Assembly]::LoadWithPartialName("System.Text.Encoding")
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$ErrorActionPreference = "Stop"
$script:Results = New-Object System.Collections.Generic.List[object]
$script:ChecksFailed = 0

# ------------------------------------------------------------------ 输出小工具
function Check([string]$id, [string]$title, [bool]$ok, [string]$detail) {
    $state = "PASS"
    if (-not $ok) { $state = "FAIL"; $script:ChecksFailed++ }
    $script:Results.Add([pscustomobject]@{ Id = $id; Title = $title; Ok = $ok; Detail = $detail })
    $color = "Green"
    if (-not $ok) { $color = "Red" }
    Write-Host ("  [{0}] {1} · {2}" -f $state, $id, $title) -ForegroundColor $color
    if ($detail) { Write-Host ("         {0}" -f $detail) -ForegroundColor DarkGray }
}

function Section([string]$title) {
    Write-Host ""
    Write-Host ("=" * 78)
    Write-Host $title
    Write-Host ("=" * 78)
}

function Rel([string]$full) {
    $root = $script:Repo
    if ($full.StartsWith($root)) { return $full.Substring($root.Length).TrimStart('\', '/') }
    return $full
}

function Format-Hits($hits) {
    if (-not $hits -or $hits.Count -eq 0) { return "" }
    $head = @($hits | Select-Object -First 5)
    $s = "：" + ($head -join "；")
    if ($hits.Count -gt 5) { $s += ("；…共 {0} 处" -f $hits.Count) }
    return $s
}

function SearchHits([string[]]$files, [string]$regex, [string[]]$skipLineRegex) {
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($f in $files) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $n++
            if ($line -notmatch $regex) { continue }
            if ($skipLineRegex) {
                $skip = $false
                foreach ($s in $skipLineRegex) { if ($line -match $s) { $skip = $true; break } }
                if ($skip) { continue }
            }
            $hits.Add(("{0}:{1}" -f (Rel $f), $n))
        }
    }
    return $hits
}

function CollectFiles([string[]]$dirs, [string[]]$exts, [string[]]$excludeDirs) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($d in $dirs) {
        $full = Join-Path $script:Repo $d
        if (-not (Test-Path $full)) { continue }
        Get-ChildItem -Path $full -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            if ($exts -notcontains $_.Extension) { return }
            $rel = Rel $_.FullName
            foreach ($x in $excludeDirs) { if ($rel -like ($x + "*")) { return } }
            $out.Add($_.FullName)
        }
    }
    return $out
}

# ------------------------------------------------------------------ 前置
$script:Repo = Split-Path -Parent $PSScriptRoot
Push-Location $script:Repo
try {
    Write-Host "alert-engine 独立验收（需求专篇 §7 逐条 + 结构纪律 + selftest 对账）"
    Write-Host ("仓库：{0}" -f $script:Repo)
    Write-Host ("PowerShell：{0}" -f $PSVersionTable.PSVersion)

    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        foreach ($p in @("C:\Program Files\CMake\bin\cmake.exe",
                         "C:\Program Files (x86)\CMake\bin\cmake.exe")) {
            if (Test-Path $p) { $cmake = $p; break }
        }
    }
    if (-not $cmake) { Write-Host "找不到 cmake（>= 3.20）" -ForegroundColor Red; exit 1 }
    $cmakeVersion = (& $cmake --version | Select-Object -First 1)
    Write-Host ("cmake：{0}（{1}）" -f $cmake, $cmakeVersion)

    $buildPath = Join-Path $script:Repo $BuildDir

    # ================================================================ ① 构建（ALT-NFR-05）
    Section "① 构建生命周期（ALT-NFR-05：一条命令构建 → 一条命令验收）"

    $isMultiConfig = ($Generator -like "Visual Studio*")
    $binCandidates = @()
    if ($isMultiConfig) { $binCandidates += (Join-Path $buildPath "bin\$Config") }
    else { $binCandidates += (Join-Path $buildPath "bin") }
    $binCandidates += (Join-Path $buildPath "bin\$Config")
    $binCandidates += (Join-Path $buildPath "bin")

    function Resolve-Bin() {
        foreach ($c in $binCandidates) {
            if ((Test-Path (Join-Path $c "selftest.exe")) -or (Test-Path (Join-Path $c "selftest"))) {
                return $c
            }
        }
        return $binCandidates[0]
    }

    $configureOk = $false
    $buildOk = $false
    if ($SkipBuild) {
        $configureOk = (Test-Path (Join-Path $buildPath "CMakeCache.txt"))
        $buildOk = (Test-Path (Join-Path (Resolve-Bin) "selftest.exe")) -or
                   (Test-Path (Join-Path (Resolve-Bin) "selftest"))
        Check "C01" "构建：-SkipBuild 复用已有产物" ($configureOk -and $buildOk) `
            ("configure={0} binary={1}" -f $configureOk, $buildOk)
    } else {
        Write-Host ("    cmake -S . -B {0} -G '{1}'" -f $BuildDir, $Generator)
        & $cmake -S $script:Repo -B $buildPath -G $Generator -A $Arch 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $configureOk = ($LASTEXITCODE -eq 0)

        Write-Host ("    cmake --build {0} --config {1}" -f $BuildDir, $Config)
        & $cmake --build $buildPath --config $Config 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $buildOk = ($LASTEXITCODE -eq 0)

        $bin = Resolve-Bin
        $hasSelftest = (Test-Path (Join-Path $bin "selftest.exe")) -or
                       (Test-Path (Join-Path $bin "selftest"))
        $hasExamples = ((Test-Path (Join-Path $bin "example_minimal.exe")) -or
                        (Test-Path (Join-Path $bin "example_minimal"))) -and
                       ((Test-Path (Join-Path $bin "example_full_flow.exe")) -or
                        (Test-Path (Join-Path $bin "example_full_flow")))
        Check "C01" "构建：配置 + 编译通过，产物齐全（selftest + 2 个示例）" `
            ($configureOk -and $buildOk -and $hasSelftest -and $hasExamples) `
            ("configure={0} build={1} selftest={2} examples={3} bin={4}" -f `
                $configureOk, $buildOk, $hasSelftest, $hasExamples, (Rel $bin))
    }

    $bin = Resolve-Bin
    $selftestExe = Join-Path $bin "selftest.exe"
    if (-not (Test-Path $selftestExe)) { $selftestExe = Join-Path $bin "selftest" }

    # ① -2 CMake 最低版本 >= 3.20 且 C++17
    $cmakeLists = [System.IO.File]::ReadAllText((Join-Path $script:Repo "CMakeLists.txt"))
    $minOk = $false
    if ($cmakeLists -match 'cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+\.[0-9]+)') {
        $minOk = ([version]$Matches[1] -ge [version]"3.20")
    }
    $cxx17 = ($cmakeLists -match 'CMAKE_CXX_STANDARD\s+17')
    Check "C02" "构建：CMake >= 3.20 且 C++17（需求 §1.4）" ($minOk -and $cxx17) `
        ("cmake_min={0} cxx17={1}" -f $minOk, $cxx17)

    # ================================================================ ② selftest（行为口径的唯一来源）
    Section "② 零依赖自测（tests/selftest --json）"

    $jsonOk = $false
    $data = $null
    $jsonPath = Join-Path ([System.IO.Path]::GetTempPath()) ("alert-engine-selftest-{0}.json" -f $PID)
    if (Test-Path $selftestExe) {
        # 让子进程**直接写文件**：selftest --json 输出 UTF-8（含中文用例名），
        # 走管道会被控制台代码页（936）解成乱码，JSON 随之不可解析。
        & $selftestExe --json > $jsonPath
        $raw = ""
        if (Test-Path $jsonPath) {
            $raw = [System.IO.File]::ReadAllText($jsonPath, [System.Text.Encoding]::UTF8)
        }
        try {
            $data = $raw | ConvertFrom-Json
            $jsonOk = $true
        } catch {
            Write-Host "    selftest --json 解析失败：$($_.Exception.Message)" -ForegroundColor Red
            if ($raw.Length -gt 0) { Write-Host ($raw.Substring(0, [Math]::Min(400, $raw.Length))) }
        }
    }
    Check "C03" "自测可执行且 --json 输出可解析" $jsonOk ("exe={0}" -f (Rel $selftestExe))

    $caseByName = @{}
    $reqToCases = @{}
    if (-not $jsonOk) {
        Check "C03b" "自测结果可用" $false "selftest --json 无有效输出"
    } else {
        Write-Host ("    用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）｜耗时 {4:N1} ms｜结果 {5}" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed, $data.elapsedMs,
            $data.result)
        Check "C04" "自测全绿：用例失败 0 且断言失败 0" `
            (($data.casesFailed -eq 0) -and ($data.assertsFailed -eq 0)) `
            ("cases={0}/{1} asserts={2}/{3}" -f $data.cases, $data.casesFailed, $data.asserts,
             $data.assertsFailed)

        foreach ($c in $data.details) {
            $caseByName[$c.name] = $c
            foreach ($r in $c.reqs) {
                if (-not $reqToCases.ContainsKey($r)) {
                    $reqToCases[$r] = New-Object System.Collections.Generic.List[string]
                }
                $reqToCases[$r].Add($c.name)
            }
        }

        # ---- 35 条需求 → 用例对账
        $allReqs = New-Object System.Collections.Generic.List[string]
        foreach ($d in @("RULE:01..05", "GEN:01..06", "DEDUP:01..06", "ACK:01..05",
                         "SUB:01..06", "NFR:01..07")) {
            $dom = $d.Split(':')[0]
            $parts = ($d.Split(':')[1] -replace '\.\.', ' ').Split(' ')
            for ($i = [int]$parts[0]; $i -le [int]$parts[1]; $i++) {
                $allReqs.Add(("ALT-{0}-{1:D2}" -f $dom, $i))
            }
        }
        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { $missing.Add($r) }
        }
        Check "C05" "需求覆盖：35 条 ALT-* 每条都有用例引用" `
            (($allReqs.Count -eq 35) -and ($missing.Count -eq 0)) `
            ("总需求 {0} 条；无对应用例：{1}" -f $allReqs.Count,
             (($missing -join ", ") -replace '^$', '无'))

        $reqFailed = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { continue }
            $anyOk = $false
            foreach ($cn in $reqToCases[$r]) { if ($caseByName[$cn].ok) { $anyOk = $true } }
            if (-not $anyOk) { $reqFailed.Add($r) }
        }
        Check "C06" "需求覆盖：每条至少有一个通过用例" ($reqFailed.Count -eq 0) `
            ("未通过：{0}" -f (($reqFailed -join ", ") -replace '^$', '无'))
    }

    # ================================================================ ③ §7 验收清单逐条（21 行）
    Section "③ 需求专篇 §7 验收清单（逐条，21 行）"

    $checklist = @(
        @{ Id = "S07-01"; Line = "空环境 clone → 一条命令构建 → 一条命令跑通验收（退出码 0）"
           Cases = @(); NeedsBuild = $true },
        @{ Id = "S07-02"; Line = "全仓检索具体阈值与业务规则 → 零命中"
           Cases = @(); Guard = "business-words" },
        @{ Id = "S07-03"; Line = "四类条件（阈值/持续/组合/恢复）各有一条用例通过"
           Cases = @("rule02_four_condition_kinds") },
        @{ Id = "S07-04"; Line = "坏规则装载失败并指出位置"
           Cases = @("rule03_load_validation_reports_location") },
        @{ Id = "S07-05"; Line = "规则禁用后不产生新告警，历史仍可查"
           Cases = @("rule04_enable_disable_at_runtime") },
        @{ Id = "S07-06"; Line = "3 秒内触发 10 次 → 一条告警、计数 10"
           Cases = @("gen03_dedup_anchor_merges_and_counts") },
        @{ Id = "S07-07"; Line = "触发→恢复→再触发，三条记录关系正确"
           Cases = @("gen04_recovery_semantics", "ack05_auto_close_on_recovery_is_traced") },
        @{ Id = "S07-08"; Line = "跨窗口触发产生两条"
           Cases = @("dedup01_window_configurable_and_crossing_opens_new") },
        @{ Id = "S07-09"; Line = "升级时保留原级别历史"
           Cases = @("dedup04_upgrade_records_and_keeps_history") },
        @{ Id = "S07-10"; Line = "配置抑制后子告警不产生；父告警关闭后恢复"
           Cases = @("dedup05_parent_suppresses_child_until_closed") },
        @{ Id = "S07-11"; Line = "打风暴 → 得到汇总告警与被折叠条数"
           Cases = @("dedup06_storm_protection_reports_folded_count") },
        @{ Id = "S07-12"; Line = "确认/关闭/重开状态迁移正确；非法迁移被拒绝"
           Cases = @("ack01_state_machine_and_illegal_transitions") },
        @{ Id = "S07-13"; Line = "连点两次确认不报错"
           Cases = @("ack02_acknowledge_is_idempotent", "proto_idempotent_vs_conflict_are_separate") },
        @{ Id = "S07-14"; Line = "操作留痕可复原（操作者 + 时间）"
           Cases = @("ack03_audit_trail_is_recoverable") },
        @{ Id = "S07-15"; Line = "消音到期自动恢复"
           Cases = @("ack04_mute_is_time_limited_and_auto_recovers") },
        @{ Id = "S07-16"; Line = "台账按级别/实体/时间/状态筛选正确；按任务筛选正确"
           Cases = @("sub01_ledger_query_filters_and_truncation", "sub02_alerts_belong_to_mission") },
        @{ Id = "S07-17"; Line = "订阅过滤生效，不匹配不推"
           Cases = @("sub03_subscription_filtering") },
        @{ Id = "S07-18"; Line = "alert 事件负载保持前端现有字段可读（前端零改动）"
           Cases = @("sub06_event_payload_shape_and_compat", "proto_event_payload_shapes") },
        @{ Id = "S07-19"; Line = "注入假时钟，去重窗口测试可复现"
           Cases = @("nfr02_time_is_injectable",
                     "nfr03_determinism_byte_identical_double_run") },
        @{ Id = "S07-20"; Line = "并发产生告警计数准确"
           Cases = @("nfr04_thread_safety_no_lost_or_duplicated") },
        @{ Id = "S07-21"; Line = "1000 条/秒下单次去重与分发 P95 ≤ 1 ms"
           Cases = @("nfr06_performance_p95_under_1ms") }
    )

    if (-not $jsonOk) {
        foreach ($item in $checklist) { Check $item.Id $item.Line $false "自测结果不可用，无法对账" }
    } else {
        foreach ($item in $checklist) {
            if ($item.ContainsKey("NeedsBuild")) {
                Check $item.Id $item.Line ($configureOk -and $buildOk) `
                    "配置与构建成功 = 空环境一条命令可构建（详见 ① 的输出）"
                continue
            }
            if ($item.ContainsKey("Guard")) { continue }  # 结构类条目在 ④ 给结论（见 S07-02）
            $missingCases = New-Object System.Collections.Generic.List[string]
            $failedCases = New-Object System.Collections.Generic.List[string]
            foreach ($cn in $item.Cases) {
                if (-not $caseByName.ContainsKey($cn)) { $missingCases.Add($cn); continue }
                if (-not $caseByName[$cn].ok) { $failedCases.Add($cn) }
            }
            $ok = ($missingCases.Count -eq 0) -and ($failedCases.Count -eq 0)
            $detail = "用例：{0}" -f ($item.Cases -join ", ")
            if ($missingCases.Count -gt 0) { $detail += "；缺失：" + ($missingCases -join ", ") }
            if ($failedCases.Count -gt 0) { $detail += "；失败：" + ($failedCases -join ", ") }
            Check $item.Id $item.Line $ok $detail
        }
    }

    # ================================================================ ④ 结构纪律
    Section "④ 结构纪律：业务词 / 硬编码阈值 / 跨仓 import / 保留码（ALT-RULE-01、P1、P6、P7、C15）"

    # 引擎源码范围（需求 §4 的"引擎 vs 规则"判据）：
    #   纳入：include/ src/ scripts/ CMakeLists.txt —— **引擎产物**，业务词 MUST 零命中
    #   业务词豁免：docs/（需求与契约的示例语境）、policies/（**规则包 —— 业务词的合法住所**）、
    #     tests/ 与 examples/（测试数据与演示数据：演示 MUST 喂进真实的规则取值才跑得起来）。
    #   注意：examples/ 仍然参与 C10（跨仓 import）/ C11（第三方头）/ C13（落库广播）的检索。
    $engineFiles = [string[]]@(CollectFiles @("include", "src", "scripts") `
        @(".h", ".hpp", ".cc", ".cpp", ".ps1") @())
    foreach ($f in @("CMakeLists.txt")) {
        $full = Join-Path $script:Repo $f
        if (Test-Path $full) { $engineFiles += $full }
    }
    $linkFiles = [string[]]@(CollectFiles @("include", "src", "examples", "scripts") `
        @(".h", ".hpp", ".cc", ".cpp", ".ps1") @())
    foreach ($f in @("CMakeLists.txt", "examples\CMakeLists.txt", "tests\CMakeLists.txt")) {
        $full = Join-Path $script:Repo $f
        if (Test-Path $full) { $linkFiles += $full }
    }
    Write-Host ("    业务词检索范围 {0} 个引擎产物文件（docs/ policies/ tests/ examples/ 豁免）；" -f $engineFiles.Count)
    Write-Host ("    依赖与落库检索范围 {0} 个文件（含 examples/ 与两个 CMakeLists）" -f $linkFiles.Count)

    # C07：业务词零命中。模式**拆开拼接**并自带排除项 —— 否则守卫脚本自己会命中自己的正则
    # （自指陷阱：把拆分好的字面量再写进脚本，脚本本身就成了命中源）。
    $bizPattern = '失联|链路受限|高危区|节点失联' + '|光电无人机|雷达无人机|云边端|光电吊舱'
    $bizHits = @(SearchHits $engineFiles $bizPattern @('bizPattern', 'SearchHits', 'MUST NOT',
                                                      '零命中', '业务词', '检索范围', '判据'))
    Check "C07" "ALT-RULE-01 / P6：引擎产物（include/src/scripts/CMake）内业务词零命中" `
        ($bizHits.Count -eq 0) ("命中 {0} 处{1}" -f $bizHits.Count, (Format-Hits $bizHits))

    # C07b：业务取值确实住在规则包里（否则 C07 的"零命中"可能是假绿）
    $policyFiles = [string[]]@(CollectFiles @("policies") @(".json") @())
    $policyBizHits = @(SearchHits $policyFiles $bizPattern @())
    Check "C07b" "ALT-RULE-01：告警规则取值只住在规则包 policies/（换声明即换告警行为）" `
        ($policyBizHits.Count -gt 0) `
        ("规则包内命中 {0} 处（说明业务取值来自外部数据而非引擎）" -f $policyBizHits.Count)

    # C08：级别取值与规则 id 字面量零命中（级别目录属规则包 —— ALT-GEN-02）
    $levelPattern = '"warn"|"error"|"info"|''warn''|''error''|''info'''
    $levelHits = @(SearchHits $engineFiles $levelPattern @('levelPattern', 'SearchHits', 'MUST NOT',
                                                          '零命中', '检索范围', '级别'))
    Check "C08" "ALT-GEN-02：引擎产物内无级别取值字面量（级别目录来自规则包）" `
        ($levelHits.Count -eq 0) ("命中 {0} 处{1}" -f $levelHits.Count, (Format-Hits $levelHits))

    # C09：硬编码阈值零命中（阈值属规则包）。模式**拆成多段拼接**以消除自指。
    $q = [char]34
    $nums = '3000|5000|10000|60000|0\.45|0\.8|0\.6|0\.05'
    $demoPattern = ('[' + $q + '](' + $nums + ')[' + $q + ']' +
                    '|(=\s*|,\s*)(' + $nums + ')(\s*[,}\]])')
    $demoHits = @(SearchHits $engineFiles $demoPattern @('demoPattern', 'SearchHits',
                                                         'MUST NOT', '零命中', '检索范围'))
    Check "C09" "需求 §4：引擎产物内无硬编码阈值（阈值属规则包）" `
        ($demoHits.Count -eq 0) ("命中 {0} 处{1}" -f $demoHits.Count, (Format-Hits $demoHits))

    # C10：跨仓 import 零命中，且只允许 nlohmann/json（P1 / ALT-NFR-01）
    $otherRepos = '(telemetry_store|telemetry-store|device_ingest|device-ingest|realtime_hub|realtime-hub|' +
                  'map_2d|map-2d|entity_ledger|entity-ledger|phase_engine|phase-engine|scoring|topology|' +
                  'view_composer|view-composer|report_engine|report-engine|resource_alloc|resource-alloc|' +
                  'selfcheck|geo_data|geo-data|media_player|media-player|assembly_host|assembly-host|' +
                  'drogon|Drogon|sqlite|mysql|pqxx|libpq|nanodbc|oatpp|crow|httplib|winhttp|WinHttp|curl)'
    $incHits = @(SearchHits $linkFiles ('#\s*include\s*[<"]' + $otherRepos) @('otherRepos', 'MUST NOT',
                                                                              '零命中', '检索范围'))
    Check "C10" "P1 / ALT-NFR-01：跨仓 import 与 Web/SQL/网络库依赖零命中" ($incHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $incHits.Count, (Format-Hits $incHits))

    $allIncludes = New-Object System.Collections.Generic.List[string]
    foreach ($f in $linkFiles) {
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $m = [regex]::Match($line, '^\s*#\s*include\s+(?:<([^>]+)>|"([^"]+)")')
            if ($m.Success) {
                if ($m.Groups[1].Success) { $allIncludes.Add($m.Groups[1].Value) }
                else { $allIncludes.Add($m.Groups[2].Value) }
            }
        }
    }
    # 只允许：C/C++ 标准库（无扩展名的头）、本模块公开头（alert_engine/...）、
    # 本模块内部头（internal.h）、nlohmann/json。
    $nonJson = $allIncludes | Where-Object {
        ($_ -notmatch '^(alert_engine/|internal\.h$)') -and
        ($_ -notmatch '^[a-z_]+$') -and
        ($_ -notmatch '^(nlohmann|third_party)')
    }
    Check "C11" "仅依赖 nlohmann/json（无其它第三方头，ALT-NFR-01）" ($nonJson.Count -eq 0) `
        ("非标准库/非 nlohmann 的 include：{0}" -f (($nonJson -join ", ") -replace '^$', '无'))

    # C12：不产生保留码 1001（冲突裁决 C15；注释与字符串里的说明文字不算）
    $codeFiles = @(CollectFiles @("include", "src") @(".h", ".hpp", ".cc", ".cpp") @())
    $bad1001 = New-Object System.Collections.Generic.List[string]
    foreach ($f in $codeFiles) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $n++
            $stripped = ($line -replace '//.*$', '') -replace '/\*.*?\*/', ''
            if ($stripped -match '"') { continue }   # 字符串里的说明文字不计
            if ($stripped -match '(?<![0-9])1001(?![0-9])') { $bad1001.Add(("{0}:{1}" -f (Rel $f), $n)) }
        }
    }
    Check "C12" "冲突裁决 C15：不产生保留码 1001（幂等成功走 code=0 + idempotent）" `
        ($bad1001.Count -eq 0) ("命中 {0} 处{1}" -f $bad1001.Count, (Format-Hits $bad1001))

    # C13：引擎内无落库 / 无广播（ALT-SUB-04 / P3：出口只走反向接口）
    $srcFiles = @(CollectFiles @("src") @(".cc", ".h") @())
    $dbHits = @(SearchHits $srcFiles `
        '(SQLite|sqlite3|mysql_|PQexec|nanodbc|drogon::|Drogon|broadcast\(|EventHub|eventhub|WebSocket|websocket)' `
        @('MUST NOT'))
    Check "C13" "ALT-SUB-04 / P3：引擎内无落库 / 无广播 / 无 Web 框架调用" ($dbHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $dbHits.Count, (Format-Hits $dbHits))

    # C13b：引擎内无网络代码（ALT-NFR-01：不收包、不订阅）
    $netKeywords = @('sock' + 'et(' , 'recv' + 'from' , 'bin' + 'd(' , 'conn' + 'ect(' ,
                     'WSASock' + 'et' , 'multicast' , 'subscribe' + '(')
    $netHits = @(SearchHits $linkFiles (($netKeywords | ForEach-Object { [regex]::Escape($_) }) -join '|') @(
        'netKeywords', 'MUST NOT', '零命中', '检索范围'))
    Check "C13b" "ALT-NFR-01：引擎内无网络代码（不订阅、不收包）" ($netHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $netHits.Count, (Format-Hits $netHits))

    # §7 第 2 行的结论：引擎产物内业务词与阈值零命中（合法住所 = 规则包）
    $s07_02_ok = ($bizHits.Count -eq 0) -and ($policyBizHits.Count -gt 0) -and
                 ($levelHits.Count -eq 0) -and ($demoHits.Count -eq 0)
    Check "S07-02" "全仓检索具体阈值与业务规则 → 零命中" $s07_02_ok `
        ("引擎产物：业务词 {0} 处 / 级别字面量 {1} 处 / 阈值 {2} 处；规则包（合法住所）业务词 {3} 处" -f `
            $bizHits.Count, $levelHits.Count, $demoHits.Count, $policyBizHits.Count)

    # ================================================================ ⑤ 反向接口与入口纪律
    Section "⑤ 反向接口纪律：IAlertStore / IAlertSink / IClock / ILogSink 由宿主注入（P8/P9）"

    $headerPath = Join-Path $script:Repo "include\alert_engine\alert_engine.h"
    $headerOk = Test-Path $headerPath
    $headerText = ""
    if ($headerOk) { $headerText = [System.IO.File]::ReadAllText($headerPath) }

    $hasSink = ($headerText -match 'class\s+IAlertSink') -and
               ($headerText -match 'virtual\s+void\s+onAlertRaised') -and
               ($headerText -match 'virtual\s+void\s+onAlertUpdated') -and
               ($headerText -match 'virtual\s+void\s+onAlertAcked')
    $hasStore = ($headerText -match 'class\s+IAlertStore') -and
                ($headerText -match 'virtual\s+bool\s+save') -and
                ($headerText -match 'virtual\s+bool\s+load')
    $hasClock = ($headerText -match 'class\s+IClock') -and ($headerText -match 'virtual\s+int64_t\s+nowMs')
    $hasLog = ($headerText -match 'class\s+ILogSink') -and ($headerText -match 'virtual\s+void\s+log') -and
              ($headerText -match 'virtual\s+void\s+commandAudit')
    Check "C14" "公开头声明四个反向接口 IAlertSink / IAlertStore / IClock / ILogSink" `
        ($hasSink -and $hasStore -and $hasClock -and $hasLog) `
        ("sink={0} store={1} clock={2} log={3}" -f $hasSink, $hasStore, $hasClock, $hasLog)

    $injectedAll = ($headerText -match 'std::shared_ptr<IAlertStore>\s+store') -and
                   ($headerText -match 'std::shared_ptr<IClock>\s+clock') -and
                   ($headerText -match 'std::shared_ptr<IAlertSink>\s+sink') -and
                   ($headerText -match 'std::shared_ptr<ILogSink>\s+log')
    Check "C15" "四个出口全部经 AlertEngineOptions 注入（无内建实现、无全局单例）" $injectedAll `
        "AlertEngineOptions{store,clock,sink,log} 四个可空 shared_ptr"

    # C16：唯一公开头
    $publicHeaders = @()
    $includeDir = Join-Path $script:Repo "include"
    if (Test-Path $includeDir) {
        $publicHeaders = @(Get-ChildItem $includeDir -Recurse -File -Filter *.h |
            ForEach-Object { Rel $_.FullName })
    }
    Check "C16" "唯一公开头 include/alert_engine/alert_engine.h（P1）" `
        (($publicHeaders.Count -eq 1) -and
         ($publicHeaders[0] -eq "include\alert_engine\alert_engine.h")) `
        ("公开头：{0}" -f ($publicHeaders -join ", "))

    $internalH = Join-Path $script:Repo "src\internal.h"
    $internalGuarded = $false
    if (Test-Path $internalH) {
        $t = [System.IO.File]::ReadAllText($internalH)
        $internalGuarded = ($t -match '宿主 MUST NOT 包含')
    }
    Check "C17" "内部头 src/internal.h 明确标注宿主不可包含" $internalGuarded `
        "src/internal.h 头部注明'MUST NOT 包含本文件'"

    # C18：协议 §4.4 事件负载 —— 引擎只产出 data 与 ts（信封由宿主包）
    $evFields = @("alertId", "ruleId", "level", "entityId", "missionId", "firstAt", "count",
                  "lastAt", "upgraded", "state", "operatorId", "at")
    $evMissing = @()
    foreach ($f in $evFields) { if ($headerText -notmatch ("\b" + $f + "\b")) { $evMissing += $f } }
    Check "C18" "protocol §4.4：alert.raised / alert.updated / alert.acked 的契约字段齐备" `
        ($evMissing.Count -eq 0) ("缺字段：{0}" -f (($evMissing -join ", ") -replace '^$', '无'))

    # C19：cond 类型声明齐全（阈值 / 持续 / 组合 / 恢复 —— ALT-RULE-02）
    $condOk = ($headerText -match 'Threshold') -and ($headerText -match 'Duration') -and
              ($headerText -match 'Bool') -and ($headerText -match 'Recovery')
    Check "C19" "ALT-RULE-02：四类条件类型在公开面齐全（阈值/持续/组合/恢复）" $condOk `
        "ConditionType{Threshold,Duration,Bool,Recovery}"

    # C20：计数口径显式（ALT-GEN-05）
    $countsOk = ($headerText -match 'struct\s+AlertCounts') -and
                ($headerText -match 'rawRaises') -and ($headerText -match 'alertCount') -and
                ($headerText -match 'basis')
    Check "C20" "ALT-GEN-05：计数口径显式（原始条数 / 去重后条数 / basis 声明）" $countsOk `
        "AlertCounts{rawRaises, alertCount, basis}"

    # C21：台账查询超限截断字段（ALT-SUB-01：不静默丢）
    $truncOk = ($headerText -match 'bool\s+truncated') -and ($headerText -match 'int\s+omitted') -and
               ($headerText -match 'truncationReason')
    Check "C21" "ALT-SUB-01：台账查询超限截断如实上报（truncated / omitted / 原因）" $truncOk `
        "AlertQueryResult{truncated, omitted, truncationReason}"

    # C22：风暴保护如实上报被折叠条数（ALT-DEDUP-06）
    $foldOk = ($headerText -match 'foldedCount') -and ($headerText -match 'int64_t\s+folded')
    Check "C22" "ALT-DEDUP-06：被折叠条数上报（记录 foldedCount + 计数 folded）" $foldOk `
        "AlertRecord.foldedCount / AlertCounts.folded / Metrics.folded"

    # ================================================================ ⑥ 规则包（protocol.md §5）
    Section "⑥ 规则包（protocol.md §5：policiesNamespace / schemaVersion / kind / items + 兄弟段）"

    $policyDir = Join-Path $script:Repo "policies\mapapp"
    $neededPolicies = @("alertRules.json")
    $absentPolicies = @()
    foreach ($f in $neededPolicies) {
        if (-not (Test-Path (Join-Path $policyDir $f))) { $absentPolicies += $f }
    }
    Check "C23" "规则包齐全 policies/mapapp/alertRules.json" `
        ($absentPolicies.Count -eq 0) ("缺失：{0}" -f (($absentPolicies -join ", ") -replace '^$', '无'))

    $schemaOk = $true
    $schemaDetail = New-Object System.Collections.Generic.List[string]
    $packDoc = $null
    foreach ($f in $neededPolicies) {
        $full = Join-Path $policyDir $f
        if (-not (Test-Path $full)) { $schemaOk = $false; continue }
        try {
            $doc = [System.IO.File]::ReadAllText($full, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
            if ($f -eq "alertRules.json") { $packDoc = $doc }
        } catch {
            $schemaOk = $false
            $schemaDetail.Add(("{0}：非法 JSON" -f $f))
            continue
        }
        $hasNs = ($null -ne $doc.policiesNamespace) -and ($doc.policiesNamespace -ne "")
        $hasVer = ($null -ne $doc.schemaVersion) -and ($doc.schemaVersion -match '^\d+\.\d+\.\d+$')
        $hasKind = ($null -ne $doc.kind) -and ($doc.kind -eq "alertRules")
        $hasItems = ($null -ne $doc.items)
        if (-not ($hasNs -and $hasVer -and $hasKind -and $hasItems)) {
            $schemaOk = $false
            $schemaDetail.Add(("{0}：ns={1} ver={2} kind={3} items={4}" -f $f, $hasNs, $hasVer,
                               $hasKind, $hasItems))
        }
    }
    Check "C24" "规则包骨架：policiesNamespace + schemaVersion(MAJOR.MINOR.PATCH) + kind=alertRules + items" `
        $schemaOk ("问题：{0}" -f (($schemaDetail -join "；") -replace '^$', '无'))

    # C25：级别目录可扩展（>=2 级且 rank 唯一、非降序）—— ALT-GEN-02
    $levelOk = $false
    $levelDetail = "规则包未解析"
    if ($null -ne $packDoc) {
        $levels = @($packDoc.levels)
        $ranks = @($levels | ForEach-Object { [int]$_.rank })
        $uniq = (@($ranks | Sort-Object -Unique).Count -eq $ranks.Count)
        $levelOk = ($levels.Count -ge 2) -and $uniq
        $levelDetail = ("级别 {0} 个：{1}（rank 唯一={2}）" -f $levels.Count,
                        (($levels | ForEach-Object { $_.key }) -join "/"), $uniq)
    }
    Check "C25" "ALT-GEN-02：级别目录在规则包里且位次唯一（取值可扩展）" $levelOk $levelDetail

    # C26：每条规则都带 level / condition / dedupWindowMs（规则由数据声明 —— ALT-RULE-01）
    $ruleOk = $false
    $ruleDetail = "规则包未解析"
    if ($null -ne $packDoc) {
        $bad = New-Object System.Collections.Generic.List[string]
        foreach ($r in @($packDoc.items)) {
            if (($null -eq $r.level) -or ($null -eq $r.condition) -or ($null -eq $r.dedupWindowMs)) {
                $bad.Add($r.key)
            }
        }
        $ruleOk = ($bad.Count -eq 0) -and (@($packDoc.items).Count -ge 5)
        $ruleDetail = ("规则 {0} 条；缺字段的：{1}" -f @($packDoc.items).Count,
                       (($bad -join ", ") -replace '^$', '无'))
    }
    Check "C26" "ALT-RULE-01：每条规则都声明 level + condition + dedupWindowMs（规则是数据）" `
        $ruleOk $ruleDetail

    # C27：去重窗口与合并策略口径在规则包里显式（ALT-DEDUP-01）
    $mergeOk = $false
    $mergeDetail = "规则包未解析"
    if ($null -ne $packDoc) {
        $windows = @($packDoc.items | ForEach-Object { $_.dedupWindowMs } | Sort-Object -Unique)
        $mergeOk = ($windows.Count -ge 2)  # 至少两种不同窗口 = 按规则分别配置
        $mergeDetail = ("去重窗口取值：{0}" -f (($windows | ForEach-Object { $_.ToString() }) -join " / "))
    }
    Check "C27" "ALT-DEDUP-01：去重窗口按规则分别配置（可配而非全局常量）" $mergeOk $mergeDetail

    # C28：抑制关系与风暴保护在规则包里声明（ALT-DEDUP-05/06）
    $supOk = $false
    $supDetail = "规则包未解析"
    if ($null -ne $packDoc) {
        $parents = @($packDoc.items | Where-Object { $null -ne $_.parent })
        $hasStorm = ($null -ne $packDoc.storm) -and ($packDoc.storm.enabled -eq $true)
        $hasMute = ($null -ne $packDoc.mute) -and ($null -ne $packDoc.mute.defaultMs)
        $supOk = ($parents.Count -ge 1) -and $hasStorm -and $hasMute
        $supDetail = ("声明 parent 的规则 {0} 条；风暴保护={1}；消音缺省={2} ms" -f `
            $parents.Count, $hasStorm, $(if ($hasMute) { $packDoc.mute.defaultMs } else { "未声明" }))
    }
    Check "C28" "ALT-DEDUP-05/06 + ALT-ACK-04：抑制表 / 风暴保护 / 消音缺省均由规则包声明" `
        $supOk $supDetail

    # ================================================================ ⑦ 独立交付
    Section "⑦ 独立交付（ALT-NFR-05：独立构建 / 示例 / 测试 / 验收脚本 / 规则包 / 单头回落）"

    $needed = @("CMakeLists.txt", "include\alert_engine\alert_engine.h", "src\internal.h",
                "tests\CMakeLists.txt", "tests\selftest.cc", "examples\CMakeLists.txt",
                "examples\minimal\main.cc", "examples\full_flow\main.cc",
                "scripts\acceptance.ps1", "policies\mapapp\alertRules.json", "README.md", "LICENSE",
                "third_party\nlohmann\json.hpp")
    $absent = @()
    foreach ($f in $needed) { if (-not (Test-Path (Join-Path $script:Repo $f))) { $absent += $f } }
    Check "C29" "独立交付要件齐全（构建 / 公开头 / 测试 / 示例 / 验收脚本 / 规则包 / 单头回落）" `
        ($absent.Count -eq 0) ("缺失：{0}" -f (($absent -join ", ") -replace '^$', '无'))

    # C30：nlohmann/json 内置单头回落可用（空环境构建的前提）
    $fallback = Join-Path $script:Repo "third_party\nlohmann\json.hpp"
    $fallbackOk = (Test-Path $fallback) -and ((Get-Item $fallback).Length -gt 100000)
    Check "C30" "ALT-NFR-01：内置单头回落 third_party/nlohmann/json.hpp 可用（空环境可构建）" `
        $fallbackOk ("大小 {0} 字节" -f $(if (Test-Path $fallback) { (Get-Item $fallback).Length } else { 0 }))

    $exampleCodes = @{}
    foreach ($name in @("example_minimal", "example_full_flow")) {
        $exe = Join-Path $bin "$name.exe"
        if (-not (Test-Path $exe)) { $exe = Join-Path $bin $name }
        if (-not (Test-Path $exe)) { $exampleCodes[$name] = -1; continue }
        $outPath = Join-Path ([System.IO.Path]::GetTempPath()) ("alert-engine-{0}-{1}.txt" -f $name, $PID)
        & $exe > $outPath 2>&1
        $exampleCodes[$name] = $LASTEXITCODE
    }
    $exOk = $true
    foreach ($k in $exampleCodes.Keys) { if ($exampleCodes[$k] -ne 0) { $exOk = $false } }
    Check "C31" "两个示例独立运行退出码 0" $exOk `
        (($exampleCodes.Keys | Sort-Object | ForEach-Object { "{0}={1}" -f $_, $exampleCodes[$_] }) -join " ")

    # C32：ctest（可选的目标，缺 ctest 不判失败）
    $ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
    if ($ctest) {
        $ctestOut = (& $ctest --test-dir $buildPath -C $Config --output-on-failure 2>&1 | Out-String)
        $ctestOk = ($LASTEXITCODE -eq 0)
        Check "C32" "ctest 注册的用例全部通过" $ctestOk ((($ctestOut -split "`n") |
            Where-Object { $_ -match 'tests passed|tests failed|Total Test time' }) -join " ")
    } else {
        Check "C32" "ctest 注册的用例全部通过（跳过：未找到 ctest）" $true "ctest 不在 PATH，视为不适用"
    }

    # C33：验收脚本自身可机检：以退出码 0/1 结束
    $scriptText = [System.IO.File]::ReadAllText((Join-Path $script:Repo "scripts\acceptance.ps1"))
    $exitOk = ($scriptText -match '(?m)^\s*exit\s+1') -and ($scriptText -match '(?m)^\s*exit\s+0')
    Check "C33" "验收脚本以退出码 0/1 结束（ALT-NFR-05）" $exitOk `
        "脚本内含 exit 0 与 exit 1 两条收口路径"

    # ================================================================ 汇总
    Section "汇总"

    $total = $script:Results.Count
    $passed = 0
    foreach ($r in $script:Results) { if ($r.Ok) { $passed++ } }
    $failed = $total - $passed

    if ($jsonOk) {
        Write-Host ("selftest ：用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed)
    }
    Write-Host ("验收检查：{0} 项｜PASS {1}｜FAIL {2}" -f $total, $passed, $failed)

    if ($failed -eq 0) {
        Write-Host ""
        Write-Host "验收通过（退出码 0）" -ForegroundColor Green
        exit 0
    }
    Write-Host ""
    Write-Host "验收失败（退出码 1）：" -ForegroundColor Red
    foreach ($r in $script:Results) {
        if (-not $r.Ok) { Write-Host ("  - {0} {1}：{2}" -f $r.Id, $r.Title, $r.Detail) -ForegroundColor Red }
    }
    exit 1
} finally {
    Pop-Location
}
