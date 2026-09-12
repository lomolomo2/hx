#!/usr/bin/env bash
# Where the architectural constraint is enforced (no eslint dependency, so it
# cannot silently stop working depending on whether a plugin is installed).
#
# The host decides "should this be done", hxd decides "can this be done". If the
# host could touch the filesystem or start processes directly, that boundary
# would be nothing but a verbal agreement.
set -u
cd "$(dirname "$0")/.."

EXEMPT="src/engine/client.ts"   # the sole exemption: it brings the engine up
fail=0

scan() {
  local pattern="$1" what="$2"
  local hits
  hits=$(grep -rnE "$pattern" src --include='*.ts' | grep -v "^$EXEMPT:" || true)
  if [ -n "$hits" ]; then
    printf '  \033[31mFAIL\033[0m  host must not %s:\n' "$what"
    echo "$hits" | sed 's/^/        /'
    fail=1
  else
    printf '  \033[32mPASS\033[0m  host does not %s\n' "$what"
  fi
}

echo "architectural constraint check"
scan "from \"(node:)?fs(/promises)?\"" "touch the filesystem directly"
scan "from \"(node:)?child_process\"" "start processes"
scan "from \"(node:)?net\"|from \"(node:)?dgram\"" "open network connections directly"

# The exempt file must state its reason explicitly, so the exemption cannot be
# widened silently
if grep -q "eslint-disable no-restricted-imports" "$EXEMPT"; then
  printf '  \033[32mPASS\033[0m  exempt file %s carries an explicit marker\n' "$EXEMPT"
else
  printf '  \033[31mFAIL\033[0m  exempt file is missing its explicit marker\n'
  fail=1
fi

exit $fail
