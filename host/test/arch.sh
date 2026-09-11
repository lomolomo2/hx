#!/usr/bin/env bash
# 架构约束的强制点（不依赖 eslint，因此不会因为装没装插件而静默失效）。
#
# host 决定"该不该做"，hxd 决定"能不能做"。如果 host 能直接碰文件系统或起进程，
# 那条边界就只是口头约定。
set -u
cd "$(dirname "$0")/.."

EXEMPT="src/engine/client.ts"   # 唯一豁免：它负责把引擎拉起来
fail=0

scan() {
  local pattern="$1" what="$2"
  local hits
  hits=$(grep -rnE "$pattern" src --include='*.ts' | grep -v "^$EXEMPT:" || true)
  if [ -n "$hits" ]; then
    printf '  \033[31mFAIL\033[0m  host 不得%s：\n' "$what"
    echo "$hits" | sed 's/^/        /'
    fail=1
  else
    printf '  \033[32mPASS\033[0m  host 不%s\n' "$what"
  fi
}

echo "架构约束检查"
scan "from \"(node:)?fs(/promises)?\"" "直接碰文件系统"
scan "from \"(node:)?child_process\"" "起进程"
scan "from \"(node:)?net\"|from \"(node:)?dgram\"" "直接开网络连接"

# 豁免文件必须明确标注理由，防止豁免被无声扩大
if grep -q "eslint-disable no-restricted-imports" "$EXEMPT"; then
  printf '  \033[32mPASS\033[0m  豁免文件 %s 有显式标注\n' "$EXEMPT"
else
  printf '  \033[31mFAIL\033[0m  豁免文件缺少显式标注\n'
  fail=1
fi

exit $fail
