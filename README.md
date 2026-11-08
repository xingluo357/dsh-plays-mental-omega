# mo-pilot

> 仓库名 `dsh-plays-mental-omega` —— 这是给 [DeepSeek Harness](https://github.com/ranxianglei) 里的 AI 用的「驾驶层」，让它能真的开打心灵终结。工具本身叫 mo-pilot。

让 AI 真正上手玩 **命令与征服：尤里的复仇 / 心灵终结（Mental Omega）** —— 不是靠截图 + 模拟鼠标，而是往 `gamemd.exe` 注入一个 32 位 DLL，直接读战场、直接下命令。

- 不截图、不动鼠标：一次命令就是一行文本，token 花在决策上，不花在画面上。
- 双通道：**命名管道**（任意脚本/AI 都能连）和 **可选的人机提示层**（`hint/`，游戏画面左侧弹出「你要建造 xxx」）。
- 兼容旧引擎：基于 [YR++](https://github.com/Ares-Developers/YRpp) 的类定义与固定地址，实测精神终结 3.3.6 / 尤里的复仇 1.001。

> 只支持 32 位原版引擎（gamemd.exe）。不支持 RA3、不支持 Red Alert 2 Remastered。

---

## 它现在能做什么

| 能 | 不能（老实说） |
| --- | --- |
| 读资金、当前帧、我方单位列表（坐标/血量/编队/任务） | 从零启动一条生产线（见下） |
| 部署 MCV（展开成建造厂） | 让手工自建的工厂被游戏推进度 |
| 移动 / 攻击 / 守卫 / 停止，支持批量与编队选择器 | 在游戏外「凭空造兵」 |
| 列出敌方实体、按索引集火，读取任意阵营的资金与建筑 | 全自动开局（第一类生产仍要玩家点一次侧边栏） |
| 把已建成的建筑落到地图格子上 | 处理被 mod 改乱的内存布局（字段偏移会变） |

**最关键的一条限制**：游戏的 `FactoryClass` 只对自己创建的实例发生产 tick。玩家点侧边栏 → 游戏建工厂 → 从那一刻起我们能往队列里塞任何东西并接管排产；玩家没点过的那一类，函数 `GetPrimaryFactory` 只会「查」不会「建」（逆向过 `0x4fa350`，读 `HouseClass+0x53d0..0x53d8` 的启动标记，手工置位也无效）。

所以推荐的合作方式是：**人点一次侧边栏开产，AI 负责排产、落地、编队和打仗**。

---

## 两条通道

### 1. 命名管道（主通道）

DLL 加载后创建 `\\.\pipe\<dll 文件名>`：`mo_pilot_v57.dll` → `\\.\pipe\mo-pilot-v57`（下划线换连字符、去掉 `.dll`）。这样多个版本可以并存，改代码时不用让玩家退出游戏。

```powershell
git clone https://github.com/<you>/mo-pilot.git
cd mo-pilot

# 1) 编译（需要 VS 2022 的 x86 工具链）
.\build.ps1            # 产物在 build\mo_pilot.dll

# 2) 注入（游戏已在跑）
build\inject.exe <gamemd 的 PID> build\mo_pilot.dll

# 3) 说话
python src\mo_cli.py --pipe mo-pilot state
python src\mo_cli.py --pipe mo-pilot units
python src\mo_cli.py --pipe mo-pilot move i:307 100 66
```

`src\mo_cli.py` 是纯标准库实现，一共几十行，换成任何语言都行。

### 2. 提示层 `hint/`（可选）

游戏画面左侧的半透明浮层，8 秒淡出、支持队列。用来让 AI 直接对玩家说话，例如「你要建造 发电厂，然后造兵营」。

```powershell
build\inject.exe <PID> build\mo_hint.dll
python hint\hint_cli.py push "第一步：造两座发电厂"
python hint\hint_cli.py push "第二步：造兵营，然后出 10 个动员兵"
```

详见 [hint/README.md](hint/README.md)。

---

## 命令速览

选择器 `sel` 的写法：`all`（全部我方）、`g:N`（编队 N）、`i:1,2,3`（`units` 打印的索引）。

| 类别 | 命令 |
| --- | --- |
| 读 | `ping` `state` `units` `scan` `houses` `house <i>` `status` `canbuild` `putinfo <gx> <gy>` |
| 动 | `move <sel> <gx> <gy>` `attack <sel> <idx>` `guard <sel>` `stop <sel>` `deploy <sel>` `mission <sel> <n>` |
| 生产 | `queue <id> [n]`（往活工厂追加） `qprod <id> [n]` `resume` `drainq <id>` `drainall` |
| 建造 | `newfactory` `place <gx> <gy>` |
| 逆向 | `diag` `probe` `cameos` `hookctor` `hookinfo` `hp <sel> <v>` `mine` … |

完整参数、语义、坑，见 **[docs/MANUAL.md](docs/MANUAL.md)**（这份手册是写给「下一个接手的 AI」的，写得比较啰嗦，但是真的都是踩过的坑）。

---

## 依赖

- **Visual Studio 2022**（x86 工具链，`vcvars32.bat`）
- **[YR++](https://github.com/Ares-Developers/YRpp)** 头文件 —— 本仓库不附带，原因与打补丁的方法见 [docs/YRPP-COMPAT.md](docs/YRPP-COMPAT.md)
- Python 3（只用标准库）

## 坐标怎么算

`move` / `place` / `putinfo` 收的是 **lepton 网格值**：`gx = 世界坐标 / 256`。不是小地图坐标，也不是屏幕像素。斜着算错一格就会把建筑放到不可建的地格上。`place` 失败时先用 `putinfo` 看那一格的 `land/flags/content`。

## 认哪边是「我方」

别信 `diag` 里 `me->Buildings` 打出来的东西（某些 mod 的字段偏移有偏差）。可靠做法：

1. `houses` 列出 `HouseClass::Array`，再 `house <i>` 看资金 —— 跟你屏幕上看到的数字对上，那个就是你。
2. 单位层面用 `TechnoClass+0x21C`（`Owner`）是否等于 `HouseClass::Player`（`0xA83D4C`）。
3. 另一个辅助判据（来自一位用 CE 的朋友）：`[TechnoClass+0x41C] == 0`。`mine` 命令会把这两种判据的四象限都数出来，对不上时你会看见。

## 安全提醒

- 这是内存级的 mod 工具，**写错地址就是直接崩游戏**（`gamemd.exe` 没有异常恢复）。改代码前先备份存档。
- 只支持单人 / 遭遇战。**不要**在联网对战里用。
- `hp` 命令能改血量，`mine`/`hookctor` 这类是逆向工具，日常不需要。

## 许可

GPL-3.0（因为 `#include` 了同样以 GPLv3 发布的 YR++ 头文件，本仓库属于衍生作品）。见 [LICENSE](LICENSE)。

心灵终结（Mental Omega）与红色警戒 2 的版权归各自权利人所有，本仓库不含任何游戏资源。
