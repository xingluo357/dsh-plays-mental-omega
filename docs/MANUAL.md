# mo-pilot 操作手册

心灵终结（Mental Omega / Yuri's Revenge 引擎）**AI 代打**工具。由一个 32 位注入 DLL 直接操作游戏内存与对象方法来读战场、下命令，**不走截图、不发鼠标**，所以 token 便宜、延迟低。

> 一句话：**读状态用 `state/units/scan`，动作用 `move/attack/deploy/queue/place`，每次动手前重取索引。**

---

## 0. 给接手 AI 的 60 秒上手

```powershell
# 1) 游戏已经开着（进程 gamemd.exe），确认 PID
Get-Process gamemd | Select-Object Id

# 2) 编译（改过 src\mo_dll.cpp 才需要）
$d='mo-pilot'
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat`" >nul && cl /nologo /utf-8 /DNOMINMAX /LD /std:c++20 /EHsc /I`"$d\YRpp-master`" /Fo:`"$d\build\\`" /Fe:`"$d\build\mo_pilot_v61.dll`" `"$d\src\mo_dll.cpp`" /link ole32.lib"

# 3) 注入
& "$d\build\inject.exe" <PID> "$d\build\mo_pilot_v61.dll"

# 4) 说话（管道名 = dll 文件名，下划线换连字符，去掉 .dll）
& 'python' "$d\src\mo_cli.py" --pipe mo-pilot-v61 ping
& 'python' "$d\src\mo_cli.py" --pipe mo-pilot-v61 state
```

`state` 返回 `ok ready=1 frame=… money=… mine=…`，`ready=1` 就通了。

---

## 1. 目录与文件

