#!/usr/bin/env sh
set -eu

BIN_PATH="${1:-./viper}"
SCRIPTS_DIR="${2:-tests/scripts}"
TIMEOUT_SECS="${3:-10}"
BIN_ABS="${BIN_PATH}"
case "${BIN_ABS}" in
    /*) ;;
    *) BIN_ABS="$(pwd)/${BIN_ABS}" ;;
esac

OUT_FILE="/tmp/viper-test.out"
ERR_FILE="/tmp/viper-test.err"

PASS_COUNT=0
FAIL_COUNT=0
TIMEOUT_COUNT=0
TOTAL_COUNT=0

HAS_TIMEOUT=0
if command -v timeout >/dev/null 2>&1; then
    HAS_TIMEOUT=1
fi

for script in "${SCRIPTS_DIR}"/*.vp; do
    [ -e "${script}" ] || continue
    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    printf "=== %s ===\n" "${script}"
    : > "${OUT_FILE}"
    : > "${ERR_FILE}"

    STATUS=0
    EXIT_CODE=0

    if [ "${HAS_TIMEOUT}" -eq 1 ]; then
        if timeout "${TIMEOUT_SECS}s" "${BIN_PATH}" "${script}" >"${OUT_FILE}" 2>"${ERR_FILE}"; then
            STATUS=1
        else
            EXIT_CODE=$?
            if [ "${EXIT_CODE}" -eq 124 ]; then
                STATUS=3
            else
                STATUS=2
            fi
        fi
    else
        if "${BIN_PATH}" "${script}" >"${OUT_FILE}" 2>"${ERR_FILE}"; then
            STATUS=1
        else
            EXIT_CODE=$?
            STATUS=2
        fi
    fi

    if [ "${STATUS}" -eq 1 ]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "PASS"
    elif [ "${STATUS}" -eq 3 ]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        echo "TIMEOUT"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        printf "FAIL(%s)\n" "${EXIT_CODE}"
    fi

    if [ "${STATUS}" -ne 1 ]; then
        sed -n '1,20p' "${OUT_FILE}"
        sed -n '1,20p' "${ERR_FILE}"
    fi
    echo
done

echo "=== semantic_index ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-index-json "${SCRIPTS_DIR}/mod_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "\"schema_version\"" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(index)"
    sed -n '1,20p' "${OUT_FILE}"
    sed -n '1,20p' "${ERR_FILE}"
fi
echo

echo "=== bytecode_binary ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_BYTECODE="$(mktemp /tmp/viper-bytecode-XXXXXX.vbb)"
if "${BIN_PATH}" --emit-bytecode="${TMP_BYTECODE}" "${SCRIPTS_DIR}/mod_test.vp" >/tmp/vbb-emit.out 2>/tmp/vbb-emit.err &&
   head -c 4 "${TMP_BYTECODE}" >/tmp/vbb-magic.bin &&
   printf 'VPK1' | cmp -s - /tmp/vbb-magic.bin &&
   "${BIN_PATH}" --run-bytecode="${TMP_BYTECODE}" >/tmp/vbb-run.out 2>/tmp/vbb-run.err &&
   grep -q "Module test result:" /tmp/vbb-run.out &&
   grep -q "42.00" /tmp/vbb-run.out; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(bytecode)"
    sed -n '1,30p' /tmp/vbb-emit.out 2>/dev/null || true
    sed -n '1,30p' /tmp/vbb-emit.err 2>/dev/null || true
    sed -n '1,30p' /tmp/vbb-run.out 2>/dev/null || true
    sed -n '1,30p' /tmp/vbb-run.err 2>/dev/null || true
fi
rm -f "${TMP_BYTECODE}"
echo

echo "=== vpm_core ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_DIR="$(mktemp -d /tmp/viper-vpm-XXXXXX)"
if (
    cd "${TMP_DIR}" &&
    "${BIN_ABS}" pkg init demo >/tmp/vpm-init.out 2>/tmp/vpm-init.err &&
    "${BIN_ABS}" pkg add @web 0.1.0 >/tmp/vpm-add.out 2>/tmp/vpm-add.err &&
    "${BIN_ABS}" pkg list >/tmp/vpm-list.out 2>/tmp/vpm-list.err &&
    grep -q "@web(0.1.0)" /tmp/vpm-list.out &&
    grep -q "^@web 0.1.0$" viper.lock &&
    "${BIN_ABS}" pkg install >/tmp/vpm-install.out 2>/tmp/vpm-install.err &&
    test -f .viper/packages/@web/versions/0.1.0/index.vp &&
    test -f .viper/packages/@web/versions/0.1.0/index.vbc &&
    test -f .viper/packages/@web/versions/0.1.0/index.vbb &&
    test -f .viper/packages/@web/versions/0.1.0/index.vabi &&
    head -c 4 .viper/packages/@web/versions/0.1.0/index.vbc >/tmp/vbc-magic.bin &&
    printf 'VPK1' | cmp -s - /tmp/vbc-magic.bin &&
    cat > .viper/packages/@web/versions/0.1.0/index.vp <<'EOF' &&
pr("init-web")
pub fn webPing() {
    ret 7
}
fn webSecret() {
    ret 99
}
EOF
    "${BIN_ABS}" pkg build >/tmp/vpm-build.out 2>/tmp/vpm-build.err &&
    grep -q "artifact" /tmp/vpm-build.out &&
    "${BIN_ABS}" pkg abi check >/tmp/vpm-abi-check.out 2>/tmp/vpm-abi-check.err &&
    grep -q "abi check passed" /tmp/vpm-abi-check.out &&
    grep -q "0.1.0" .viper/packages/@web/current &&
    cp -r .viper/packages/@web/versions/0.1.0 .viper/packages/@web/versions/0.2.0 &&
    printf '# viper abi v1\nfn webPing 0\nfn webExtra 1\n' > .viper/packages/@web/versions/0.2.0/index.vabi &&
    "${BIN_ABS}" pkg abi diff @web 0.1.0 0.2.0 >/tmp/vpm-abi-diff.out 2>/tmp/vpm-abi-diff.err &&
    "${BIN_ABS}" pkg abi diff @web 0.1.0 0.2.0 --fail-on-breaking >/tmp/vpm-abi-diff-nb.out 2>/tmp/vpm-abi-diff-nb.err &&
    grep -q "ADDED index.vabi fn webExtra 1" /tmp/vpm-abi-diff.out &&
    grep -q "added=1" /tmp/vpm-abi-diff.out &&
    cp -r .viper/packages/@web/versions/0.1.0 .viper/packages/@web/versions/0.3.0 &&
    printf '# viper abi v1\nfn webPing 1\n' > .viper/packages/@web/versions/0.3.0/index.vabi &&
    ! "${BIN_ABS}" pkg abi diff @web 0.1.0 0.3.0 --fail-on-breaking >/tmp/vpm-abi-diff-br.out 2>/tmp/vpm-abi-diff-br.err &&
    cat /tmp/vpm-abi-diff-br.out /tmp/vpm-abi-diff-br.err | grep -q "breaking changes detected" &&
    "${BIN_ABS}" pkg lock >/tmp/vpm-lock.out 2>/tmp/vpm-lock.err &&
    grep -q "^# viper lockfile v1$" viper.lock &&
    cat > app.vp <<'EOF' &&
use "@web" as web
pr("pkg ok")
pr(web.webPing())
EOF
    "${BIN_ABS}" app.vp >/tmp/vpm-run.out 2>/tmp/vpm-run.err &&
    grep -q "init-web" /tmp/vpm-run.out &&
    grep -q "pkg ok" /tmp/vpm-run.out &&
    grep -q "7.00" /tmp/vpm-run.out &&
    mv .viper/packages/@web/versions/0.1.0/index.vp .viper/packages/@web/versions/0.1.0/index.vp.bak &&
    "${BIN_ABS}" app.vp >/tmp/vpm-run-vbc.out 2>/tmp/vpm-run-vbc.err &&
    grep -q "init-web" /tmp/vpm-run-vbc.out &&
    grep -q "pkg ok" /tmp/vpm-run-vbc.out &&
    grep -q "7.00" /tmp/vpm-run-vbc.out &&
    cat > app_private.vp <<'EOF' &&
use "@web" as web
pr(web.webSecret())
EOF
    ! "${BIN_ABS}" app_private.vp >/tmp/vpm-run-private.out 2>/tmp/vpm-run-private.err &&
    cat /tmp/vpm-run-private.out /tmp/vpm-run-private.err | grep -q "Unknown namespaced callable" &&
    cp .viper/packages/@web/versions/0.1.0/index.vabi .viper/packages/@web/versions/0.1.0/index.vabi.bak &&
    printf '# viper abi v1\nfn missing_symbol 0\n' > .viper/packages/@web/versions/0.1.0/index.vabi &&
    ! "${BIN_ABS}" pkg abi check >/tmp/vpm-abi-check-fail.out 2>/tmp/vpm-abi-check-fail.err &&
    cat /tmp/vpm-abi-check-fail.out /tmp/vpm-abi-check-fail.err | grep -q "abi check failed" &&
    ! "${BIN_ABS}" app.vp >/tmp/vpm-run-vabi.out 2>/tmp/vpm-run-vabi.err &&
    cat /tmp/vpm-run-vabi.out /tmp/vpm-run-vabi.err | grep -q "ABI mismatch" &&
    mv .viper/packages/@web/versions/0.1.0/index.vabi.bak .viper/packages/@web/versions/0.1.0/index.vabi &&
    mv .viper/packages/@web/versions/0.1.0/index.vbc .viper/packages/@web/versions/0.1.0/index.vbc.bak &&
    "${BIN_ABS}" app.vp >/tmp/vpm-run-vbb.out 2>/tmp/vpm-run-vbb.err &&
    grep -q "init-web" /tmp/vpm-run-vbb.out &&
    grep -q "pkg ok" /tmp/vpm-run-vbb.out &&
    grep -q "7.00" /tmp/vpm-run-vbb.out &&
    "${BIN_ABS}" pkg remove @web >/tmp/vpm-rm.out 2>/tmp/vpm-rm.err &&
    test ! -d .viper/packages/@web &&
    "${BIN_ABS}" pkg list >/tmp/vpm-list2.out 2>/tmp/vpm-list2.err &&
    grep -q "(no packages)" /tmp/vpm-list2.out &&
    ! "${BIN_ABS}" app.vp >/tmp/vpm-run2.out 2>/tmp/vpm-run2.err &&
    cat /tmp/vpm-run2.out /tmp/vpm-run2.err | grep -q "Package module not found"
); then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(vpm)"
    sed -n '1,40p' /tmp/vpm-init.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-add.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-list.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-install.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-build.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-check.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-check.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff-nb.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff-nb.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff-br.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-diff-br.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-lock.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-vbc.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-private.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-private.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-check-fail.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-abi-check-fail.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-vabi.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-vabi.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-vbb.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-rm.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-list2.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run2.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run2.err 2>/dev/null || true
fi
rm -rf "${TMP_DIR}"
echo

echo "=== scale_dynamic_limits ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_SCALE="$(mktemp -d /tmp/viper-scale-XXXXXX)"
if (
    cd "${TMP_SCALE}" &&

    STAR_DIR="star80" &&
    mkdir -p "${STAR_DIR}" &&
    : > "${STAR_DIR}/main.vp" &&
    i=1 &&
    while [ "${i}" -le 80 ]; do
        mod="$(printf 'm%03d.vp' "${i}")" &&
        fn="$(printf 'f%03d' "${i}")" &&
        printf 'use "%s"\n' "${mod}" >> "${STAR_DIR}/main.vp" &&
        printf 'fn %s() {\n    ret %d\n}\n' "${fn}" "${i}" > "${STAR_DIR}/${mod}" &&
        i=$((i + 1))
    done &&
    printf '\nv total = 0\n' >> "${STAR_DIR}/main.vp" &&
    i=1 &&
    while [ "${i}" -le 80 ]; do
        fn="$(printf 'f%03d' "${i}")" &&
        printf 'total = total + %s()\n' "${fn}" >> "${STAR_DIR}/main.vp" &&
        i=$((i + 1))
    done &&
    printf 'pr("total:", total)\n' >> "${STAR_DIR}/main.vp" &&
    "${BIN_ABS}" "${STAR_DIR}/main.vp" >/tmp/scale-star.out 2>/tmp/scale-star.err &&
    grep -q "total: 3240.00" /tmp/scale-star.out &&

    CHAIN_DIR="chain70" &&
    mkdir -p "${CHAIN_DIR}" &&
    i=1 &&
    while [ "${i}" -le 70 ]; do
        mod="$(printf 'c%03d.vp' "${i}")" &&
        fn="$(printf 'c%03d' "${i}")" &&
        next_i=$((i + 1)) &&
        if [ "${i}" -lt 70 ]; then
            next_mod="$(printf 'c%03d.vp' "${next_i}")" &&
            next_fn="$(printf 'c%03d' "${next_i}")" &&
            {
                printf 'use "%s"\n\n' "${next_mod}" &&
                printf 'fn %s() {\n    ret %s() + 1\n}\n' "${fn}" "${next_fn}"
            } > "${CHAIN_DIR}/${mod}"
        else
            printf 'fn %s() {\n    ret 1\n}\n' "${fn}" > "${CHAIN_DIR}/${mod}"
        fi &&
        i=$((i + 1))
    done &&
    printf 'use "c001.vp"\npr(c001())\n' > "${CHAIN_DIR}/main.vp" &&
    "${BIN_ABS}" "${CHAIN_DIR}/main.vp" >/tmp/scale-chain.out 2>/tmp/scale-chain.err &&
    grep -q "70.00" /tmp/scale-chain.out
); then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(scale)"
    sed -n '1,40p' /tmp/scale-star.out 2>/dev/null || true
    sed -n '1,40p' /tmp/scale-star.err 2>/dev/null || true
    sed -n '1,40p' /tmp/scale-chain.out 2>/dev/null || true
    sed -n '1,40p' /tmp/scale-chain.err 2>/dev/null || true
fi
rm -rf "${TMP_SCALE}"
echo

echo "---"
printf "Total: %s | Pass: %s | Fail: %s | Timeout: %s\n" \
    "${TOTAL_COUNT}" "${PASS_COUNT}" "${FAIL_COUNT}" "${TIMEOUT_COUNT}"

if [ "${FAIL_COUNT}" -gt 0 ] || [ "${TIMEOUT_COUNT}" -gt 0 ]; then
    exit 1
fi
