# 实例内存保护器实现计划

> **For agentic workers:** 使用 executing-plans 技能按任务在当前会话逐项实现和检查；实现按序执行；独立代码审查按审查技能执行。步骤使用复选框跟踪。

**Goal:** 在 PG18 中实现低依赖实例内存保护，按 cgroup 压力安全取消或终止普通用户会话，降低实例 OOM 风险。

**Architecture:** 专用无数据库连接 background worker 采集压力、选择目标和跟踪动作；共享槽保存代次和请求，目标进程在 PG 安全中断点验证请求并处理。OS 私有内存只辅助排序，效果用 cgroup 新样本判断。

**Tech Stack:** PostgreSQL 18 C、PG background worker/ProcSignal/LWLock、Linux cgroup v2/procfs、PG 回归与 TAP 测试；macOS 仅运行 fake-provider 测试。

## 2026-09-29 动作补丁交付记录

本次继续实现进程选择、查询取消和连接终止；下方原始清单保留完整首版范围，未勾选项不等于本次均未实现，以本段记录为准。

- 注册普通客户端的实例启动代次、共享槽代次、PID、语句序号；初始 superuser 会话保护。复制、autovacuum、系统后台进程及 SAL 不作为目标。
- 并行 leader 将身份写入 FixedParallelState；worker 按会话和语句代次校验后注册为受保护成员，仅 leader 接收动作。私有内存按 Linux smaps_rollup 的 Private_Clean/Dirty/Hugetlb 近似，不相加 RSS；读取前后核验 /proc/PID/stat 启动时间及共享槽身份。
- 每轮轮转精查最多 8 个客户端和 8 个并行 worker。worker 配额公平分给当轮 leader，各 leader 独立轮转 worker。结果是部分采样的保守估计，不保证看见所有 worker 内存，也不是释放量承诺。
- 排序优先达到最小收益门槛的真正 idle；idle in transaction 不享受低影响优先级，再比较私有内存。原型最小门槛 max(64 MiB, A/16)。未知、过期及在途候选不选。
- 新增 PROCSIG_MEMORY_PROTECTION；信号处理只置位和唤醒，ProcessInterrupts 安全点验证身份、模式、时效和请求。取消返回 57014，终止返回 57P01，沿用 PG 错误/退出及并行清理流程，无 SIGKILL。
- 取消和取消后升级终止都绑定原语句；独立选择的连接终止绑定会话。语句切换和撤销使请求 serial 失效；安全适配 hook 返回后再次核验，防止旧请求误伤下一条查询或已撤销请求继续执行。
- enforce 控制器每次仅保留一个在途动作；普通压力先取消，宽限 200 ms 后若仍为同一语句可升级，临界压力可跳过宽限。请求寿命 1 s；三次退出无净下降冷却 1 s。每次重新读取 cgroup 判断效果。实际执行前重新核验新鲜、可信压力，过期采样不行动。
- native collector 仅在本 cgroup 有有限上限、真正层级根可见且可见祖先均无上限时允许自动动作；受限祖先、隐藏根和非 Linux 保持无动作。部署仍须保证专用实例 cgroup，不能由此证明节点级内存风险已消除。
- 状态接口新增 boundary_trusted、cancel_requests、terminate_requests；计数是接受的请求数，不是成功动作数或释放字节。保留动作/无进展日志；完整事件环及 SRF 尚未实现。
- Taurus 只预留 memory_protection_action_safe_hook；本仓库没有 SAL 协议实现。接入及取消、断连、提交边界认证前必须保持 observe。当前不改变 jemalloc 配置，不做 SAL/IPC 分桶预算。
- 实施调整：先采用单在途动作，比原计划两个更保守；拒绝不可信边界表现为状态可观测但不自动动作，而非拒绝保存 enforce 配置。observe 当前不生成模拟选择事件。
- 测试包含 SQL 确定性规则/解析/排序回归以及 Python stdlib + libpq 临时集群集成，无需 IPC::Run。集成覆盖 simple/extended 取消、失败事务恢复、代次/语句过期请求、延迟动作过期、升级请求跨语句、off 撤销、管理会话保护、idle/active 终止、并行 worker 不单独终止及整体清理。
- 构建问题记录：头文件结构调整后一度误用了旧测试扩展二进制，临时集群崩溃；清理重编后集成通过，并增加该测试对象的头文件依赖，防止复用旧 ABI。未影响用户数据库。
- 尚未完成首版检查点 B/C/D：真实 Linux 自动压力闭环及无进展/重启故障矩阵、事件环、Meson/Windows、Taurus 实际 SAL 清理认证。macOS 动作注入测试不能替代这些验收。
- 构建依赖修复：新增 ProcSignal 枚举后，未启用依赖追踪的旧对象曾使并行长错误消息测试卡住；已完整 clean 并以 --enable-depend 重配，之后核心 231 项全部通过。动作集成另加入长并行错误消息测试并通过。后续开发沿用 --enable-depend。
- 最终验证：macOS cassert 完整清理重编无编译警告；模块 SQL 回归 1/1、PG 核心回归 231/231、libpq 动作集成均通过；Linux 条件分支语法检查、pgindent 和 git diff --check 通过。独立审查发现的撤销竞态、升级请求跨语句、worker 采样饥饿及宽限计时问题均已修复并复查，无剩余具体 P1/P2 发现。
- off 仍维护会话注册/语句身份并占用一个 worker 名额；高并发下的共享锁与 procfs 开销尚待 Linux 压测。部分 worker 采样可能低估收益，使某些大并行任务暂不入选。
- 第二版全局记账、请求管理及排队不进入本次补丁。未提交、合并或部署。