| 路径 | 作用 |
|---|---|
| `src\mo_dll.cpp` | 全部逻辑：管道线程 + 所有命令。**唯一需要改的源文件** |
| `src\inject.cpp` | CreateRemoteThread + LoadLibraryA 注入器，参数 `<pid> <dll 全路径>` |
| `src\mo_cli.py` | 纯标准库命名管道客户端，`--pipe NAME` |
| `build\` | 编译产物（`mo_pilot_vNN.dll`、`inject.exe`） |
| `YRpp-master\` | YR++ 头文件（Ares 团队维护的 gamemd 结构体/地址），**已打过 3 处补丁才能被新 MSVC 编过**，别替换 |
| `tools\disasm.py` | capstone 反汇编器，按 VA 打印 gamemd.exe 的指令（PE 解析含 section 换算） |
| `tools\offs.cpp` | 编译期打印结构体偏移，用来核对 YR++ 布局 |

---

## 2. 能力边界（必须先知道）

**能做**
- 读：帧号、资金、我方单位/建筑、全部敌方目标、阵营列表
- 指挥：移动 / 集火攻击 / 守卫 / 停止 / 部署 MCV / 换使命
- 生产：**往游戏已在运行的工厂队列里追加**（`queue`）
- 建筑：把已造好但悬空的建筑**落地**（`place`），需要自己挑合法格子
- 逆向辅助：`probe`（结构体偏移核对）、`hookctor`（记录游戏建厂调用点）

**不能做（已确认，不要再撞）**
- **从零启动生产**。游戏的工厂只在「玩家点了侧边栏」时由游戏自己创建；手工 `new FactoryClass()`（哪怕正确调了 ctor）不会被游戏 tick，进度永远 0。
- 手工产出的单位是半成品（停在 limbo、坐标 0,0），给它下移动命令会 AV。**只用游戏自己产出来的兵**。
- 直接读侧边栏 cameo 状态（YR++ 的 `SidebarClass::Tabs` 布局与 MO 3.6 不符，偏移全 0）。

**因此推荐分工**：玩家负责点侧边栏（生产什么由他点一下），AI 负责排产追加、建筑落地、编队、行军、打仗。

---

## 3. 命令参考

调用格式：`mo_cli.py --pipe <管道> <命令> [参数...]`。所有命令都返回单行 `ok ...` 或 `err ...`。

### 3.1 读

| 命令 | 说明 |
|---|---|
| `ping` | 探活，返回 `ok pong` |
| `state` | `ok ready=1 frame=<帧> money=<资金> mine=<我方单位数>` |
| `units` | 每行 `idx\|TYPEID\|x\|y\|z\|hp\|group\|mission\|target`，**含我方全部**（建筑也在里面） |
| `scan` | 敌方目标：`idx\|TYPEID\|x=…\|y=…\|hp=…\|d=<到我方基地距离>\|bld=0/1\|owner=<阵营ID>`，已过滤中立民用建筑 |
| `houses` | `idx\|ID\|me=\|player=\|neutral=\|dead=\|bld=`，列全部阵营 |
| `house <idx>` | 该阵营的建筑/单位坐标与资金（用来核对「我是不是这个阵营」） |
| `canbuild` | 可造项：`id\|kind\|cost`（kind 列不可靠，别依赖） |
| `status [n]` | 各工厂进度：`id\|progress\|queued\|suspended\|onhold` |
| `diag` | 一堆内部诊断（工厂数组、主工厂指针、建筑坐标、待落地建筑） |
| `putinfo <gx> <gy>` | 某格能不能建：land/flags/content/height，并列出 limbo 中的待落地建筑 |
| `probe [sel]` | 结构体偏移核对：把我方单位里「值 == 我方 HouseClass*」的偏移全列出来，并 dump 各阵营 +0x60..+0x7C |

### 3.2 动

选择器 `sel` 语法（几乎所有动作命令都要它）：
- `all` —— 我方全部由玩家控制的单位
- `g:N` —— Ctrl+数字 编队 N（-1 表示未编队）
- `i:1,2,3` —— 直接给 `units`/`scan` 里的索引

| 命令 | 说明 |
|---|---|
| `move <sel> <gx> <gy>` | 行军到**网格坐标**（见第 4 节） |
| `attack <sel> <targetIdx>` | 集火 `scan` 里的目标索引 |
| `guard <sel>` / `stop <sel>` | 守卫 / 停止 |
| `deploy <sel>` | MCV 部署成建造厂（`QueueMission(Mission::Unload,true)`，实测唯一可行入口） |
| `mission <sel> <n>` | 直接设使命编号（调试用） |
| `queue <typeId> [n]` | **往正在运行的工厂队列追加生产**（安全，实测 q 40→42 且逐个消费） |
| `place <gx> <gy>` | 把 limbo 中的建筑放到该格（内部会 Put + Place(false) + 标记已落地 + 清工厂） |
| `hp <sel> <value>` | 直接把选中单位的血量写成 `value`（`hp i:12 0` = 秒杀，见第 12 节） |
| `spawn <gx> <gy>` | 把坐标 0,0 的我方单位挪到该格（救 limbo 用，慎用） |
| `drainq <typeId>` / `drainall` | 清队列（`Clear` + `AbandonProduction`），清掉玩家排的一堆兵用 |
| `resume` | 把本人工厂的 suspended/hold 全清掉 |
| `houses`/`house` | 见上 |

### 3.3 只给逆向用（日常别碰，会崩）

`newfactory` `fixfactory` `tryfac <absType> <cat>` `flag <0/1>` `prod <id> [n]` `qprod <id> [n]` `start` `finish` `tickf <n>` `tickdone <n>` `pump <n>` `aiprod` `loco <sel>` `unlimbo <sel>` `cameos` `linkcameo <id>` `hookctor` `unhook` `hookinfo` `primary` `whoami`

这些是排障历史留下的探针：`prod/qprod/tryfac/flag` 会导致**免费秒建、单位残缺、甚至 Fatal Error 崩溃**，除非你在复现某个 bug，否则不要调用。

---

## 4. 坐标系（最容易翻车的地方）

游戏里有两套坐标，**动作命令用的是「网格」**：

| 概念 | 换算 |
|---|---|
| 世界坐标（`units`/`scan`/`house` 里打印的 x,y） | 1 格 = 256 世界单位 |
| 网格坐标（`move`/`place` 参数） | `gx = 世界x / 256`，`gy = 世界y / 256` |
| 格心 | `CellClass::Cell2Coord(cell)` = 网格×256 + 128 |

例：敌方建造厂 `x=28416 y=16640` → 网格 `(111, 65)`。
**不要**把世界坐标直接当网格传，也不要用 MapClass 的 MapCoords 推算——之前在这个坑里扫了半小时无效格子。

找合法建筑格：`putinfo <gx> <gy>` 看成败，再 `place`。基地内部相邻空地通常可靠；把楼放进别人楼里会 `put=false`。

---

## 5. 认阵营（别信 diag，信钱）

1. `houses` 列出全部阵营
2. `house <idx>` 看该阵营资金
3. **和玩家屏幕上的资金数字对照**，对上的就是玩家
4. 本局实测：`house 0 = Chinese = 玩家`，`house 1 = USSR = 敌人`（AI 有几十万作弊资金），`house 2 = Neutral`（地图民用建筑，会污染 `scan`，已在代码里过滤）

`Me()` 取的是 `HouseClass::Player`，实测正确。`TechnoClass::Owner` 偏移 **0x21C**（`probe` 实测，YR++ 一致）。

> 关于「我方永远是 esi+0x6C」：在**这个** MO 3.6 构建里，`TechnoClass+0x6C` 是我方单位的 **Health**（`0x782`/`0x7D0` 这种值），不是房主指针；房主在 `+0x21C`。如果你在 CE 里看到某个函数用 `[esi+0x6C]` 直接和「我方」比较，那多半是**另一个结构**（例如某个列表项/临时结构），可以用 `probe` 扫一遍确认，不要直接套到 TechnoClass 上。

---

## 6. 生产：唯一可靠的路

**结论**：只有**游戏自己创建的**工厂会 tick、会扣钱。

```
游戏自建工厂 ✓  pct 8→11→14→…→54，money 同步减少
手工 new 工厂 ✗  无论如何调 Update()/StartProduction()/置 flag，pct 恒 0
```

**排产的两种方式**

| 方式 | 结果 |
|---|---|
| `queue <id> [n]`（直接 `QueuedObjects.AddItem`） | ✅ 安全，随时可追加，游戏逐个消费 |
| `qprod`/`prod`/`DemandProduction` | ⚠️ 工厂**忙时抛 C++ 异常**（`0xE06D7363`），空闲时可以但会顶掉玩家当前生产项 |

`queue` 的 typeId 用 `canbuild` 里那列 ID（如 `napowr`、`nahand`、`e2`、`shk`）。

**工厂会消失**：生产完成后游戏会回收工厂（`FactoryClass::Array` 里没了、`Primary_For*` 清零），这是正常的，不是 bug。玩家下次点该类生产时会再创建。

---

## 7. 建筑落地流程

1. 玩家点建造 → 游戏里的工厂推进到 `pct=54 susp=1`（造好、等放置）
2. `diag` 会显示该建筑在 limbo
3. `putinfo <gx> <gy>` 找合法格
4. `place <gx> <gy>`
5. 约 4 秒后再 `house 0` 确认它还在、血量正常

`place` 内部做了三件必须做的事（少一步楼会被游戏清掉）：
`Put(coord, North)` → `Place(false)` → `ActuallyPlacedOnMap = true`，最后清空工厂的 `Object/Balance/Production`。

---

## 8. 实战 playbook（已经跑通的一局）

```
1. houses / house 0        → 确认我是 house 0（钱和屏幕对上）
2. scan                    → 拿到敌方全部目标的 idx 与坐标
3. units                   → 按类型挑战斗单位（E2/FLAKT/DOG/HTK/CTNK），组 i:1,2,3
4. move i:… 100 66         → 行军到敌方基地边缘（网格）
5. attack i:… <idx>        → 集火最靠外的防御/电厂
6. 等 10~15s → scan        → 目标消失 = 打掉了；idx 会重排，**下一次必须重新取**
7. 循环 5~6，优先：光棱塔 → 电厂 → 兵营 → 建造厂 → 车厂/雷达
```

实战记录（一局，2026-11-08）：51~78 个 E2/FLAKT/DOG 分批推进，依次摧毁 NAPRIS 光棱塔、NAPOWR×3、NAHAND、NACNST，敌方基地重建电厂但建造厂没了。

**注意事项**
- `attack` 返回 `ordered=N` 只代表**命令下达成功**，不代表命中。判断战果只能看 `scan` 里的 hp / 目标是否消失。
- 索引每次都在变（单位死亡会重排 `TechnoClass::Array`）。**养成「每次操作前重取一遍 units/scan」的习惯**，不要缓存 idx。
- 远程单位（光棱塔、V3、无畏）不会主动走位，只能靠我们自己推进；敌人会打你基地（无人机/ZEP），要在家里留几个 E2。

---

## 9. 编译/注入的坑（都是真踩过的）

1. **`node`/`cl` 都不在 PATH**：用 `vcvars32.bat` 包一层，路径写死；Python 用 `python`。
2. **必须 `/utf-8 /DNOMINMAX /std:c++20 /EHsc`**，并且 `/link ole32.lib`（`CoCreateInstance`/`OleRun` 用）。少 `/DNOMINMAX` 会在 `YRMath.h` 的 min/max 上报 C2059。
3. **LNK1104：旧 dll 被游戏加载时无法覆盖**。解法：输出到新文件名（`mo_pilot_v61.dll`），管道名由文件名派生，因此多版本可共存、互不抢管道。**不要**为了覆盖去关游戏。
4. **改中文源码只能用 `write`/`edit` 工具**。用 PowerShell `Get-Content -Raw` + `Set-Content` 会把 UTF-8 中文注释按 ANSI 读成乱码再写回，源码只能推倒重写（踩过两次）。
5. **`__try` 里不能有带析构的 C++ 对象**（报 C2712）。把逻辑拆成独立函数，`__try` 里只用裸指针。
6. **YR++ 的补丁别丢**：`CCINIClass.h` 参数名 `default`→`default_`；`ArrayClasses.h` 的 `VectorClass::Clear/SetCapacity/Swap`、`DynamicVectorClass::Swap` 要补模板实参，移动构造要写 `VectorClass<T>(std::move(other))`。
7. **`YRComPtr::operator bool` 是 explicit**，用 `(bool)ptr`，别写 `!= nullptr`。
8. **游戏会在你写错的地方 Fatal Error**（`CRASH code=0xC0000005 addr=…`）。游戏内命令都包了 `__try/__except`，异常会被捕获成 `err av ...` 而不会崩游戏——但**手工造出来的对象**（假工厂、假单位）仍可能在之后的帧里被游戏访问而崩，看到 `Fatal Error - Yuri's Revenge` 只能任务管理器结束 gamemd。

