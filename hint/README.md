# mo-hint —— 让 AI 在游戏画面上对玩家说话

注入 `mo_hint.dll` 后，游戏屏幕**中左侧**会出现一条「半透明黑底 + 白字」的提示，内容完全由 AI 决定，比如：

> **你要建造 发电厂，然后造兵营**

每条显示 8 秒后淡出；支持队列（一次下发多步，按顺序逐条展示）。

---

## 1. 原理（为什么这样做）

游戏是 `gamemd.exe` + cnc-ddraw（`fullscreen=false, border=True`，也就是无边框窗口）。
所以**不用碰游戏的渲染表面**：注入的 DLL 在游戏进程里开一个分层窗口
（`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE`），
用 `UpdateLayeredWindow` 把文字画上去。

- 不 hook DDraw、不改后台缓冲，**不会拖慢游戏、不会有撕裂**
- 鼠标点击穿透（`WS_EX_TRANSPARENT`），不会挡住玩家操作
- 中文渲染走 GDI 微软雅黑，抗锯齿正常

时序：管道线程收指令 → 加锁改状态 → `PostMessage(WM_APP+1)` 让 UI 线程重绘；
UI 线程每 40ms 的 `WM_TIMER` 负责淡出（8s 保持 + 0.7s 淡出）和切下一条。

---

## 2. 编译 / 注入 / 使用

```powershell
$d='mo-hint'

# 编译（只在改了源码后需要）
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat`" >nul && cl /nologo /utf-8 /DNOMINMAX /LD /std:c++20 /EHsc /Fo:`"$d\build\\`" /Fe:`"$d\build\mo_hint.dll`" `"$d\src\hint_dll.cpp`" /link user32.lib gdi32.lib"

# 注入（游戏开着，拿 gamemd.exe 的 PID）
Get-Process gamemd | Select-Object Id
& 'mo-pilot\build\inject.exe' <PID> "$d\build\mo_hint.dll"

# 说话（管道名 = dll 文件名，下划线换连字符）
$py='python'
& $py "$d\src\hint_cli.py" say "你要建造 发电厂"
& $py "$d\src\hint_cli.py" push "第二步：造兵营"
```

---

## 3. 命令

| 命令 | 作用 |
|---|---|
| `say <文本>` | 立刻显示（并清空队列） |
| `push <文本>` | 进队列，等当前这条放完再显示 |
| `next` | 跳过当前这条 |
| `clear` | 清空并隐藏 |
| `pos <x> <y>` | 面板左上角，`y=-1` 表示垂直居中（默认 `48 -1`） |
| `size <pt>` | 字号（默认 34，范围 10–120） |
| `info` | 查询当前文本 / 队列长度 / 不透明度 / 位置 |
| `test` | 显示一条示例 |
| `ping` | 探活 |

返回都是单行 `ok ...` 或 `err ...`。文本以 UTF-8 传入，DLL 内部转 UTF-16 渲染。

---

## 4. 目录

```
mo-hint/
  src/hint_dll.cpp     全部逻辑（分层窗口 + 消息循环 + 管道服务器）
  src/hint_cli.py      管道客户端（纯标准库）
  tools/host.cpp       32 位测试宿主窗口（无游戏时也能验证覆盖窗口）
  build/mo_hint.dll    产物
  build/host.exe       测试宿主
```

无游戏时的自测流程：

```powershell
$p = Start-Process "$d\build\host.exe" -PassThru
& 'mo-pilot\build\inject.exe' $p.Id "$d\build\mo_hint.dll"
& $py "$d\src\hint_cli.py" say "测试：你要建造 发电厂"
```

---

## 5. 已验证 / 未验证

**已验证**（2026-11-08，用 `build\host.exe` 做宿主 + 桌面截图）：
- 分层窗口能画在别的窗口上面，中文正常、位置在屏幕中左、鼠标可穿透
- `say` 立即可见；8 秒后淡出
- `push` 队列按顺序推进（第一条淡出后自动显示第二条）
- `info` 状态正确

**未验证**：
- 在 `gamemd.exe` 里注入后的实际显示效果（游戏当时没开）
- 如果哪天把 `ddraw.ini` 改成真独占全屏（`fullscreen=true`），分层窗口会被盖住，需要改成 hook 后台表面画字——目前不需要

---

*最后更新：2026-11-08*