## 2026-09-28 实际交付记录

当前交付为第一版的观测基础补丁，不代表检查点 A 的全部内容或第一版完成。

- 已实现：纯压力状态机与边界测试；cgroup 数字、membership、mountinfo 解析；Linux 本 cgroup 只读采样；无数据库连接 observer；off/observe 重载；共享状态与有权限控制的 pg_memory_protection_status()；重启测试；配置文档。
- 未实现：祖先/平台有效边界认证、进程私有内存采样及候选排序、会话/语句代次、动作槽和事件环、安全取消/终止、Taurus IPC 认证。enforce 明确不提供。
- 实施调整：语句注册和身份钩子移到动作补丁一起实现；采集器当前的有效样本仅代表 local_cgroup_only，不能作为后续 enforce 的可信实例边界。新增 memory_observer.c 将纯策略与后台生命周期分开。
- 默认 off；observer 仍占一个 max_worker_processes 名额以支持重载。没有可用名额时状态保持 observer_not_started；headroom 默认 0，关闭分类。
- 验证：macOS cassert 构建成功；模块 SQL 回归通过；PG 核心 231 项回归通过；Linux 条件编译分支语法检查通过；源码按 pgindent 格式化。独立只读审查无 P1/P2 发现。
- 未验证：真实 Linux cgroup 运行、Meson 构建、Windows 构建和 Taurus。当前缺少 IPC::Run，生命周期验证采用 SQL 回归，没有将未执行的 TAP 标为通过。
- 本地测试环境：/private/tmp/pg-memory-protector-build；安装路径 /private/tmp/pg-memory-protector-install。基线首次回归受共享内存沙箱及 macOS 动态库路径影响，改用独立临时安装后完成上述验证。
- 新增系统接口已更新开发目录版本；测试须初始化新集群，不使用既有生产数据目录。尚未提交、合并或部署。

## 全局约束

- 规范：../specs/2026-09-28-instance-memory-protector-design.md；本文件已开始执行，实际进度见下方记录。
- 第一版无预算、授信、排队或精细分配记账；第二版独立推进全局记账和请求管理。
- PG 与 SAL 同 cgroup；不重复扣减 SAL/IPC。jemalloc 5.2.1 现有配置不变；不依赖 stats 或主动 purge。
- 只处理已认证普通用户 backend，不直接处理 parallel worker、复制/后台进程或 SAL。
- 不用 SIGKILL；不能强行突破 PG 或 Taurus 中断延迟/临界区。
- observe/off 无动作，未知/过期数据无新动作，所有队列/事件/共享槽有界。
- Taurus enforce 必须先完成现有 SAL 取消/断连清理认证；社区通过不等于 Taurus 通过。
- 所有阈值原型值来自规范第 4 节，不是认证生产默认值。
- 开始编码前创建隔离 worktree，将本计划和规范复制进去；不覆盖当前工作区用户修改。当前阶段只有文档。

## 文件结构

新建：

- src/include/postmaster/memory_protector.h：公用类型、共享槽和通知接口。
- src/backend/postmaster/memory_protector.c：worker 主循环、策略、动作和有界事件。
- src/backend/postmaster/memory_pressure.c：cgroup/procfs 解析、平台采样和候选内存读取。
- src/backend/utils/adt/memory_protectorfuncs.c：只读状态/事件 SRF。
- src/test/modules/test_memory_protector/：测试扩展、回归、TAP、测试专用采样注入。
- doc/src/sgml/memory-protection.sgml：范围、配置、错误行为和操作说明。