---

## 10. 关键偏移（`probe`/`tools\offs.cpp` 实测）

| 项 | 值 |
|---|---|
| `TechnoClass::Health` | `+0x6C` |
| `TechnoClass::DisplayProductionTo` | `+0x210` |
| `TechnoClass::Group` | `+0x214` |
| `TechnoClass::Focus` | `+0x218` |
| `TechnoClass::Owner` | `+0x21C` |
| `HouseClass::Player`（= `Me()`） | `0xA83D4C` |
| `TechnoClass::Array` | `0xA8EC78` |
| 资金 | `*(int*)((char*)Me() + 0x30C)` |
| 游戏 `new FactoryClass` | `push 0x74; call 0x7C8E17; mov ecx,eax; call 0x4C98B0` |
| 建厂函数（只有游戏调用的那条路能 tick） | `0x4FA350`，内部读 `HouseClass+0x53D0..0x53D8` 的「本类已开始生产」标记 |

---

## 11. 还在的坑（后来者可以捡）

- **从零启动生产**：`0x4FA350` 的调用条件还没完全推出来（参数怎么排、前置标记怎么置）。若攻下来就能完全自主发育，不必玩家点侧边栏。
- **假单位 locomotor**：limbo 单位装 locomotor（`LocomotionClass::CreateInstance` / `ChangeLocomotorTo`）都会 AV，导致手工产出的兵不能动。修好它才能绕过"必须游戏产兵"。
- **多选/编队**：`g:N` 依赖玩家在游戏里 Ctrl+数字编队；可以考虑自己实现按框选（读单位坐标算，不走游戏选择集）。

