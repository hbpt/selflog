#!/usr/bin/env bash
# =============================================================================
# selflog.sh —— 会记录自己的脚本（Linux/bash 版，功能与 selflog.exe 对齐）
#
#   selflog.sh [选项]                    自身模式：记录启动参数，stdin 原样转发到
#                                        stdout 的同时写入日志（行为等价 cat）
#   selflog.sh [选项] -- 程序 [参数...]  包装模式：记录指定程序的全部 I/O
#   selflog.sh --install <原始程序>      把自己顶替成该程序（真身改名 <主名>_real）
#   selflog.sh --uninstall <原始程序>    撤销顶替
#
# 透明代理模式：
#   把 original 改名为 original_real，再把本脚本复制成 original。
#   之后每次启动 original，参数与 stdin/stdout/stderr 原样转发给 original_real，
#   同时写进日志；退出码原样返回。代理模式下不解析任何命令行选项
#   （参数必须原样转发），配置只能走环境变量（SELFLOG_*，见 --help）。
# =============================================================================
set -u
SELFLOG_VER="1.0.0sh"

# ---------------------------------------------------------------- 全局状态
g_log=""            # 日志路径（resolve_log 之后有效）
g_log_arg=""        # --log 指定的路径
g_tag=""
g_cp=""             # iconv 源编码（空 = 原样字节）
g_raw=0
g_noecho=0
g_quiet=0
g_truncate=0
g_nostdin=0
g_say=""
g_err=""
g_mode=""           # self | wrapper | proxy
tmpdir=""
T_START=0

self_exe=$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")
self_dir=$(dirname "$self_exe")
self_base=$(basename "$self_exe")
case "$self_base" in
  *.*) self_stem="${self_base%.*}" ;;   # original.sh -> original；无扩展名保持原样
  *)   self_stem="$self_base" ;;
esac

die() { printf 'selflog: %s\n' "$2" >&2; exit "$1"; }

# MSYS/Git Bash 下 mktemp 可能返回带反斜杠的混合路径，统一转成 /c/... 形式，
# 否则 tee 写入的路径和 bash 内建 [ -f ] 看到的路径会不一致
if command -v cygpath >/dev/null 2>&1; then
    _t=$(cygpath -u "${TMPDIR:-${TEMP:-/tmp}}" 2>/dev/null) || _t=/tmp
    TMPDIR=${_t:-/tmp}
    export TMPDIR
    unset _t
fi

# ---------------------------------------------------------------- 时间工具
now_local() { date '+%Y-%m-%d %H:%M:%S.%3N' 2>/dev/null || date '+%Y-%m-%d %H:%M:%S'; }
now_utc()   { date -u '+%Y-%m-%d %H:%M:%S.%3N' 2>/dev/null || date -u '+%Y-%m-%d %H:%M:%S'; }
elapsed_now() {
    if [ "$T_START" != "0" ] && [ -n "${EPOCHREALTIME:-}" ]; then
        awk -v a="$T_START" -v b="$EPOCHREALTIME" 'BEGIN{printf "%.3f", b-a}'
    else
        echo "?"
    fi
}

# ---------------------------------------------------------------- 环境变量工具
env_flag() {
    local v="${!1:-}"
    case "$v" in 1|[Yy]|[Tt]*) echo 1 ;; *) echo 0 ;; esac
}
env_int() {
    local v="${!1:-$2}"
    case "$v" in ''|*[!0-9]*) echo "$2" ;; *) echo "$v" ;; esac
}