修改既有文件：postmaster/{Makefile,meson.build,bgworker.c,postmaster.c}；storage/ipc/{ipci.c,procsignal.c}；include/storage/procsignal.h；tcop/postgres.c；utils/init/postinit.c；utils/misc/guc_tables.c；utils/adt/{Makefile,meson.build}；include/catalog/pg_proc.dat；backend/catalog/system_views.sql；test/modules/{Makefile,meson.build}；doc/src/sgml/{filelist.sgml,config.sgml,monitoring.sgml}。按实际是否需要新增源文件引用调整，禁止无关重构。

## 1. 公用类型、纯策略和确定性测试

**产物：** 可独立验证的压力状态转换，无真实采样、无发送动作。

核心接口在 memory_protector.h 中先定义：

```c
typedef enum MPState { MP_NORMAL, MP_PRESSURE, MP_CRITICAL, MP_STALE } MPState;
typedef enum MPAction { MP_NONE, MP_CANCEL, MP_TERMINATE } MPAction;
typedef struct MPIdentity
{
    uint64 instance_epoch;
    uint64 slot_generation;
    uint64 statement_sequence;
    ProcNumber proc_number;
    pid_t pid;
} MPIdentity;
typedef struct MPPressureSample
{
    uint64 limit_bytes;
    uint64 current_bytes;
    uint64 sampled_at_ms; /* monotonic */
    uint64 config_generation;
    bool valid;
} MPPressureSample;
typedef struct MPPolicy
{
    uint64 headroom_bytes;
    uint32 sample_interval_ms;
    uint32 sample_max_age_ms;
    uint32 cancel_grace_ms;
    uint32 exit_wait_ms;
    uint32 max_inflight;
} MPPolicy;
typedef struct MPController MPController;
extern MPState MPAdvance(MPController *ctl, const MPPressureSample *sample,
                         const MPPolicy *policy, uint64 now_ms);
```

- [ ] 测试扩展加入两种构建系统，创建 `test_memory_protector.control`、`test_memory_protector--1.0.sql`、`test_memory_protector.c`、`sql/test_memory_protector.sql` 和对应 expected 文件。SQL 入口 `test_mp_policy() RETURNS boolean` 内部使用显式失败报告，不依赖发布构建关闭后的 Assert。
- [ ] 先实现测试向量并确认缺少被测实现时构建/测试失败：L=32 GiB、A=4 GiB；U=27 正常；U=28 两个样本压力；U=30 一个样本临界；U=26 三个样本恢复；超过 300 ms 为 stale；U>L 不下溢；A=0 或 1.5A>=L 拒绝。
- [ ] 实现 MPController 的状态、连续样本计数和配置代次；更换配置清空连续计数，时间使用单调来源，禁止依赖墙钟倒退。
- [ ] 加测试：重复相同采样序号不算多个连续样本；这要求 sample 增加单调 sample_sequence 字段，测试和 provider 同步采用。
- [ ] 运行模块回归并检查只改变输入数据才能推进状态，无后台线程或全局副作用。

示例验证调用：

```sql
CREATE EXTENSION test_memory_protector;
SELECT test_mp_policy(); -- t；任何向量不符必须 ERROR
```

## 2. Linux 采集与非 Linux 降级

**接口：** memory_pressure.c 提供 `bool MPReadPressure(MPPressureSample *out)`、`bool MPReadCandidateMemory(pid_t pid, uint64 expected_start_ticks, uint64 *private_bytes)`；失败必须清除 valid，不能返回上一轮成功结果。

- [ ] 增加 `test_mp_pressure_parser() RETURNS boolean`，用内存字符串夹具测试 `/proc/self/cgroup`、mountinfo、memory.max/current/stat 的解析。测试空白、max、超 64 位、缺失字段、路径转义、非根 cgroup mount、读取截断。
- [ ] 解析器与文件 IO 分离；生产目录来自实际进程 cgroup 和 mountinfo，不能硬编码 /sys/fs/cgroup 下某个实例路径。
- [ ] 采集路径/限额改变时重新验证祖先限制，返回解释性失败原因；默认不将共享祖先限额视为实例独占。没有可信平台边界时 observe。
- [ ] 使用小型固定缓冲流式读取文件，设置长度上限；Linux 外返回 unsupported，不伪造有效样本。
- [ ] 对候选先检查 /proc/PID/stat 的启动时间，再采集内存并复核身份；测试 PID 变化导致结果作废。RssAnon 只初筛，smaps_rollup 私有项用于有限候选，权限不足返回 unavailable。
- [ ] 运行解析回归；真实本机只做读取检查，不调整开发机 cgroup 限额。

测试夹具例子：

