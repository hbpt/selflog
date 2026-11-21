# selflog — 会记录自己的 exe / 脚本

一套"会记录自己的程序"：Windows 版 `selflog.exe`（本文件主体）与功能对齐的 Linux/macOS bash 版 `selflog.sh`（见文末「bash 版」一节）。

Windows 版是一个 193 KB 的单文件控制台程序（x64，`/MT` 静态链接，不依赖任何运行库）。

它的主要用法是**透明代理**：把某个程序改成"套壳"——`original.exe` 换成 selflog，真身改名 `original_real.exe`。之后每次有人启动 `original.exe`，参数和所有输入输出都会原样转给真身，同时完整落进日志文件，退出码也原样返回，调用方察觉不到中间多了一层。

```
dist\selflog.exe        ← 可直接分发
```

---

## 三分钟上手

```bat
:: 1) 让 selflog 顶替 foo.exe（真身自动改名为 foo_real.exe）
selflog.exe --install "C:\tools\foo.exe"

:: 2) 之后照常使用，谁启动都一样
"C:\tools\foo.exe" --version
type input.txt | "C:\tools\foo.exe" --strict

:: 3) 看日志（默认在 foo.exe 同目录的 selflog.log）
type "C:\tools\selflog.log"

:: 4) 想撤销随时可以
selflog.exe --uninstall "C:\tools\foo.exe"
```

手工做同样的事也就两步：把 `foo.exe` 改名 `foo_real.exe`，再把 `selflog.exe` 复制成 `foo.exe`。**判定规则只有一条：同目录存在 `<自身主名>_real.exe` 就是代理模式。** 所以同一个 selflog.exe 复制成多少份、叫多少名字都不会互相干扰。

`--uninstall` 删除替身前会逐字节确认那确实是当前 selflog 的副本，不会误删别的文件；`--install` 也不会覆盖已存在的 `_real`。

---

## 三种模式

| 模式 | 触发条件 | 行为 |
|---|---|---|
| **透明代理**（主用法） | 同目录存在 `<自身主名>_real.exe`（或设了 `SELFLOG_REAL`） | 命令行**一个字节不改**转发给真身，tee 它的 stdin/stdout/stderr，退出码原样返回 |
| **包装模式** | 命令行里出现 `--` 或第一个非选项参数 | 记录并转发给指定程序，相当于临时用一次代理 |
| **自身模式** | 以上都不满足 | 像个带日志的 `cat`：把 stdin 抄到 stdout，全程记录 |

### 代理模式为什么不能解析自己的选项

因为 `foo.exe --log x.log` 里的 `--log` 是属于 foo 的参数，被截走会导致被代理程序行为异常。所以：

- 代理模式下 **selflog 一个选项都不解析**，全部原样转发（`argv[0]` 换成真身路径，其余按 Windows 规则重新拼装成命令行）。
- 配置只能走环境变量（见下），并且默认不打印任何提示，不污染被代理程序的标准错误。

```bat
set SELFLOG_FILE=D:\logs\foo.log     :: 换个日志位置
set SELFLOG_CP=936                   :: 按 GBK 解码，让中文可读
set SELFLOG_RAW=1                    :: 原样写入，不转义
set SELFLOG_NOECHO=1                 :: 只记日志，不回显
set SELFLOG_TRUNCATE=1               :: 本次运行前清空
set SELFLOG_TAG=nightly              :: 会话标签
set SELFLOG_REAL=D:\bin\foo.real.exe :: 手工指定真身，覆盖 _real 约定
```

---

## 日志长什么样

同一文件按会话追加，每次启动一个 `SESSION … END` 块，字段固定、可直接 `findstr`：