# ---------------------------------------------------------------- 找“真身”
# 查找顺序：SELFLOG_REAL 环境变量 > 同目录 <主名>_real > 同目录 <主名>_real.exe
# 主名取自本脚本的实际文件名（readlink -f 解析过），不是 $0 的原始写法。
REAL_EXE=""
find_real() {
    local cand
    if [ -n "${SELFLOG_REAL:-}" ] && [ -f "$SELFLOG_REAL" ] && [ -x "$SELFLOG_REAL" ]; then
        REAL_EXE=$SELFLOG_REAL
        return 0
    fi
    for cand in "$self_dir/${self_stem}_real" "$self_dir/${self_stem}_real.exe"; do
        if [ -f "$cand" ] && [ -x "$cand" ]; then
            REAL_EXE=$cand
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------- 日志文件
# 不带 --log 时默认写到脚本同目录 selflog.log；不可写则退到 $TMPDIR，再退当前目录
resolve_log() {
    local cand="${1:-}"
    if [ -z "$cand" ]; then
        cand="$self_dir/selflog.log"
        if ! : >> "$cand" 2>/dev/null; then
            cand="${TMPDIR:-/tmp}/selflog.log"
            : >> "$cand" 2>/dev/null || cand="$PWD/selflog.log"
        fi
    fi
    mkdir -p "$(dirname "$cand")" 2>/dev/null || true
    : >> "$cand" 2>/dev/null || die 3 "日志文件不可写: $cand"
    g_log="$cand"
}

log_kv()   { printf '%-14s: %s\n' "$1" "$2" >> "$g_log"; }
log_line() { printf '%s\n' "$*" >> "$g_log"; }

# ---------------------------------------------------------------- 会话头/尾
write_header() {  # $1=模式说明  $2=参数数组名
    local mode_desc=$1 arr_name=$2 i
    local -a args=()
    eval 'args=("${'"$arr_name"'[@]}")'

    {
        log_line "SESSION $(now_local) (local) | $(now_utc)Z (UTC)"
        log_kv mode "$mode_desc"
        [ -n "$g_tag" ] && log_kv tag "$g_tag"
        if [ "$g_raw" = 1 ]; then
            log_kv encoding "raw bytes (verbatim)"
        elif [ -n "$g_cp" ]; then
            log_kv encoding "text ($g_cp -> UTF-8 via iconv; CR/TAB escaped)"
        else
            log_kv encoding "escaped (CR/TAB as \\r \\t; UTF-8 passed through)"
        fi
        log_kv cwd "$PWD"
        log_kv parent "$PPID ($(ps -o comm= -p "$PPID" 2>/dev/null || echo '?'))"
        log_kv log_file "$g_log"
        log_kv cmdline "$(printf '%q ' "${args[@]}")"
    } >> "$g_log"

    log_kv "argc" "${#args[@]}"
    for i in "${!args[@]}"; do
        log_kv "argv[$i]" "${args[i]}"
    done
}

# ---------------------------------------------------------------- 负载落盘
# 默认：CR/TAB 转义成 \r \t（日志保持干净文本，可 grep），其余字节原样
#       （Linux 文本环境多为 UTF-8，中文直接可读）。
# --raw：字节原样。--cp <编码>：先 iconv 解码成 UTF-8 再落盘。
escape_stream() {
    LC_ALL=C sed -e 's/\r/\\r/g' -e 's/\t/\\t/g'
}

emit_stream() {  # $1=标签(IN/OUT/ERR)  $2=临时文件
    local kind=$1 f=$2 n
    [ -f "$f" ] || return 0
    n=$(wc -c < "$f" 2>/dev/null) || n=0
    [ "$n" -gt 0 ] || return 0
    {
        printf -- '--- %-3s %s bytes ---\n' "$kind" "$n"
        if [ -n "$g_cp" ]; then
            iconv -f "$g_cp" -t UTF-8 "$f" >> "$g_log" 2>/dev/null || cat "$f" >> "$g_log"
        elif [ "$g_raw" = 1 ]; then
            cat "$f" >> "$g_log"
        else
            escape_stream < "$f" >> "$g_log"
        fi
        echo
    } >> "$g_log"
}

finish_log() {  # $1=退出码
    local rc=$1 bin=0 bout=0 berr=0
    [ -f "$tmpdir/in"  ] && bin=$(wc -c < "$tmpdir/in"  2>/dev/null || echo 0)
    [ -f "$tmpdir/out" ] && bout=$(wc -c < "$tmpdir/out" 2>/dev/null || echo 0)
    [ -f "$tmpdir/err" ] && berr=$(wc -c < "$tmpdir/err" 2>/dev/null || echo 0)

    emit_stream IN  "$tmpdir/in"
    emit_stream OUT "$tmpdir/out"
    emit_stream ERR "$tmpdir/err"
    log_kv totals "stdin=$bin stdout=$bout stderr=$berr bytes"
    log_line "END $(now_local)  elapsed=$(elapsed_now)s  exit=$rc"
}

# ---------------------------------------------------------------- 帮助
usage() {
    cat <<'EOF'
selflog —— 会记录自己的脚本：启动参数 + stdin/stdout/stderr（bash 版）

用法:
  selflog.sh [选项]                        自身模式：边读 stdin 边写 stdout，全程记录
  selflog.sh [选项] -- 程序 [参数...]       包装模式：记录指定程序
  selflog.sh --install <原始程序>           把自己顶替成该程序（真身改名 <主名>_real）
  selflog.sh --uninstall <原始程序>         撤销顶替，把真身改回来

透明代理模式（主要用法）:
  original 改名为 original_real，再把本脚本复制成 original。
  之后每次启动 original，参数与 stdin/stdout/stderr 都原样转发给
  original_real，同时写进日志；退出码原样返回，调用方察觉不到中间多了一层。
  此模式不解析任何命令行选项（参数必须原样转发），配置只能走环境变量。

选项:
  -l, --log <文件>    指定日志文件（默认：脚本同目录 selflog.log）
  -t, --tag <名称>    给这次会话打标签，便于在同名进程间区分
      --truncate      本次运行前清空日志（默认追加）
      --raw           负载按原始字节写入（bash 版默认已接近原样，仅转义 CR/TAB）
      --cp <编码>     先按指定编码解码再以 UTF-8 写日志（iconv，如 GBK、GB18030）
      --text          等价于 --cp <当前 locale 字符集>
      --no-echo       包装模式下不把子进程输出回显到本终端
      --no-stdin      自身模式下不读标准输入（只记录启动参数）
      --say <文本>    自身模式：向 stdout 写一段文本（用于自测）
      --err <文本>    自身模式：向 stderr 写一段文本（用于自测）
  -q, --quiet         不打印提示信息
  -h, --help          显示本帮助
  -v, --version       显示版本

环境变量（透明代理模式下唯一的配置途径）:
  SELFLOG_FILE       日志路径（优先级高于默认位置）
  SELFLOG_TAG        会话标签
  SELFLOG_CP         按 iconv 编码名解码为 UTF-8，如 GBK
  SELFLOG_RAW=1      负载原样写入，不转义
  SELFLOG_NOECHO=1   只记日志，不回显
  SELFLOG_TRUNCATE=1 本次运行前清空日志
  SELFLOG_REAL       手工指定真身路径，覆盖 <主名>_real 约定

示例:
  selflog.sh --install ~/tools/foo        之后直接运行 foo 即可，日志在 foo 同目录
  selflog.sh --uninstall ~/tools/foo      恢复原状
  cat in.txt | selflog.sh --log run.log
  selflog.sh -- ls -la
EOF
}

# ---------------------------------------------------------------- 选项解析
parse_args() {
    local -a a=("$@")
    local n=$# i=0 v
    HAS_CHILD=0
    CHILD_ARGS=()
    ACTION=""
    ACTION_ARG=""

    while (( i < n )); do
        v="${a[i]}"
        case "$v" in
            -l|--log)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                g_log_arg="${a[i+1]}"; i=$(( i + 2 )) ;;
            -t|--tag)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                g_tag="${a[i+1]}"; i=$(( i + 2 )) ;;
            --truncate) g_truncate=1; i=$(( i + 1 )) ;;
            --raw)      g_raw=1;      i=$(( i + 1 )) ;;
            --text)
                g_cp=$(locale charmap 2>/dev/null || true)
                g_cp=${g_cp:-UTF-8}; i=$(( i + 1 )) ;;
            --cp)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                g_cp="${a[i+1]}"; i=$(( i + 2 )) ;;
            --no-echo)  g_noecho=1;  i=$(( i + 1 )) ;;
            --no-stdin) g_nostdin=1; i=$(( i + 1 )) ;;
            --say)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                g_say="${a[i+1]}"; i=$(( i + 2 )) ;;
            --err)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                g_err="${a[i+1]}"; i=$(( i + 2 )) ;;
            -q|--quiet) g_quiet=1; i=$(( i + 1 )) ;;
            -h|--help)  usage; exit 0 ;;
            -v|--version) echo "selflog $SELFLOG_VER"; exit 0 ;;
            --install)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                ACTION=install; ACTION_ARG="${a[i+1]}"; i=$(( i + 2 )) ;;
            --uninstall)
                (( i + 1 < n )) || die 2 "$v 需要一个参数"
                ACTION=uninstall; ACTION_ARG="${a[i+1]}"; i=$(( i + 2 )) ;;
            --)
                HAS_CHILD=1
                CHILD_ARGS=("${a[@]:i+1}")
                break ;;
            -*) die 2 "未知选项: $v（-h 查看帮助）" ;;
            *)  die 2 "意外的位置参数: $v —— 包装模式请用 -- 分隔（selflog.sh [选项] -- 程序 参数）" ;;
        esac
    done
    return 0
}