```text
memory.max="34359738368\n", memory.current="30064771072\n" → valid
memory.max="max\n", no platform bound → unsupported enforce
memory.current="18446744073709551616\n" → invalid overflow
sample path changed between reads → invalid; rediscover next round
```

## 3. 共享注册、身份和只观察 worker

**接口：** `MPShmemSize/MPShmemInit`；`MPRegisterBackend/MPUnregisterBackend`；`MPBeginStatement/MPEndStatement`；`MemoryProtectorMain(Datum)`。注册使用 ProcNumber 槽，实例代次与槽代次均在共享状态初始化/复用时更新。

- [ ] 创建共享区：每个 backend 槽一条身份/状态、一条动作、256 条事件环、两个在途槽；使用 checked size arithmetic。短锁内仅复制/更新，不读取 procfs 或发送信号。
- [ ] postinit.c 在角色与认证完成后注册允许的 backend，退出回调注销。平台保护标记只由可信初始化接口设置；初始社区 profile 保护超级用户连接作为保守策略，普通用户不可自行声明保护。
- [ ] postgres.c 按实际执行边界发布语句序号：简单协议每条 RawStmt、扩展协议 Parse/Bind/Execute 的独立工作段；COPY 与相应执行段关联。空闲时标记无活跃语句。错误长跳转清除活跃状态，不遗留旧序号。
- [ ] 将 MemoryProtectorMain 加入 bgworker.c 的 InternalBGWorkers，注册位置审计 RegisterBackgroundWorker 的允许阶段。使用 BGWORKER_SHMEM_ACCESS，不使用数据库连接标志；与恢复状态兼容，从 ConsistentState 开始。
- [ ] 为支持运行时 off/observe/enforce 切换，启动时注册一个固定 worker，off 下低频等待并处理重载。占用一个 max_worker_processes 名额必须文档化；注册失败不能静默宣称有保护。
- [ ] 实现 SIGHUP/SIGTERM、Latch 等待、postmaster 死亡检查、心跳与配置代次；不做 SQL 访问。
- [ ] TAP `001_observe.pl`：普通连接注册/退出；管理连接保护；worker 重启后槽仍可信；off/observe 均零真实动作；重复连接不增大共享区。

此任务交付可运行 observe。此时 enforce 必须报未就绪，不能空开关。

## 4. 目标排序、动作去重与有限循环

**接口：** `bool MPSelectVictim(MPIdentity *out, MPAction *action)`、`bool MPQueueAction(const MPIdentity *id, MPAction action, uint64 action_id)`。选择和入队间重新核验槽代次/状态；不在一个锁中执行昂贵 OS 读取。

- [ ] 添加 `test_mp_selector()`：大内存真正 idle 优先；idle in transaction 不走低影响优先级；小于最小收益阈值不入选；所有系统角色和保护标记排除；无可信指标返回无候选。
- [ ] 实现低频全量便宜快照和每轮有限精查。原型每轮最多精查 8 个候选；扫描游标防止后部连接永久不被检查；指标最大年龄与样本时效分别记录。
- [ ] 用可信并行 leader 关联汇总 worker 指标，无法确认关联不归并；仅 leader 入选，已退出 leader 不映射到复用 PID。
- [ ] 实现两个在途槽和动作 ID 去重；PRESSURE 一次一个，CRITICAL 最多两个；目标已在途不重发。
- [ ] 添加确定性测试：三次退出无净下降触发 1 s 冷却；STALE 不新增；没有候选只记录原因；临界状态不等待取消宽限；恢复后立即停止新动作。
- [ ] observe 使用相同选择代码生成模拟事件，但不写可执行动作槽，避免切换 enforce 后误执行旧模拟请求。

## 5. 后端安全取消/终止

**接口：** `MPHandleInterrupt()` 只设置 sig_atomic_t pending、InterruptPending 和唤醒；`MPProcessInterrupt()` 在主执行上下文取请求、验证并处理；返回值表示未处理/已拒绝/已消费。信号原因为新增 PROCSIG_MEMORY_PROTECTION，更新 NUM_PROCSIGNALS 边界。