```
================================================================================
SESSION 2026-11-13 16:22:41.143  (local)   |   2026-11-13 08:22:41.143Z  (UTC)
================================================================================
mode          : proxy (transparent forward)
tag           : proxytest
exe           : C:\tools\proxydemo.exe
cwd           : C:\work
pid           : 264
parent        : 14004 (powershell.exe)
user          : DELL\Administrator
log_file      : C:\tools\selflog.log
encoding      : escaped (byte-exact, UTF-8 passed through)
cmdline       : "C:\tools\proxydemo.exe" hello
proxy_of      : C:\tools\proxydemo.exe
real_exe      : C:\tools\proxydemo_real.exe
fwd_argc      : 1
fwd[1]        : hello
--------------------------------------------------------------------------------
child         : C:\tools\proxydemo_real.exe hello
--------------------------------------------------------------------------------
child_pid     : 12140
--------------------------------------------------------------------------------
--- [+0.093s] IN  37 bytes ---
hello world\nsecond line\nhello again\r\n
--- [+0.109s] OUT 25 bytes ---
hello world\nhello again\r\n

--------------------------------------------------------------------------------
END 2026-11-13 16:22:41.251  elapsed=0.109s  exit=0
totals        : stdin=37 stdout=25 stderr=0 bytes
================================================================================
```

设计取舍：

- **`\r\n` 是转义显示的**，日志永远是干净文本，不会被 Windows 换行搞乱；但**合法 UTF-8 序列原样保留**，所以中文能直接读。要字节级原样存用 `--raw`。
- **每段 I/O 带相对时间戳和字节数**，粒度是"一次读写批次"。
- **`cmdline`（代理收到的原文）和 `fwd[n]`（真正转发出去的参数）都有**，代理出问题时能一眼看出是谁改坏了参数。
- 每条记录写完立刻 `fflush`，进程被强杀也不丢已记录内容。
- `--cp 936` 可以把 GBK 输出解成 UTF-8 再落盘（牺牲逐字节可逆，换可读性）。

### 字段说明

| 字段 | 含义 |
|---|---|
| `mode` | `proxy` / `wrapper` / `self` |
| `exe` | 本进程（替身）的完整路径 |
| `proxy_of` | 调用方实际启动的那个名字 |
| `real_exe` | 找到的真身路径 |
| `fwd_argc` / `fwd[n]` | 原样转发给真身的参数 |
| `cwd` | 启动时的工作目录（相对路径解析问题十有八九在这儿） |
| `pid` / `parent` | 本进程 PID、父进程 PID 与映像名 |
| `encoding` | 本次负载的编码姿态，决定日志里的字节怎么解读 |
| `totals` | 整个会话的 stdin/stdout/stderr 字节数 |

---

## 选项（仅非代理模式可用）

| 选项 | 说明 |
|---|---|
| `--install <程序.exe>` | 安装代理：真身改名 `<主名>_real.exe`，selflog 顶替原名 |
| `--uninstall <程序.exe>` | 卸载代理：验明替身身份后删除，并把真身改回原名 |
| `-l, --log <文件>` | 日志路径，父目录不存在会自动创建 |
| `-t, --tag <名称>` | 会话标签 |
| `--truncate` | 本次运行前清空日志（默认追加） |
| `--raw` | 负载原样写入，不转义 |
| `--cp <代码页>` / `--text` | 按代码页（如 936）解码为 UTF-8 再写日志 |
| `--no-echo` | 包装模式下只记日志，不回显子进程输出 |
| `--no-stdin` | 自身模式下不读 stdin，只记录启动参数 |
| `--say <文本>` / `--err <文本>` | 自身模式往 stdout / stderr 写一段文本，用于自测 |
| `-q, --quiet` | 不打印提示信息 |
| `-h, --help` / `-v, --version` | 帮助 / 版本 |

---

## 典型用途

**查"这个程序到底是谁、用什么参数启动的"**——把它挂上代理，日志里参数和后续行为能对上：

```bat
selflog.exe --install "%ProgramFiles%\Vendor\App\tool.exe"
```

**抓自动构建/CI 的完整输出**（时间戳能看出哪一步卡了多久）：

```bat
selflog.exe --log build.log --tag nightly -- msbuild /m /p:Configuration=Release
```

**复现"同样输入为什么这次结果不同"**——stdin 原文和 stdout 逐段留档：

```bat
type case.json | selflog.exe --log case.log -- myparser.exe --strict
```

**给别人发一份可读的 bug 复现材料**：纯文本、带时间戳和字节数、会话之间有分隔线。

---

## 构建与测试

需要 Visual Studio 2022（含 C++ 桌面开发工作负载）：

```bat
build.bat            :: 产出 dist\selflog.exe
test\selftest.bat    :: 7 组回归断言
```