---

## 12. 附：`+0x41C` 这条「认我方」判据（来自用户的 CE 脚本）

用户写过一个 CE ASM 补丁，挂在 `gamemd.exe+0x1F550D`，**目的是让 AI 能认出「谁是我方」**（他澄清过：实验那局并没有启用这个补丁）：

```
newmem:
  cmp [esi+6c+3b0],0      ; = [esi+0x41C]，用户实测「为 0 表示这是我方」
  je  returnhere          ; 是我方 → 跳过
originalcode:
  sub eax,eax             ; eax = 0
  mov [esi+6C],eax        ; TechnoClass::Health ← 0（原逻辑的扣血致死）
  jg  gamemd.exe+1F55EA
```

结论与用法：
- `+0x6C` = `TechnoClass::Health`（已实测）。
- `+0x41C` 在用户实测里「己方恒为 0」，是一条**辅助性别判据**，和 `+0x21C`（`Owner` 指针）互为交叉验证：`Owner` 指「属于哪个阵营」，`+0x41C` 指「是不是玩家自己」。
- 用 `mine` 命令做四象限核对：

  | 命令 | 作用 |
  |---|---|
  | `mine` | 遍历 `TechnoClass::Array`，按 `Owner==Me()` 与 `[+0x41C]==0` 两种判据分组，打印不一致的条目，末尾给 `both/ownerOnly/flagOnly/neither` 计数 |
  | `probe all` | 打印 `-- +0x41C scan --` 段，抽样对比 `mine` / `foe` 的该字段 |

- 若 `mine` 显示 `both` 覆盖绝大部分、`flagOnly` / `ownerOnly` 只有零星（通常是被心灵控制的单位、部署中的 MCV、或 limbo 对象），就以 `Owner` 为主、`+0x41C` 为辅。
- 顺带得到的写血能力：`hp <sel> <value>` 直接把 `+0x6C` 写成该值，`hp i:N 0` 即秒杀，`hp all 500` 全军补血。

> 这个 CE 补丁是**外部进程改代码**，和注入的 DLL 互不干扰；补丁启用时血量读数没有战术意义，别用它判断战力。

---

*最后更新：2026-11-08。改动 `src\mo_dll.cpp` 后记得升版本号并重新注入；旧版本 dll 留在 build 目录里当历史。*