# ---------------------------------------------------------------- install / uninstall
do_action() {
    local mode=$1 target=$2
    local full dir base stem ext real

    full=$(readlink -f "$target" 2>/dev/null) || full=""
    [ -n "$full" ] && [ -e "$full" ] || die 2 "找不到文件: $target"
    [ "$full" != "$self_exe" ] || die 2 "这是 selflog 自己，请指定要被顶替的原始程序"

    dir=$(dirname "$full"); base=$(basename "$full")
    if [[ "$base" == *.* ]]; then
        stem="${base%.*}"; ext=".${base##*.}"
    else
        stem="$base"; ext=""
    fi
    real="$dir/${stem}_real${ext}"

    if [ "$mode" = install ]; then
        [ -e "$real" ] && die 2 "已存在 $real，可能已经装过；先用 --uninstall 恢复"
        cmp -s "$full" "$self_exe" && die 2 "$full 已经是 selflog 副本了"
        mv "$full" "$real" || die 3 "重命名失败（权限或文件占用）"
        if ! cp "$self_exe" "$full"; then
            mv "$real" "$full"
            die 3 "复制副本失败，已回滚"
        fi
        chmod --reference="$real" "$full" 2>/dev/null || chmod 755 "$full" 2>/dev/null || true
        printf '[selflog] 已安装代理\n  真身: %s\n  替身: %s\n  之后凡是以 %s 启动的进程，参数与全部 I/O 都会记录到同目录的 selflog.log\n' \
            "$real" "$full" "$stem"
    else
        [ -e "$real" ] || die 2 "找不到真身 $real，无法恢复"
        cmp -s "$full" "$self_exe" || die 2 "$full 不是当前 selflog 的副本，拒绝删除（请手工处理）"
        rm -f "$full" || die 3 "删除替身失败，请先关闭正在运行的它"
        if ! mv "$real" "$full"; then
            die 3 "替身已删除，但恢复真身失败；原文件仍在 $real"
        fi
        printf '[selflog] 已卸载代理，%s 恢复为原始程序\n' "$full"
    fi

    # 日志里留一笔，日后能对上“什么时候装的”
    resolve_log ""
    {
        log_line ""
        log_line "$(echo "$mode" | tr 'a-z' 'A-Z')  $(now_local)"
        log_kv target "$full"
        log_kv real "$real"
    } >> "$g_log"
}