构建脚本自己定位 MSVC 工具集和 Windows SDK，不调用 `vcvars64.bat`（它在受限环境里会调 `reg.exe` 而被策略拦下）。

源码：`src\selflog.c`（单文件）、`src\selflog.rc`（版本信息）。

---

## 实现要点

- 代理/包装模式用三个匿名管道 + 三个泵线程 tee：`stdin → 子进程`、`子进程 stdout → 控制台 + 日志`、`子进程 stderr → 控制台 + 日志`。
- 管道是流式的，子进程每次 `write` 都会唤醒泵；如果每次都落一条记录，日志会被 `dir` 这类逐行输出切成几十段。所以 OUT/ERR 侧用 `PeekNamedPipe` 判断"管道已抽干"才落盘（或缓冲满 64KB）。
- `CreateProcessW` 只吃一整条命令行字符串，代码按 Windows 的反向规则重新拼装（含空格/制表/引号加引号，引号前反斜杠成对翻倍），并给真身换掉 `argv[0]`。
- 嵌套保护：每层把 `SELFLOG_DEPTH` 加一传给子进程，超过 4 层直接报错退出，`_real` 万指回自己也不会无限递归。
- 日志写入走单把临界区 + 每条 `fflush`；`stdout`/`stderr` 两个线程并发写不会错乱。
- 自己的提示信息走 `WriteConsoleW`，控制台代码页 936 也不乱码。

## 已知限制

1. **子进程的标准流是管道，不是控制台**。子进程会认为输出被重定向，C 运行库切全缓冲——`python` 不加 `-u`、`grep` 不加 `--line-buffered` 时输出会滞后成块（内容不丢，实时性差）。
2. **没有 PTY**。依赖真实终端的程序（`more` 翻页、进度条、彩色、需要 cursor 控制的 TUI）行为会退化；命令行工具、批处理、构建脚本不受影响。
3. **读密码这类直接访问控制台的输入会失败**（子进程的 stdin 是管道）。这类程序不适合挂代理。
4. **Ctrl+C 同时打到替身和真身**，两边各处理一次。
5. 若把 `_real` 删掉，替身会退化成自身模式（等 stdin），并在 stderr 上提示一行；同一日志文件被多实例并发追加时不保证块间严格时序。
6. **日志会记录标准输入原文**，包括误输入的口令，注意文件权限。
7. 本 exe 是 x64 且静态链接；能记录 x86 子进程，但自身不能在 32 位 Windows 上运行。

---

## bash 版（Linux / macOS / Git Bash）

`dist/selflog.sh` 是与 exe 功能对齐的纯 bash 实现（bash ≥ 4，依赖 tee/cat/mktemp/iconv 等标准工具），三种模式、`--install/--uninstall`、`SELFLOG_*` 环境变量、退出码透传、递归保护全部一致。

```bash
# 1) 让 selflog 顶替 foo（真身自动改名为 foo_real）
selflog.sh --install ~/tools/foo

# 2) 之后照常使用
~/tools/foo --version
cat input.txt | ~/tools/foo --strict

# 3) 撤销
selflog.sh --uninstall ~/tools/foo

# 包装模式 / 自身模式
selflog.sh -l run.log -- ls -la
cat in.txt | selflog.sh --log run.log
```

**判定规则与 exe 相同**：同目录存在 `<自身主名>_real`（可执行）即代理模式；名字取自脚本实际路径（`readlink -f "$0"`），复制成多少份都不冲突。

环境变量名与 exe 一致：`SELFLOG_FILE / SELFLOG_TAG / SELFLOG_CP / SELFLOG_RAW / SELFLOG_NOECHO / SELFLOG_TRUNCATE / SELFLOG_REAL`（`SELFLOG_CP` 用 iconv 编码名，如 `GBK`、`GB18030`，不再是数字代码页）。

### 与 exe 版的实现差异

