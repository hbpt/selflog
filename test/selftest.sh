#!/usr/bin/env bash
# =============================================================================
# selftest.sh —— selflog.sh 回归测试
# 运行: ./selftest.sh    （全部通过输出 ALL PASSED，退出码 0）
# =============================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SL="$HERE/../dist/selflog.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if command -v cygpath >/dev/null 2>&1; then
    WORK=$(cygpath -u "$WORK")
fi
if command -v cygpath >/dev/null 2>&1; then
    WORK=$(cygpath -u "$WORK")
fi
cd "$WORK"

PASS=0; FAIL=0
ok()   { PASS=$(( PASS + 1 )); echo "[PASS] $1"; }
bad()  { FAIL=$(( FAIL + 1 )); echo "[FAIL] $1"; }
check() { # $1=描述  $2=条件命令结果(0=通过)
    if [ "$2" = 0 ]; then ok "$1"; else bad "$1"; fi
}

# ------------------------------------------------ 1. 自身模式：参数 + stdin + say/err
printf 'hello world\nsecond line\n' > in.txt
"$SL" -l t1.log --tag T1 --say out1 --err err1 < in.txt > o1.txt 2> e1.txt
rc=$?
[ "$rc" = 0 ]                                   ; check "1a 自身模式退出码" $?
grep -q "argv\[1\] *: -l" t1.log                ; check "1b 记录了启动参数" $?
grep -q "^--- IN  24 bytes ---" t1.log          ; check "1c stdin 字节数" $?
grep -q "^--- OUT 5 bytes ---" t1.log           ; check "1d --say 记入 OUT" $?
grep -q "^--- ERR 5 bytes ---" t1.log           ; check "1e --err 记入 ERR" $?
grep -q "hello world" t1.log                    ; check "1f stdin 内容可读" $?
grep -q "out1" o1.txt && grep -q "err1" e1.txt  ; check "1g say/err 落到对应流" $?

# ------------------------------------------------ 2. 包装模式：I/O 镜像 + 退出码透传
"$SL" -l t2.log --tag T2 -- bash -c 'echo child-out; echo child-err >&2; exit 7' \
    > o2.txt 2> e2.txt
rc=$?
[ "$rc" = 7 ]                                   ; check "2a 退出码透传(7)" $?
grep -q "child-out" t2.log && grep -q "child-err" t2.log ; check "2b 子进程输出落盘" $?
grep -q "child_pid" t2.log                      ; check "2c 记录 child_pid" $?
grep -q "child-out" o2.txt                      ; check "2d 输出回显到终端" $?
grep -q "child-err" e2.txt                      ; check "2e stderr 回显到终端" $?

# ------------------------------------------------ 3. 透明代理：参数原样转发
printf '#!/usr/bin/env bash\necho "real: $*"\ncat\nexit 5\n' > demo_real
chmod +x demo_real
cp "$SL" demo && chmod +x demo
SELFLOG_FILE=t3.log SELFLOG_TAG=PT ./demo --opt1 v1 < in.txt > o3.txt 2>/dev/null
rc=$?
[ "$rc" = 5 ]                                   ; check "3a 代理退出码透传(5)" $?
grep -q "fwd\[0\] *: --opt1" t3.log             ; check "3b 选项未被吞掉且留痕" $?
grep -q "real: --opt1 v1" o3.txt                ; check "3c 参数到达真身" $?
grep -q "mode *: proxy" t3.log                  ; check "3d 日志标注 proxy 模式" $?
grep -q "hello world" t3.log                    ; check "3e stdin 镜像" $?

# ------------------------------------------------ 4. install / uninstall 往返
printf '#!/usr/bin/env bash\necho "orig: $*"\nexit 3\n' > foo && chmod +x foo
H0=$(sha256sum foo | cut -d' ' -f1)
"$SL" --install foo >/dev/null 2>&1             ; check "4a install 成功" $?
[ -x foo ] && [ -f foo_real ]                   ; check "4b 真身/替身就位" $?
././foo --version > o4.txt 2>&1; rc=$?
[ "$rc" = 3 ] && grep -q "orig: --version" o4.txt ; check "4c 被顶替后行为不变" $?
[ -f selflog.log ]                              ; check "4d 默认日志生成" $?
"$SL" --install foo >/dev/null 2>&1
[ $? != 0 ]                                     ; check "4e 重复 install 被拒绝" $?
"$SL" --uninstall foo >/dev/null 2>&1           ; check "4f uninstall 成功" $?
H1=$(sha256sum foo | cut -d' ' -f1)
[ "$H0" = "$H1" ]                               ; check "4g 恢复后逐字节一致" $?

# ------------------------------------------------ 5. 递归保护
mkdir -p loop && cp "$SL" loop/l && chmod +x loop/l
SELFLOG_REAL="$PWD/loop/l" loop/l x >/dev/null 2>&1
[ $? = 4 ]                                      ; check "5a _real 指回自己时退出码 4" $?

# ------------------------------------------------ 6. 编码
printf '\xd6\xd0\xce\xc4\n' | "$SL" -q --cp GBK -l t6.log --truncate >/dev/null
grep -q "中文" t6.log                           ; check "6a --cp GBK 解码中文" $?
printf '中文输入\n' | "$SL" -q -l t6b.log --truncate >/dev/null
grep -q "中文输入" t6b.log                      ; check "6b UTF-8 默认透传" $?

# ------------------------------------------------ 7. 杂项
echo z | "$SL" -q --no-stdin -l t7.log --truncate >/dev/null
grep -q "stdin=0 " t7.log                       ; check "7a --no-stdin 不读输入" $?
"$SL" -q --no-echo -l t7b.log --truncate -- echo quiet-out > o7.txt
[ ! -s o7.txt ]                                 ; check "7b --no-echo 不回显" $?
grep -q "quiet-out" t7b.log                     ; check "7c no-echo 时仍落盘" $?
"$SL" -l t7c.log --tag a --no-stdin -q; "$SL" -l t7c.log --tag b --no-stdin -q
[ "$(grep -c '^SESSION' t7c.log)" = 2 ]         ; check "7d 多次会话追加" $?
"$SL" -q --nonexistent-opt >/dev/null 2>&1
[ $? = 2 ]                                      ; check "7e 未知选项报错(2)" $?

echo
echo "passed=$PASS failed=$FAIL"
[ "$FAIL" = 0 ] && { echo "ALL PASSED"; exit 0; }
exit 1