# ---------------------------------------------------------------- 子进程运行
# 三个 FIFO + 三个泵：
#   stdin 泵 : 我们的 stdin -> FIFO（tee 顺路写 $tmpdir/in）
#   out/err 泵: FIFO -> 我们的 stdout/stderr（tee 顺路写 $tmpdir/out|err）
# 退出码原样透传；bash 版的 I/O 实时写进临时文件，正常退出后统一汇编进日志。
run_child() {  # "$@" = 子程序及其参数
    local rc

    tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/selflog.XXXXXX") || die 3 "无法创建临时目录"
    trap 'rm -rf "$tmpdir"' EXIT

    local fin="$tmpdir/in.fifo" fout="$tmpdir/out.fifo" ferr="$tmpdir/err.fifo"
    mkfifo "$fin" "$fout" "$ferr" || die 3 "mkfifo 失败"

    if [ "$g_noecho" = 1 ]; then
        ( cat > "$tmpdir/out" < "$fout" ) & P_OUT=$!
        ( cat > "$tmpdir/err" < "$ferr" ) & P_ERR=$!
    else
        ( tee "$tmpdir/out"  < "$fout" ) & P_OUT=$!
        ( tee "$tmpdir/err"  < "$ferr" 1>&2 ) & P_ERR=$!
    fi
    # 非交互 shell 中，后台任务的 stdin 会被自动接到 /dev/null；
    # 必须先把 stdin 复制到一个额外 fd，再显式传给后台泵
    exec {SELFLOG_INFD}<&0
    ( tee "$tmpdir/in" <&"$SELFLOG_INFD" > "$fin" ) & P_IN=$!
    exec {SELFLOG_INFD}<&-

    export SELFLOG_DEPTH=$(( g_depth + 1 ))    # 防真身指回自己造成无限递归
    "$@" < "$fin" > "$fout" 2> "$ferr" & P_CHILD=$!
    unset SELFLOG_DEPTH

    log_kv child_pid "$P_CHILD"
    log_line "--------------------------------------------------------------------------------"

    wait "$P_CHILD"; rc=$?
    # stdin 泵可能还阻塞在终端读取上，不等它（子进程已退出，数据已捕获）
    kill "$P_IN" 2>/dev/null
    wait "$P_OUT" 2>/dev/null
    wait "$P_ERR" 2>/dev/null

    return $rc
}