| 方面 | exe 版 | bash 版 |
|---|---|---|
| 管道 | 3 个匿名管道 + 3 个泵线程 | 3 个 FIFO + 3 个后台 `tee` 泵 |
| 负载落盘 | 逐块带时间戳、抽干才落盘（分段少） | 实时写入临时文件，进程退出后统一汇编（按流记录总字节数） |
| 转义 | 控制字符 `\xHH`，合法 UTF-8 放行 | 默认只转义 CR/TAB（`\r` `\t`），其余字节原样 |
| 强杀保护 | 每条立即 fflush，被杀不丢 | 数据在临时文件里，**被 SIGKILL 时日志只剩会话头** |
| 编码 | `--cp 936`（Windows 代码页） | `--cp GBK`（iconv 名称） |

### bash 版已知限制

1. 同样的管道限制：子进程看不到 TTY，`python -u`、`grep --line-buffered` 才实时。
2. 日志汇编发生在正常退出时；进程被 `kill -9` 时本次会话的 I/O 不会写进日志。
3. `mkfifo` 在 NFS 等特殊文件系统上不可用时无法运行（`/tmp` 一般没问题）。
4. MSYS/Git Bash 下可用，但 Windows 控制台交互（Ctrl+Z 等）与原生 exe 行为略有差异。

回归测试：`test/selftest.sh`，32 项断言（参数记录、I/O 镜像、退出码透传、代理转发、install/uninstall 往返 sha256 校验、递归保护、GBK/UTF-8 编码、no-echo/no-stdin、多会话追加）。

---

## bash 版（Linux / macOS / Git Bash）

`dist/selflog.sh` 是与 exe 功能对齐的纯 bash 实现（bash ≥ 4，依赖 tee/cat/mktemp/iconv 等标准工具），三种模式、`--install/--uninstall`、`SELFLOG_*` 环境变量、退出码透传、递归保护全部一致。

```bash
# 1) 让 selflog 顶替 foo（真身自动改名为 foo_real）
selflog.sh --install ~/tools/foo

# 2) 之后照常使用
~/tools/foo --version
cat input.txt | ~/tools/foo --strict

# 3) 撤销
selflog.sh --uninstall ~/tools/foo

# 包装模式 / 自身模式
selflog.sh -l run.log -- ls -la
cat in.txt | selflog.sh --log run.log
```

**判定规则与 exe 相同**：同目录存在 `<自身主名>_real`（可执行）即代理模式；名字取自脚本实际路径（`readlink -f "$0"`），复制成多少份都不冲突。

环境变量名与 exe 一致：`SELFLOG_FILE / SELFLOG_TAG / SELFLOG_CP / SELFLOG_RAW / SELFLOG_NOECHO / SELFLOG_TRUNCATE / SELFLOG_REAL`（`SELFLOG_CP` 用 iconv 编码名，如 `GBK`、`GB18030`，不再是数字代码页）。

### 与 exe 版的实现差异

| 方面 | exe 版 | bash 版 |
|---|---|---|
| 管道 | 3 个匿名管道 + 3 个泵线程 | 3 个 FIFO + 3 个后台 `tee` 泵 |
| 负载落盘 | 逐块带时间戳、抽干才落盘（分段少） | 实时写入临时文件，进程退出后统一汇编（按流记录总字节数） |
| 转义 | 控制字符 `\xHH`，合法 UTF-8 放行 | 默认只转义 CR/TAB（`\r` `\t`），其余字节原样 |
| 强杀保护 | 每条立即 fflush，被杀不丢 | 数据在临时文件里，**被 SIGKILL 时日志只剩会话头** |
| 编码 | `--cp 936`（Windows 代码页） | `--cp GBK`（iconv 名称） |

### bash 版已知限制

1. 同样的管道限制：子进程看不到 TTY，`python -u`、`grep --line-buffered` 才实时。
2. 日志汇编发生在正常退出时；进程被 `kill -9` 时本次会话的 I/O 不会写进日志。
3. `mkfifo` 在 NFS 等特殊文件系统上不可用时无法运行（`/tmp` 一般没问题）。
4. MSYS/Git Bash 下可用，但 Windows 控制台交互（Ctrl+Z 等）与原生 exe 行为略有差异。

回归测试：`test/selftest.sh`，32 项断言（参数记录、I/O 镜像、退出码透传、代理转发、install/uninstall 往返 sha256 校验、递归保护、GBK/UTF-8 编码、no-echo/no-stdin、多会话追加）。