- [ ] 测试扩展提供仅测试构建可用的定向动作注入接口，普通用户无 EXECUTE 权限；生产不得暴露任意压力伪造入口。
- [ ] 在 procsignal.c 分发通知，不在信号 handler 获取 LWLock、读 /proc、malloc 或 ereport。
- [ ] 在 ProcessInterrupts 的安全区域处理：遵守 InterruptHoldoffCount/CritSectionCount；取消额外遵守 QueryCancelHoldoffCount。不可处理时保留请求并确保后续安全点继续检查，不自旋。
- [ ] 实际执行取消的那一刻重新比较 statement_sequence；不先把旧动作无条件转成 QueryCancelPending。终止验证会话代次，取消验证会话和语句代次。
- [ ] 复用既有 LockErrorCleanup、错误恢复和 FATAL 退出语义，增加明确原因；取消 57014，终止 57P01。普通用户取消/终止优先级与本功能交互必须测试，不能篡改其他信号原因。
- [ ] TAP `002_actions.pl`：pg_sleep 取消后同连接 SELECT 1 成功；事务内取消后处于失败事务，ROLLBACK 可恢复；终止后连接断开，其他连接可用且 postmaster 未重启。
- [ ] TAP `003_identity.pl` 使用同步屏障而非随机 sleep：延迟取消时结束旧语句并启动新语句，新语句不被旧请求取消；连接断开及槽复用后旧终止无效；holdoff 解除后才处理。
- [ ] 并行任务取消测试等待 leader 与 workers 正常清理；不中断独立系统后台进程。

## 6. 闭环集成、参数和诊断

- [ ] 在 guc_tables.c 添加两个平台管理参数，mode 为 SIGHUP 枚举，headroom 使用带单位且范围校验的表示；若采用 int kB，显式文档化最大值并在换算时使用 uint64。非法 reload 保留上一有效配置并记录原因。
- [ ] 第一版社区 enforce 支持可信 Linux v2 边界；非 Linux/未知限制明确拒绝，fake provider 仅测试启用。Taurus 安全资格由构建/平台适配器给定，不由普通用户 GUC 绕过。
- [ ] 实现状态/事件 SRF 和系统视图：状态对监控角色可读；事件按权限隐藏目标身份，默认不含 SQL。目录 OID 使用仓库开发惯例分配并检查重复。
- [ ] 完整循环：采样→状态→选择→发布→确认处理/退出→新采样；worker 重启核对在途 action_id，不重放已消费动作。退出仅说明私有地址空间清理，不将其记为释放指定字节。
- [ ] TAP `004_controller.pl` 注入压力序列，验证从取消升级、旧语句结束后重新选取、采样过期、恢复、模式切换、no_progress 和 worker 重启。
- [ ] 建立 `memory-protection.sgml` 并从文档包含链引用；说明占用一个 worker 名额、off 的后台监控开销、连接断开及提交结果可能不确定、参数原型和第二版边界。

## 7. 隔离压力验收与 Taurus 认证

- [ ] 添加显式手动运行的 Linux 压测脚本/说明，要求操作者给定专用可委派 cgroup，拒绝根 cgroup 和没有有限 memory.max 的目标；不在默认回归中制造真实 OOM。
- [ ] 固定现有 jemalloc 配置，比较 off/observe/enforce；记录 cgroup memory.events 前后差值、PG 启动时间、客户端成功/取消/断连数、完成时间、CPU、P99、采样及退出延迟。不能只看最终 U。
- [ ] 覆盖大查询后 idle 残留、并发导数/建索引、并行任务、连接重试、限制下调、采集失败、页缓存高而 anon 低；每组预先填写吞吐和成功率门槛，缺少门槛不算通过。
- [ ] fake SAL 用于验证同域计费和背景增长无进展行为；它不能替代 Taurus 真实 IPC 生命周期认证。
- [ ] Taurus 在其仓库验证队列满、延迟完成、取消、断连与提交边界；现有安全协议未通过时维持 observe，记录阻塞原因，不宣称社区补丁自动兼容。
- [ ] 完成相关 PG 回归与构建验证后做代码评审；不要为了达标在测试中降低负载或把全部请求取消。

## 验证命令与环境

编码时在隔离 worktree 中建立独立构建目录；下列是 Autoconf 路线，不能在用户已有构建目录覆盖配置：

```sh
mkdir build-memory-protector
cd build-memory-protector
../configure --enable-cassert --enable-tap-tests
make -j4
make -C src/test/modules/test_memory_protector check
make check
```

若缺 Perl TAP 依赖，记录具体缺失，不把跳过当通过。Meson 路线在单独目录验证新源文件/测试模块已接入；按生成测试清单选择 test_memory_protector 对应测试，不能猜测不存在的测试名称。真实 Linux enforce、jemalloc 行为和 Taurus 认证不能由 macOS fake 测试替代。

## 开发检查点

A（任务 1–3）：只观察闭环；B（任务 4–6）：确定性测试下主动保护；C（任务 7）：社区 Linux 压力认证；D：Taurus 实际协议认证。每个检查点汇报实际测试和未覆盖项。第二版全局记账及请求管理不进入本计划。
