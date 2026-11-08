# 让 YR++ 在 VS 2022 上编过（3 处补丁）

本仓库 **不附带 YR++ 源码**。原因：YR++ 是 Ares 团队的项目（GPLv3），把它的头文件塞进本仓库既臃肿又容易和上游漂移。请自行下载：

```powershell
# 没有 git 也行，直接下 zip
Invoke-WebRequest "https://codeload.github.com/Ares-Developers/YRpp/zip/refs/heads/master" -OutFile YRpp.zip
Expand-Archive YRpp.zip -DestinationPath .
# → YRpp-master\
```

`build.ps1` 默认在仓库根目录找 `YRpp-master\`，也可以用 `-Yrpp <path>` 指定。

YR++ 是用老版本 MSVC 写的，直接丢进 VS 2022 的 `cl.exe` 会报几处错。**只改这 3 处**（改的是头文件里的语法，不改逻辑）：

## 1. `CCINIClass.h` 第 222 / 225 行：`default` 是 C++ 关键字

```cpp
// 改前
... LoadFromStream(..., ..., bool default = ...);
// 改后（参数名换成 default_，两处）
... LoadFromStream(..., ..., bool default_ = ...);
```

`default` 在 C++ 里是保留字，新编译器不再容忍做参数名。

## 2. `ArrayClasses.h`：模板成员函数的模板实参

新标准下，类模板里调用自己的成员模板不再能靠推导，必须显式写出。涉及：

- `VectorClass::Clear`、`VectorClass::SetCapacity`、`VectorClass::Swap`
- `DynamicVectorClass::Swap`

把 `Clear();` 这类调用改成 `VectorClass<T>::Clear();`。`CounterClass` 那一处是 `VectorClass<int>::Swap(...)`。

错误形态：`error C2951` / `error C2760`，指向 `Clear`、`Swap`。

## 3. `ArrayClasses.h` 第 217 行：移动构造的初始化列表

```cpp
// 改前
VectorClass(VectorClass&& other) : ..., (std::move(other))
// 改后
VectorClass(VectorClass&& other) : ..., VectorClass<T>(std::move(other))
```

少了基类名，`cl` 会当成「在初始化列表里调用了一个函数」。

---

## 编译期还需要的一个开关

`/DNOMINMAX`。YRPP 的 `YRMath.h` 里用了 `min` / `max`，而 `<windows.h>` 的宏会把它们替换掉，报：

```
error C2059: 语法错误:“)”
```

`build.ps1` 已经带上了。

## 验证是不是全好了

```powershell
.\build.ps1
# 看到 "OK -> build\mo_pilot.dll" 就通了
```

如果报的是 `LNK1104: 无法打开文件 build\mo_pilot.dll`，那不是代码问题 —— 是**旧 DLL 还被游戏加载着**。换个文件名输出（`.\build.ps1 -Out mo_pilot_v58.dll`），注入新名字那个，管道名会跟着变（见 README「两条通道」）。

## 上游是否会跟上

YR++ 的目标是兼容经典编译器（MSVC 6 / .NET 2003 时代），所以这 3 处补丁大概率**不会**被上游合并。它们是本地兼容层，不是 bug 修复。