# ---------------------------------------------------------------- 自身模式
run_self() {
    local rc=0

    tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/selflog.XXXXXX") || die 3 "无法创建临时目录"
    trap 'rm -rf "$tmpdir"' EXIT

    # --say / --err：写屏幕的同时落进对应流文件，日志里才有 OUT/ERR 记录
    [ -n "$g_say" ] && printf '%s\n' "$g_say" | tee "$tmpdir/out"
    [ -n "$g_err" ] && printf '%s\n' "$g_err" | tee "$tmpdir/err" >&2

    if [ "$g_nostdin" = 1 ]; then
        rc=0
    else
        if [ -t 0 ] && [ "$g_quiet" != 1 ]; then
            printf '[selflog] 正在读取标准输入，输入结束后按 Ctrl+D 退出\n' >&2
        fi
        # 前台运行：stdin 原样转发到 stdout 的同时写入日志（行为等价 cat）
        tee "$tmpdir/in"; rc=$?
    fi
    log_line "--------------------------------------------------------------------------------"
    return $rc
}

# ===================================================================== main
main() {
    local rc=0 proxy=0

    [ -n "${EPOCHREALTIME:-}" ] && T_START=$EPOCHREALTIME || T_START=0
    g_depth=$(env_int SELFLOG_DEPTH 0)
    if [ "$g_depth" -gt 3 ]; then
        die 4 "代理嵌套过深（$g_depth 层），疑似 _real 指回了自己，已退出"
    fi

    # 先判透明代理，再解析命令行——顺序不能反。
    # 同目录存在 <自身主名>_real 说明自己是顶替上去的替身，此时一个参数都不能
    # 解析：命令行必须原样交给真身。配置只来自环境变量。
    if find_real; then
        proxy=1; g_mode="proxy (transparent forward)"
        g_log_arg="${SELFLOG_FILE:-}"
        g_tag="${SELFLOG_TAG:-}"
        g_cp="${SELFLOG_CP:-}"
        g_raw=$(env_flag SELFLOG_RAW)
        g_noecho=$(env_flag SELFLOG_NOECHO)
        g_truncate=$(env_flag SELFLOG_TRUNCATE)
        g_quiet=1
        SELF_ARGS=("$0" "$@")
        CHILD_ARGS=("$@")
        HAS_CHILD=1
    else
        parse_args "$@" || exit 2
        SELF_ARGS=("$0" "$@")
        if [ -n "$ACTION" ]; then
            do_action "$ACTION" "$ACTION_ARG"
            exit $?
        fi
        if [ "$HAS_CHILD" = 1 ]; then
            [ "${#CHILD_ARGS[@]}" -gt 0 ] || die 2 "-- 之后必须跟要运行的程序"
            g_mode="wrapper (explicit child)"
        else
            g_mode="self"
        fi
    fi

    if [ "$g_mode" = self ] && [ "$self_stem" != "selflog" ] && [ "$g_quiet" != 1 ]; then
        # 名字被改成别的、却又找不到 _real：多半是替身被孤立了，说一声
        printf "selflog: 同目录没有 %s_real，退化为自身模式（只记录本进程）\n" "$self_stem" >&2
    fi

    resolve_log "$g_log_arg"
    write_header "$g_mode" SELF_ARGS

    if [ "$proxy" = 1 ]; then
        log_kv proxy_of "${SELF_ARGS[0]:-}"
        log_kv real_exe "$REAL_EXE"
        log_kv fwd_argc "${#CHILD_ARGS[@]}"
        for i in "${!CHILD_ARGS[@]}"; do
            log_kv "fwd[$i]" "${CHILD_ARGS[i]}"
        done
        log_line "--------------------------------------------------------------------------------"
    elif [ "$g_mode" = "wrapper (explicit child)" ]; then
        log_line "--------------------------------------------------------------------------------"
    fi

    if [ "$proxy" = 1 ]; then
        # argv[0] 换成真身路径（与 exe 版一致），其余参数原样转发
        run_child "$REAL_EXE" "${CHILD_ARGS[@]}"; rc=$?
    elif [ "$HAS_CHILD" = 1 ]; then
        if [ -t 0 ] && [ "$g_quiet" != 1 ] && [ "$g_nostdin" != 1 ]; then
            printf '[selflog] 正在读取标准输入，输入结束后按 Ctrl+D 退出\n' >&2
        fi
        run_child "${CHILD_ARGS[@]}"; rc=$?
    else
        run_self; rc=$?
    fi

    finish_log "$rc"
    exit $rc
}

main "$@"
