#!/usr/bin/env sh
set -eu

BIN_PATH="${1:-./viper}"
SCRIPTS_DIR="${2:-tests/scripts}"
TIMEOUT_SECS="${3:-10}"
CC_BIN="${CC:-gcc}"
BIN_ABS="${BIN_PATH}"
HARNESS_ROOT="$(pwd)"
VIPER_STD_DIR="${VIPER_STD_PATH:-${HARNESS_ROOT}/lib/std}"
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

    script_dir="$(dirname "${script}")"
    script_name="$(basename "${script}")"
    sidecar_c="${script_dir}/$(basename "${script}" .vp).c"
    sidecar_so="${script_dir}/$(basename "${script}" .vp).so"
    if [ -f "${sidecar_c}" ]; then
        if ! "${CC_BIN}" -shared -fPIC -o "${sidecar_so}" "${sidecar_c}" >"${OUT_FILE}" 2>"${ERR_FILE}"; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            printf "FAIL(build-fixture)\n"
            sed -n '1,20p' "${OUT_FILE}"
            sed -n '1,20p' "${ERR_FILE}"
            echo
            continue
        fi
        : > "${OUT_FILE}"
        : > "${ERR_FILE}"
    fi

    STATUS=0
    EXIT_CODE=0

    if [ "${HAS_TIMEOUT}" -eq 1 ]; then
        if (
            cd "${script_dir}" &&
            VIPER_STD_PATH="${VIPER_STD_DIR}" timeout "${TIMEOUT_SECS}s" "${BIN_ABS}" "${script_name}"
        ) >"${OUT_FILE}" 2>"${ERR_FILE}"; then
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
        if (
            cd "${script_dir}" &&
            VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" "${script_name}"
        ) >"${OUT_FILE}" 2>"${ERR_FILE}"; then
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

echo "=== typecheck_failures ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_TYPE_DIR="$(mktemp -d /tmp/viper-typecheck-XXXXXX)"
TYPECHECK_OK=1

cat > "${TMP_TYPE_DIR}/var_mismatch.vp" <<'EOF'
var bad: str = 12
EOF

cat > "${TMP_TYPE_DIR}/assign_mismatch.vp" <<'EOF'
var bad: int = 1
bad = "oops"
EOF

cat > "${TMP_TYPE_DIR}/return_mismatch.vp" <<'EOF'
fn nope() -> int {
    ret "oops"
}

nope()
EOF

if ! (
    cd "${TMP_TYPE_DIR}" &&
    ! VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" var_mismatch.vp >/tmp/viper-type-var.out 2>/tmp/viper-type-var.err &&
    cat /tmp/viper-type-var.out /tmp/viper-type-var.err | grep -q "Type mismatch for variable 'bad'" &&
    ! VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" assign_mismatch.vp >/tmp/viper-type-assign.out 2>/tmp/viper-type-assign.err &&
    cat /tmp/viper-type-assign.out /tmp/viper-type-assign.err | grep -q "Type mismatch for assignment 'bad'" &&
    ! VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" return_mismatch.vp >/tmp/viper-type-return.out 2>/tmp/viper-type-return.err &&
    cat /tmp/viper-type-return.out /tmp/viper-type-return.err | grep -q "Type mismatch for return in function 'nope'"
); then
    TYPECHECK_OK=0
fi

if [ "${TYPECHECK_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(typecheck)"
    sed -n '1,20p' /tmp/viper-type-var.out 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-type-var.err 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-type-assign.out 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-type-assign.err 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-type-return.out 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-type-return.err 2>/dev/null || true
fi
rm -rf "${TMP_TYPE_DIR}"
echo

echo "=== effect_verification ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_EFFECT_DIR="$(mktemp -d /tmp/viper-effects-XXXXXX)"
EFFECT_OK=1

cat > "${TMP_EFFECT_DIR}/missing_panic.vp" <<'EOF'
@effect(os)
fn bad() -> str {
    panic("boom")
    ret os_env("USER")
}
EOF

cat > "${TMP_EFFECT_DIR}/missing_ffi.vp" <<'EOF'
@effect(os)
fn open_plugin(path) {
    ret load_dl(path)
}
EOF

if ! (
    cd "${TMP_EFFECT_DIR}" &&
    ! VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" missing_panic.vp >/tmp/viper-effect-panic.out 2>/tmp/viper-effect-panic.err &&
    cat /tmp/viper-effect-panic.out /tmp/viper-effect-panic.err | grep -q "Effect mismatch for function 'bad'" &&
    cat /tmp/viper-effect-panic.out /tmp/viper-effect-panic.err | grep -q "panic" &&
    ! VIPER_STD_PATH="${VIPER_STD_DIR}" "${BIN_ABS}" missing_ffi.vp >/tmp/viper-effect-ffi.out 2>/tmp/viper-effect-ffi.err &&
    cat /tmp/viper-effect-ffi.out /tmp/viper-effect-ffi.err | grep -q "Effect mismatch for function 'open_plugin'" &&
    cat /tmp/viper-effect-ffi.out /tmp/viper-effect-ffi.err | grep -q "ffi"
); then
    EFFECT_OK=0
fi

if [ "${EFFECT_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(effect-verification)"
    sed -n '1,20p' /tmp/viper-effect-panic.out 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-effect-panic.err 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-effect-ffi.out 2>/dev/null || true
    sed -n '1,20p' /tmp/viper-effect-ffi.err 2>/dev/null || true
fi
rm -rf "${TMP_EFFECT_DIR}"
echo

echo "=== semantic_index ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-index-json "${SCRIPTS_DIR}/semantic_effects_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "\"schema_version\"" "${OUT_FILE}" &&
   grep -q "\"declared_effects\": \\[\"os\", \"auth\"\\]" "${OUT_FILE}" &&
   grep -q "\"inferred_effects\": \\[\"os\"\\]" "${OUT_FILE}" &&
   grep -q "\"target\": \"os_env\"" "${OUT_FILE}" &&
   grep -q "\"return_type\": \"str\"" "${OUT_FILE}" &&
   grep -q "\"effects\": \\[\"os\", \"auth\", \"ffi\", \"panic\"\\]" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(index)"
    sed -n '1,20p' "${OUT_FILE}"
    sed -n '1,20p' "${ERR_FILE}"
fi
echo

echo "=== context_pack ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-context-pack "${SCRIPTS_DIR}/semantic_effects_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^CTXv1$" "${OUT_FILE}" &&
   grep -q "^summary: modules=1 vars=1 fns=3 structs=0 imports=0 calls=3 effects=4$" "${OUT_FILE}" &&
   grep -q "^effects: os,auth,ffi,panic$" "${OUT_FILE}" &&
   grep -q "^  pub fn read_user(user_id) -> str effects=os,auth inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   grep -q "^  fn open_plugin(path) effects=ffi inferred=ffi calls=load_dl/1$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(ctx)"
    sed -n '1,40p' "${OUT_FILE}"
    sed -n '1,40p' "${ERR_FILE}"
fi
echo

echo "=== focused_context_pack ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-context-pack --focus=read_user "${SCRIPTS_DIR}/semantic_effects_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^CTXv1$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^summary: modules=1 vars=0 fns=1 structs=0 imports=0 calls=1 effects=2$" "${OUT_FILE}" &&
   grep -q "^effects: os,auth$" "${OUT_FILE}" &&
   grep -q "^  pub fn read_user(user_id) -> str effects=os,auth inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   ! grep -q "^  fn open_plugin(path) effects=ffi inferred=ffi calls=load_dl/1$" "${OUT_FILE}" &&
   ! grep -q "^  fn risky() effects=- inferred=panic calls=panic/1$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(focus-ctx)"
    sed -n '1,40p' "${OUT_FILE}"
    sed -n '1,40p' "${ERR_FILE}"
fi
echo

echo "=== impact_context_pack ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-context-pack --focus=read_user --impact "${SCRIPTS_DIR}/impact_context_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^CTXv1$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^summary: modules=1 vars=0 fns=1 structs=0 imports=0 calls=1 effects=1$" "${OUT_FILE}" &&
   grep -q "^impact: modules=2 callers=3$" "${OUT_FILE}" &&
   grep -q "^  pub fn read_user(user_id) -> str effects=os inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   grep -q "^callers:$" "${OUT_FILE}" &&
   grep -q "^  tests/scripts/impact_lib.vp::pub fn format_user(user_id) -> str effects=- inferred=- calls=read_user/1$" "${OUT_FILE}" &&
   grep -q "^  tests/scripts/impact_context_test.vp::fn route(user_id) -> str effects=- inferred=- calls=user.format_user/1$" "${OUT_FILE}" &&
   grep -q "^  tests/scripts/impact_context_test.vp::fn render(user_id) -> str effects=- inferred=- calls=route/1$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(impact-ctx)"
    sed -n '1,80p' "${OUT_FILE}"
    sed -n '1,40p' "${ERR_FILE}"
fi
echo

echo "=== project_state ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-project-state --focus=read_user --impact "${SCRIPTS_DIR}/impact_context_test.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^PSTATEv1$" "${OUT_FILE}" &&
   grep -q "^scope: focused$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^impact: yes$" "${OUT_FILE}" &&
   grep -q "^tracked_modules: 2$" "${OUT_FILE}" &&
   grep -q "^semantic_items: 4$" "${OUT_FILE}" &&
   grep -q "^semantic_fingerprint: " "${OUT_FILE}" &&
   grep -q "^  tests/scripts/impact_context_test.vp sha256=" "${OUT_FILE}" &&
   grep -q "^  tests/scripts/impact_lib.vp sha256=" "${OUT_FILE}" &&
   grep -q "^symbol_modules:$" "${OUT_FILE}" &&
   grep -Eq "^  m[0-9a-f]{16} tests/scripts/impact_lib.vp$" "${OUT_FILE}" &&
   grep -Eq "^  m[0-9a-f]{16} tests/scripts/impact_context_test.vp$" "${OUT_FILE}" &&
   grep -q "^symbol_index:$" "${OUT_FILE}" &&
   grep -Eq "^  s[0-9a-f]{16} T m[0-9a-f]{16} pub fn read_user\\(user_id\\) -> str effects=os inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   grep -q "^summary_items:$" "${OUT_FILE}" &&
   grep -q "^  target tests/scripts/impact_lib.vp::pub fn read_user(user_id) -> str effects=os inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   grep -q "^context:$" "${OUT_FILE}" &&
   grep -q "^CTXv1$" "${OUT_FILE}" &&
   grep -q "^impact: modules=2 callers=3$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(project-state)"
    sed -n '1,120p' "${OUT_FILE}"
    sed -n '1,40p' "${ERR_FILE}"
fi
echo

echo "=== project_state_test_links ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TEST_LINK_STATE="$(mktemp /tmp/viper-test-links-XXXXXX.vstate)"
if "${BIN_ABS}" --emit-project-state="${TEST_LINK_STATE}" --focus=read_user --impact "tests/fixtures/semantic_diff_after.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^tracked_tests:$" "${TEST_LINK_STATE}" &&
   grep -q "^  tests/scripts/semantic_link_test.vp sha256=" "${TEST_LINK_STATE}" &&
   grep -q "^test_index:$" "${TEST_LINK_STATE}" &&
   grep -Eq "^  tests/scripts/semantic_link_test.vp covers=s[0-9a-f]{16},s[0-9a-f]{16}$" "${TEST_LINK_STATE}" &&
   "${BIN_ABS}" --resume-project-state="${TEST_LINK_STATE}" --focus=read_user --impact >/tmp/vresume-tests.out 2>/tmp/vresume-tests.err &&
   grep -q "^PRESUMEv6$" /tmp/vresume-tests.out &&
   grep -q "^resume_focus: read_user$" /tmp/vresume-tests.out &&
   grep -q "^resume_impact: yes$" /tmp/vresume-tests.out &&
   grep -q "^tests:$" /tmp/vresume-tests.out &&
   grep -q "^  1|test=tests/scripts/semantic_link_test.vp|hits=2|symbols=read_user,route$" /tmp/vresume-tests.out &&
   "${BIN_ABS}" --query-project-state="${TEST_LINK_STATE}" --query-name=read_user --impact >/tmp/vquery-tests.out 2>/tmp/vquery-tests.err &&
   grep -q "^PQUERYv3$" /tmp/vquery-tests.out &&
   grep -q "^query_impact: yes$" /tmp/vquery-tests.out &&
   grep -q "^tests:$" /tmp/vquery-tests.out &&
   grep -q "^  1|test=tests/scripts/semantic_link_test.vp|hits=2|symbols=read_user,route$" /tmp/vquery-tests.out; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(project-state-test-links)"
    sed -n '1,140p' "${OUT_FILE}"
    sed -n '1,80p' "${ERR_FILE}"
    sed -n '1,120p' /tmp/vresume-tests.out 2>/dev/null || true
    sed -n '1,80p' /tmp/vresume-tests.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-tests.out 2>/dev/null || true
    sed -n '1,80p' /tmp/vquery-tests.err 2>/dev/null || true
fi
rm -f "${TEST_LINK_STATE}"
echo

echo "=== state_plan ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_STATE_PLAN_DIR="$(mktemp -d /tmp/viper-state-plan-XXXXXX)"
mkdir -p "${TMP_STATE_PLAN_DIR}/tests/scripts"
cp "tests/fixtures/semantic_diff_before.vp" "${TMP_STATE_PLAN_DIR}/semantic_diff_before.vp"
cp "tests/fixtures/semantic_diff_after.vp" "${TMP_STATE_PLAN_DIR}/semantic_diff_after.vp"
cp "tests/scripts/semantic_link_test.vp" "${TMP_STATE_PLAN_DIR}/tests/scripts/semantic_link_test.vp"
STATE_PLAN_BEFORE="${TMP_STATE_PLAN_DIR}/before.vstate"
STATE_PLAN_AFTER="${TMP_STATE_PLAN_DIR}/after.vstate"
STATE_PLAN_OK=1
if ! "${BIN_ABS}" --emit-project-state="${STATE_PLAN_BEFORE}" --focus=read_user --impact "${TMP_STATE_PLAN_DIR}/semantic_diff_before.vp" >/tmp/vstate-plan-before.out 2>/tmp/vstate-plan-before.err; then
    STATE_PLAN_OK=0
fi
if [ "${STATE_PLAN_OK}" -eq 1 ] && ! "${BIN_ABS}" --emit-project-state="${STATE_PLAN_AFTER}" --focus=read_user --impact "${TMP_STATE_PLAN_DIR}/semantic_diff_after.vp" >/tmp/vstate-plan-after.out 2>/tmp/vstate-plan-after.err; then
    STATE_PLAN_OK=0
fi
if [ "${STATE_PLAN_OK}" -eq 1 ]; then
    cat > "${TMP_STATE_PLAN_DIR}/semantic_diff_before.vp" <<'EOF'
fn broken(
EOF
    cat > "${TMP_STATE_PLAN_DIR}/semantic_diff_after.vp" <<'EOF'
fn broken(
EOF
    cat > "${TMP_STATE_PLAN_DIR}/tests/scripts/semantic_link_test.vp" <<'EOF'
fn broken(
EOF
fi
if [ "${STATE_PLAN_OK}" -eq 1 ] && "${BIN_ABS}" --emit-state-plan="${STATE_PLAN_BEFORE}" "${STATE_PLAN_AFTER}" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^PSTATEPLANv1$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^impact: yes$" "${OUT_FILE}" &&
   grep -q "^summary: before=2 after=3 added=2 removed=1 unchanged=1$" "${OUT_FILE}" &&
   grep -q "^change_plan:$" "${OUT_FILE}" &&
   grep -q "^  1|status=changed|kind=target|name=read_user|score=35|blast=2|depfx=os,panic|checks=contract,effects,callers$" "${OUT_FILE}" &&
   grep -q "^  2|status=added|kind=caller|name=route|score=12|blast=0|depfx=os,panic|checks=new_surface,effects$" "${OUT_FILE}" &&
   grep -q "^test_plan:$" "${OUT_FILE}" &&
   grep -q "^  1|test=tests/scripts/semantic_link_test.vp|hits=2|max_score=35|symbols=read_user,route|checks=contract,new_surface,effects,callers$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(state-plan)"
    sed -n '1,140p' "${OUT_FILE}"
    sed -n '1,80p' "${ERR_FILE}"
    sed -n '1,80p' /tmp/vstate-plan-before.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-plan-after.err 2>/dev/null || true
fi
rm -rf "${TMP_STATE_PLAN_DIR}"
echo

echo "=== state_plan_run ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_STATE_RUN_DIR="$(mktemp -d /tmp/viper-state-run-XXXXXX)"
mkdir -p "${TMP_STATE_RUN_DIR}/tests/scripts"
cp "tests/fixtures/semantic_diff_before.vp" "${TMP_STATE_RUN_DIR}/semantic_diff_before.vp"
cp "tests/fixtures/semantic_diff_after.vp" "${TMP_STATE_RUN_DIR}/semantic_diff_after.vp"
cp "tests/scripts/semantic_link_test.vp" "${TMP_STATE_RUN_DIR}/tests/scripts/semantic_link_test.vp"
STATE_RUN_BEFORE="${TMP_STATE_RUN_DIR}/before.vstate"
STATE_RUN_AFTER="${TMP_STATE_RUN_DIR}/after.vstate"
STATE_RUN_OK=1
if ! "${BIN_ABS}" --emit-project-state="${STATE_RUN_BEFORE}" --focus=read_user --impact "${TMP_STATE_RUN_DIR}/semantic_diff_before.vp" >/tmp/vstate-run-before.out 2>/tmp/vstate-run-before.err; then
    STATE_RUN_OK=0
fi
if [ "${STATE_RUN_OK}" -eq 1 ] && ! "${BIN_ABS}" --emit-project-state="${STATE_RUN_AFTER}" --focus=read_user --impact "${TMP_STATE_RUN_DIR}/semantic_diff_after.vp" >/tmp/vstate-run-after.out 2>/tmp/vstate-run-after.err; then
    STATE_RUN_OK=0
fi
if [ "${STATE_RUN_OK}" -eq 1 ]; then
    cat > "${TMP_STATE_RUN_DIR}/semantic_diff_before.vp" <<'EOF'
fn broken(
EOF
    cat > "${TMP_STATE_RUN_DIR}/semantic_diff_after.vp" <<'EOF'
fn broken(
EOF
fi
if [ "${STATE_RUN_OK}" -eq 1 ] && "${BIN_ABS}" --run-state-plan="${STATE_RUN_BEFORE}" "${STATE_RUN_AFTER}" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^PSTATEEXECv2$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^impact: yes$" "${OUT_FILE}" &&
   grep -q "^semantic_fingerprint: " "${OUT_FILE}" &&
   grep -q "^tests_planned: 1$" "${OUT_FILE}" &&
   grep -q "^results:$" "${OUT_FILE}" &&
   grep -q "^  1|test=tests/scripts/semantic_link_test.vp|status=pass|exit=0|hits=2|max_score=35|symbols=read_user,route|checks=contract,new_surface,effects,callers$" "${OUT_FILE}" &&
   grep -q "^tests_failed: 0$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(state-plan-run)"
    sed -n '1,140p' "${OUT_FILE}"
    sed -n '1,80p' "${ERR_FILE}"
    sed -n '1,80p' /tmp/vstate-run-before.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-run-after.err 2>/dev/null || true
fi
rm -rf "${TMP_STATE_RUN_DIR}"
echo

echo "=== state_proof_resume ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_STATE_PROOF_DIR="$(mktemp -d /tmp/viper-state-proof-XXXXXX)"
mkdir -p "${TMP_STATE_PROOF_DIR}/tests/scripts"
cp "tests/fixtures/semantic_diff_before.vp" "${TMP_STATE_PROOF_DIR}/semantic_diff_before.vp"
cp "tests/fixtures/semantic_diff_after.vp" "${TMP_STATE_PROOF_DIR}/semantic_diff_after.vp"
cp "tests/scripts/semantic_link_test.vp" "${TMP_STATE_PROOF_DIR}/tests/scripts/semantic_link_test.vp"
STATE_PROOF_BEFORE="${TMP_STATE_PROOF_DIR}/before.vstate"
STATE_PROOF_AFTER="${TMP_STATE_PROOF_DIR}/after.vstate"
STATE_PROOF_OK=1
if ! "${BIN_ABS}" --emit-project-state="${STATE_PROOF_BEFORE}" --focus=read_user --impact "${TMP_STATE_PROOF_DIR}/semantic_diff_before.vp" >/tmp/vstate-proof-before.out 2>/tmp/vstate-proof-before.err; then
    STATE_PROOF_OK=0
fi
if [ "${STATE_PROOF_OK}" -eq 1 ] && ! "${BIN_ABS}" --emit-project-state="${STATE_PROOF_AFTER}" --focus=read_user --impact "${TMP_STATE_PROOF_DIR}/semantic_diff_after.vp" >/tmp/vstate-proof-after.out 2>/tmp/vstate-proof-after.err; then
    STATE_PROOF_OK=0
fi
if [ "${STATE_PROOF_OK}" -eq 1 ] && ! "${BIN_ABS}" --run-state-plan="${STATE_PROOF_BEFORE}" "${STATE_PROOF_AFTER}" > "${STATE_PROOF_AFTER}.vproof" 2>/tmp/vstate-proof-run.err; then
    STATE_PROOF_OK=0
fi
if [ "${STATE_PROOF_OK}" -eq 1 ] && "${BIN_ABS}" --resume-project-state="${STATE_PROOF_AFTER}" --focus=read_user --impact >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^verified: yes$" "${OUT_FILE}" &&
   grep -q "^verified_tests: 1$" "${OUT_FILE}" &&
   grep -q "^verified_failed: 0$" "${OUT_FILE}" &&
   grep -q "^verification:$" "${OUT_FILE}" &&
   grep -q "^  1|tests/scripts/semantic_link_test.vp status=pass$" "${OUT_FILE}" &&
   "${BIN_ABS}" --query-project-state="${STATE_PROOF_AFTER}" --query-name=read_user --impact >/tmp/vproof-query.out 2>/tmp/vproof-query.err &&
   grep -q "^verified: yes$" /tmp/vproof-query.out &&
   grep -q "^verified_tests: 1$" /tmp/vproof-query.out &&
   grep -q "^verified_failed: 0$" /tmp/vproof-query.out &&
   grep -q "^  1|tests/scripts/semantic_link_test.vp status=pass$" /tmp/vproof-query.out; then
    cat > "${TMP_STATE_PROOF_DIR}/semantic_diff_after.vp" <<'EOF'
@effect(os)
pub fn read_user(user_id) -> str {
    panic("boom")
    ret os_env(user_id)
}

fn render(user_id) -> str {
    panic("later")
    ret read_user(user_id)
}

fn route(user_id) -> str {
    ret read_user(user_id)
}
EOF
    if "${BIN_ABS}" --resume-project-state="${STATE_PROOF_AFTER}" --focus=read_user --impact >/tmp/vproof-stale-resume.out 2>/tmp/vproof-stale-resume.err &&
       grep -q "^valid: no$" /tmp/vproof-stale-resume.out &&
       grep -q "^verified: yes$" /tmp/vproof-stale-resume.out &&
       grep -q "^verification_stale: yes$" /tmp/vproof-stale-resume.out &&
       grep -q "^verification_stale_symbols:$" /tmp/vproof-stale-resume.out &&
       grep -A4 "^verification_stale_symbols:$" /tmp/vproof-stale-resume.out | grep -Eq "^  s[0-9a-f]{16}$" &&
       grep -q "^verification_stale_tests:$" /tmp/vproof-stale-resume.out &&
       grep -q "^  tests/scripts/semantic_link_test.vp$" /tmp/vproof-stale-resume.out &&
       "${BIN_ABS}" --query-project-state="${STATE_PROOF_AFTER}" --query-name=read_user --impact >/tmp/vproof-stale-query.out 2>/tmp/vproof-stale-query.err &&
       grep -q "^valid: no$" /tmp/vproof-stale-query.out &&
       grep -q "^verified: yes$" /tmp/vproof-stale-query.out &&
       grep -q "^verification_stale: yes$" /tmp/vproof-stale-query.out &&
       grep -q "^  tests/scripts/semantic_link_test.vp$" /tmp/vproof-stale-query.out &&
       "${BIN_ABS}" --verify-project-state="${STATE_PROOF_AFTER}" >/tmp/vproof-stale-check.out 2>/tmp/vproof-stale-check.err &&
       grep -q "^valid: no$" /tmp/vproof-stale-check.out &&
       grep -q "^verified: yes$" /tmp/vproof-stale-check.out &&
       grep -q "^verification_stale: yes$" /tmp/vproof-stale-check.out &&
       grep -q "^  tests/scripts/semantic_link_test.vp$" /tmp/vproof-stale-check.out &&
       "${BIN_ABS}" --refresh-project-state="${STATE_PROOF_AFTER}" >/tmp/vproof-stale-refresh.out 2>/tmp/vproof-stale-refresh.err &&
       grep -q "^status: refreshed$" /tmp/vproof-stale-refresh.out &&
       grep -q "^verified: yes$" /tmp/vproof-stale-refresh.out &&
       grep -q "^verification_stale: yes$" /tmp/vproof-stale-refresh.out &&
       grep -q "^  tests/scripts/semantic_link_test.vp$" /tmp/vproof-stale-refresh.out &&
       grep -q "^proof: refreshed$" /tmp/vproof-stale-refresh.out &&
       grep -q "^proof_tests: 1$" /tmp/vproof-stale-refresh.out &&
       grep -q "^proof_failed: 0$" /tmp/vproof-stale-refresh.out &&
       "${BIN_ABS}" --resume-project-state="${STATE_PROOF_AFTER}" --focus=read_user --impact >/tmp/vproof-fresh-resume.out 2>/tmp/vproof-fresh-resume.err &&
       grep -q "^valid: yes$" /tmp/vproof-fresh-resume.out &&
       grep -q "^verified: yes$" /tmp/vproof-fresh-resume.out &&
       ! grep -q "^verification_stale: yes$" /tmp/vproof-fresh-resume.out &&
       grep -q "^  1|tests/scripts/semantic_link_test.vp status=pass$" /tmp/vproof-fresh-resume.out; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "PASS"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "FAIL(state-proof-resume)"
        sed -n '1,160p' "${OUT_FILE}"
        sed -n '1,80p' "${ERR_FILE}"
        sed -n '1,120p' "${STATE_PROOF_AFTER}.vproof" 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-query.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-query.err 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-stale-resume.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-stale-resume.err 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-stale-query.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-stale-query.err 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-stale-check.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-stale-check.err 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-stale-refresh.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-stale-refresh.err 2>/dev/null || true
        sed -n '1,120p' /tmp/vproof-fresh-resume.out 2>/dev/null || true
        sed -n '1,80p' /tmp/vproof-fresh-resume.err 2>/dev/null || true
        sed -n '1,80p' /tmp/vstate-proof-run.err 2>/dev/null || true
    fi
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(state-proof-resume)"
    sed -n '1,160p' "${OUT_FILE}"
    sed -n '1,80p' "${ERR_FILE}"
    sed -n '1,120p' "${STATE_PROOF_AFTER}.vproof" 2>/dev/null || true
    sed -n '1,80p' /tmp/vproof-query.out 2>/dev/null || true
    sed -n '1,80p' /tmp/vproof-query.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-proof-run.err 2>/dev/null || true
fi
rm -rf "${TMP_STATE_PROOF_DIR}"
echo

echo "=== project_state_refresh ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_STATE_DIR="$(mktemp -d /tmp/viper-state-XXXXXX)"
STATE_OK=1
cp "${SCRIPTS_DIR}/impact_lib.vp" "${TMP_STATE_DIR}/impact_lib.vp"
cat > "${TMP_STATE_DIR}/entry.vp" <<'EOF'
use "./impact_lib.vp" as user
use "./noisy.vp" as noisy

fn route(user_id) -> str {
    ret user.format_user(user_id)
}

fn render(user_id) -> str {
    ret route(user_id)
}

pr(render("HOME"))
EOF
cat > "${TMP_STATE_DIR}/noisy.vp" <<'EOF'
fn dead_branch() -> int {
    ret 1
}
EOF
STATE_FILE="${TMP_STATE_DIR}/agent.vstate"
if ! "${BIN_ABS}" --emit-project-state="${STATE_FILE}" --focus=read_user --impact "${TMP_STATE_DIR}/entry.vp" >/tmp/vstate-emit.out 2>/tmp/vstate-emit.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --verify-project-state="${STATE_FILE}" >/tmp/vstate-check-1.out 2>/tmp/vstate-check-1.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^valid: yes$" /tmp/vstate-check-1.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ]; then
    cat > "${TMP_STATE_DIR}/impact_lib.vp" <<'EOF'
@effect(os)
pub fn read_user(user_id) -> str {
    panic("boom")
    ret os_env(user_id)
}

pub fn format_user(user_id) -> str {
    ret read_user(user_id)
}
EOF
    cat > "${TMP_STATE_DIR}/noisy.vp" <<'EOF'
fn dead_branch( {
EOF
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --verify-project-state="${STATE_FILE}" >/tmp/vstate-check-2.out 2>/tmp/vstate-check-2.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^valid: no$" /tmp/vstate-check-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^changed_files: 1$" /tmp/vstate-check-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^stale_modules:$" /tmp/vstate-check-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -Eq "^  m[0-9a-f]{16} .*impact_lib.vp$" /tmp/vstate-check-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^stale_symbols:$" /tmp/vstate-check-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && [ "$(grep -Ec "^  s[0-9a-f]{16}$" /tmp/vstate-check-2.out)" -ne 2 ]; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --refresh-project-state="${STATE_FILE}" >/tmp/vstate-refresh.out 2>/tmp/vstate-refresh.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^PSTATEREFRESHv3$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^changed_files: 1$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^status: refreshed$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patched_modules: 1$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patch_mods:$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -Eq "^  m[0-9a-f]{16} .*impact_lib.vp$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patch_syms:$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|T\\|m[0-9a-f]{16}\\|\\+\\|read_user\\(user_id\\)\\|r1\\|e1\\|e2,e1\\|c1,c2$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && grep -Eq "^  s[0-9a-f]{16}\\|C\\|m[0-9a-f]{16}\\|\\+\\|format_user\\(user_id\\)\\|r1\\|-\\|-\\|c3$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patched_symbols: 1$" /tmp/vstate-refresh.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --verify-project-state="${STATE_FILE}" >/tmp/vstate-check-3.out 2>/tmp/vstate-check-3.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^valid: yes$" /tmp/vstate-check-3.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ]; then
    printf '\n// metadata-only change\n' >> "${TMP_STATE_DIR}/impact_lib.vp"
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --verify-project-state="${STATE_FILE}" >/tmp/vstate-check-4.out 2>/tmp/vstate-check-4.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^valid: no$" /tmp/vstate-check-4.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --refresh-project-state="${STATE_FILE}" >/tmp/vstate-refresh-2.out 2>/tmp/vstate-refresh-2.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^PSTATEREFRESHv3$" /tmp/vstate-refresh-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^status: rehashed$" /tmp/vstate-refresh-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patched_modules: 0$" /tmp/vstate-refresh-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^patched_symbols: 0$" /tmp/vstate-refresh-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && grep -q "^patch_syms:$" /tmp/vstate-refresh-2.out; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! "${BIN_ABS}" --verify-project-state="${STATE_FILE}" >/tmp/vstate-check-5.out 2>/tmp/vstate-check-5.err; then
    STATE_OK=0
fi
if [ "${STATE_OK}" -eq 1 ] && ! grep -q "^valid: yes$" /tmp/vstate-check-5.out; then
    STATE_OK=0
fi

if [ "${STATE_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(project-state-refresh)"
    sed -n '1,80p' /tmp/vstate-emit.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-emit.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-check-1.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-check-1.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-check-2.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-check-2.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-refresh.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-refresh.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-check-3.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-check-3.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-check-4.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-check-4.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-refresh-2.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-refresh-2.err 2>/dev/null || true
    sed -n '1,80p' /tmp/vstate-check-5.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vstate-check-5.err 2>/dev/null || true
fi
rm -rf "${TMP_STATE_DIR}"
echo

echo "=== resume_project_state ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_RESUME_DIR="$(mktemp -d /tmp/viper-resume-XXXXXX)"
RESUME_OK=1
cp "${SCRIPTS_DIR}/impact_context_test.vp" "${TMP_RESUME_DIR}/entry.vp"
cp "${SCRIPTS_DIR}/impact_lib.vp" "${TMP_RESUME_DIR}/impact_lib.vp"
RESUME_STATE="${TMP_RESUME_DIR}/agent.vstate"
if ! "${BIN_ABS}" --emit-project-state="${RESUME_STATE}" --focus=read_user --impact "${TMP_RESUME_DIR}/entry.vp" >/tmp/vresume-emit.out 2>/tmp/vresume-emit.err; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! "${BIN_ABS}" --resume-project-state="${RESUME_STATE}" >/tmp/vresume-1.out 2>/tmp/vresume-1.err; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^PRESUMEv4$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^valid: yes$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^semantic_items: 4$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^semantic_fingerprint: " /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^mods:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^rets:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^effs:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^calls:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^syms:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "^symbol_index:$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^  r1=str$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^  e1=os$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|C\\|m[0-9a-f]{16}\\|-\\|route\\(user_id\\)\\|r1\\|-\\|-\\|c2$" /tmp/vresume-1.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! "${BIN_ABS}" --resume-project-state="${RESUME_STATE}" --focus=read_user >/tmp/vresume-focus.out 2>/tmp/vresume-focus.err; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^PRESUMEv6$" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^resume_focus: read_user$" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^resume_impact: no$" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && [ "$(grep -Ec "^  s[0-9a-f]{16}\\|" /tmp/vresume-focus.out)" -ne 1 ]; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|T\\|m[0-9a-f]{16}\\|\\+\\|read_user\\(user_id\\)\\|r1\\|e1\\|e1\\|c1$" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "route(user_id)" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "format_user(user_id)" /tmp/vresume-focus.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! "${BIN_ABS}" --resume-project-state="${RESUME_STATE}" --focus=read_user --impact --brief >/tmp/vresume-brief.out 2>/tmp/vresume-brief.err; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^brief: yes$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^mods:$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^brief_syms:$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^ledger_bytes: [1-9][0-9]*$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^ledger_full_bytes: [1-9][0-9]*$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^ledger_saved_bytes: [1-9][0-9]*$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^ledger_saved_pct: [1-9][0-9]*$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "^rets:$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "^syms:$" /tmp/vresume-brief.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && [ "$(grep -Ec "^  [0-9]+\\|s[0-9a-f]{16}\\|[TC]\\|m[0-9a-f]{16}\\|[^|]+\\|[^|]+\\|[+-]\\|[^|]*\\|[0-9]+$" /tmp/vresume-brief.out)" -ne 4 ]; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ]; then
    cat > "${TMP_RESUME_DIR}/impact_lib.vp" <<'EOF'
@effect(os)
pub fn read_user(user_id) -> str {
    panic("boom")
    ret os_env(user_id)
}

pub fn format_user(user_id) -> str {
    ret read_user(user_id)
}
EOF
fi
if [ "${RESUME_OK}" -eq 1 ] && ! "${BIN_ABS}" --resume-project-state="${RESUME_STATE}" >/tmp/vresume-2.out 2>/tmp/vresume-2.err; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^valid: no$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^changed_files: 1$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^stale_files:$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^stale_modules:$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -Eq "^  m[0-9a-f]{16} .*impact_lib.vp$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^stale_symbols:$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && [ "$(grep -Ec "^  s[0-9a-f]{16}$" /tmp/vresume-2.out)" -ne 2 ]; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && ! grep -q "^syms:$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi
if [ "${RESUME_OK}" -eq 1 ] && grep -q "^symbol_index:$" /tmp/vresume-2.out; then
    RESUME_OK=0
fi

if [ "${RESUME_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(resume-state)"
    sed -n '1,80p' /tmp/vresume-emit.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vresume-emit.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vresume-1.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vresume-1.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vresume-focus.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vresume-focus.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vresume-2.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vresume-2.err 2>/dev/null || true
fi
rm -rf "${TMP_RESUME_DIR}"
echo

echo "=== query_project_state ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_QUERY_DIR="$(mktemp -d /tmp/viper-query-XXXXXX)"
QUERY_OK=1
cp "${SCRIPTS_DIR}/impact_context_test.vp" "${TMP_QUERY_DIR}/entry.vp"
cp "${SCRIPTS_DIR}/impact_lib.vp" "${TMP_QUERY_DIR}/impact_lib.vp"
QUERY_STATE="${TMP_QUERY_DIR}/agent.vstate"
if ! "${BIN_ABS}" --emit-project-state="${QUERY_STATE}" --focus=read_user --impact "${TMP_QUERY_DIR}/entry.vp" >/tmp/vquery-emit.out 2>/tmp/vquery-emit.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-name=read_user >/tmp/vquery-name.out 2>/tmp/vquery-name.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^PQUERYv3$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_name: read_user$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_effect: -$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_call: -$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_impact: no$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_deps: no$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 1$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && grep -q "^paths:$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|T\\|m[0-9a-f]{16}\\|\\+\\|read_user\\(user_id\\)\\|r1\\|e1\\|e1\\|c1$" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && grep -q "route(user_id)" /tmp/vquery-name.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-effect=os >/tmp/vquery-effect.out 2>/tmp/vquery-effect.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_effect: os$" /tmp/vquery-effect.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vquery-effect.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 1$" /tmp/vquery-effect.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|T\\|m[0-9a-f]{16}\\|\\+\\|read_user\\(user_id\\)\\|r1\\|e1\\|e1\\|c1$" /tmp/vquery-effect.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-call=read_user >/tmp/vquery-call.out 2>/tmp/vquery-call.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_call: read_user$" /tmp/vquery-call.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vquery-call.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 1$" /tmp/vquery-call.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^  s[0-9a-f]{16}\\|C\\|m[0-9a-f]{16}\\|\\+\\|format_user\\(user_id\\)\\|r1\\|-\\|-\\|c1$" /tmp/vquery-call.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && grep -q "read_user(user_id)|r1|e1|e1|c1" /tmp/vquery-call.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-name=read_user --impact >/tmp/vquery-impact.out 2>/tmp/vquery-impact.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_impact: yes$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_deps: no$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 4$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^paths:$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^explain:$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^risk_top:$" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^paths:/{flag=1;next}/^explain:/{flag=0} flag && /^  s[0-9a-f]{16}=/{count++} END{print count+0}' /tmp/vquery-impact.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^explain:/{flag=1;next}/^mods:/{flag=0} flag && /^  s[0-9a-f]{16}=/{count++} END{print count+0}' /tmp/vquery-impact.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^risk_top:/{flag=1;next}/^mods:/{flag=0} flag && /^  [0-9]+\|s[0-9a-f]{16}\|/{count++} END{print count+0}' /tmp/vquery-impact.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=seed$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=seed\(name=read_user\)$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=impact\(from=s[0-9a-f]{16},depth=1\)$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=impact\(from=s[0-9a-f]{16},depth=2\)$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=impact\(from=s[0-9a-f]{16},depth=3\)$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  1\|s[0-9a-f]{16}\|name=read_user\|score=39\|blast=3\|depfx=os\|pub=1$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  2\|s[0-9a-f]{16}\|name=format_user\|score=28\|blast=2\|depfx=os\|pub=1$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  3\|s[0-9a-f]{16}\|name=route\|score=16\|blast=1\|depfx=os\|pub=0$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  4\|s[0-9a-f]{16}\|name=render\|score=6\|blast=0\|depfx=os\|pub=0$' /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "route(user_id)" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "render(user_id)" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "format_user(user_id)" /tmp/vquery-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-name=read_user --impact --brief >/tmp/vquery-brief.out 2>/tmp/vquery-brief.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^brief: yes$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^mods:$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^brief_syms:$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^ledger_bytes: [1-9][0-9]*$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^ledger_full_bytes: [1-9][0-9]*$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^ledger_saved_bytes: [1-9][0-9]*$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq "^ledger_saved_pct: [1-9][0-9]*$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^risk_top:$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && grep -q "^rets:$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && grep -q "^syms:$" /tmp/vquery-brief.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(grep -Ec "^  [0-9]+\\|s[0-9a-f]{16}\\|[TC]\\|m[0-9a-f]{16}\\|[^|]+\\|[^|]+\\|[+-]\\|[^|]*\\|[0-9]+$" /tmp/vquery-brief.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-name=render --query-deps >/tmp/vquery-deps.out 2>/tmp/vquery-deps.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_impact: no$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_deps: yes$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 4$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^paths:$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^explain:$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^risk_top:$" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^paths:/{flag=1;next}/^explain:/{flag=0} flag && /^  s[0-9a-f]{16}=/{count++} END{print count+0}' /tmp/vquery-deps.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^explain:/{flag=1;next}/^mods:/{flag=0} flag && /^  s[0-9a-f]{16}=/{count++} END{print count+0}' /tmp/vquery-deps.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && [ "$(awk '/^risk_top:/{flag=1;next}/^mods:/{flag=0} flag && /^  [0-9]+\|s[0-9a-f]{16}\|/{count++} END{print count+0}' /tmp/vquery-deps.out)" -ne 4 ]; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=seed$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}->s[0-9a-f]{16}$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=seed\(name=render\)$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=deps\(from=s[0-9a-f]{16},depth=1\)$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=deps\(from=s[0-9a-f]{16},depth=2\)$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=deps\(from=s[0-9a-f]{16},depth=3\)$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  1\|s[0-9a-f]{16}\|name=read_user\|score=39\|blast=3\|depfx=os\|pub=1$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  2\|s[0-9a-f]{16}\|name=format_user\|score=28\|blast=2\|depfx=os\|pub=1$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  3\|s[0-9a-f]{16}\|name=route\|score=16\|blast=1\|depfx=os\|pub=0$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  4\|s[0-9a-f]{16}\|name=render\|score=6\|blast=0\|depfx=os\|pub=0$' /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "route(user_id)" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "format_user(user_id)" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "read_user(user_id)" /tmp/vquery-deps.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! "${BIN_ABS}" --query-project-state="${QUERY_STATE}" --query-effect=os --impact >/tmp/vquery-effect-impact.out 2>/tmp/vquery-effect-impact.err; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_effect: os$" /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^query_impact: yes$" /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^matches: 4$" /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -q "^risk_top:$" /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=seed\(effect=os\)$' /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  s[0-9a-f]{16}=impact\(from=s[0-9a-f]{16},depth=1,effect=os\)$' /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi
if [ "${QUERY_OK}" -eq 1 ] && ! grep -Eq '^  1\|s[0-9a-f]{16}\|name=read_user\|score=39\|blast=3\|depfx=os\|pub=1$' /tmp/vquery-effect-impact.out; then
    QUERY_OK=0
fi

if [ "${QUERY_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(query-state)"
    sed -n '1,80p' /tmp/vquery-emit.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-emit.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-name.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-name.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-effect.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-effect.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-call.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-call.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-impact.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-impact.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vquery-deps.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vquery-deps.err 2>/dev/null || true
fi
rm -rf "${TMP_QUERY_DIR}"
echo

echo "=== bench_project_state ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
TMP_BENCH_DIR="$(mktemp -d /tmp/viper-bench-XXXXXX)"
BENCH_OK=1
cp "${SCRIPTS_DIR}/impact_context_test.vp" "${TMP_BENCH_DIR}/entry.vp"
cp "${SCRIPTS_DIR}/impact_lib.vp" "${TMP_BENCH_DIR}/impact_lib.vp"
BENCH_STATE="${TMP_BENCH_DIR}/agent.vstate"
if ! "${BIN_ABS}" --emit-project-state="${BENCH_STATE}" --focus=read_user --impact "${TMP_BENCH_DIR}/entry.vp" >/tmp/vbench-emit.out 2>/tmp/vbench-emit.err; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! "${BIN_ABS}" --bench-project-state="${BENCH_STATE}" --focus=read_user --impact >/tmp/vbench-resume.out 2>/tmp/vbench-resume.err; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^PBENCHv1$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^mode: resume$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^scope: ledger$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^resume_focus: read_user$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^resume_impact: yes$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^selection_symbols: 4$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^selection_modules: 2$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^include_tests: yes$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ]; then
    BENCH_BRIEF_BYTES="$(awk -F': ' '/^brief_bytes:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_FULL_BYTES="$(awk -F': ' '/^full_bytes:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_SAVED_BYTES="$(awk -F': ' '/^saved_bytes:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_SAVED_PCT="$(awk -F': ' '/^saved_pct:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_BRIEF_TOKENS="$(awk -F': ' '/^brief_tokens_est:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_FULL_TOKENS="$(awk -F': ' '/^full_tokens_est:/{print $2}' /tmp/vbench-resume.out)"
    BENCH_SAVED_TOKENS="$(awk -F': ' '/^saved_tokens_est:/{print $2}' /tmp/vbench-resume.out)"
    if [ -z "${BENCH_BRIEF_BYTES}" ] || [ -z "${BENCH_FULL_BYTES}" ] || [ -z "${BENCH_SAVED_BYTES}" ] || [ -z "${BENCH_SAVED_PCT}" ] || \
       [ -z "${BENCH_BRIEF_TOKENS}" ] || [ -z "${BENCH_FULL_TOKENS}" ] || [ -z "${BENCH_SAVED_TOKENS}" ] || \
       [ "${BENCH_BRIEF_BYTES}" -ge "${BENCH_FULL_BYTES}" ] || [ "${BENCH_SAVED_BYTES}" -le 0 ] || \
       [ "${BENCH_SAVED_PCT}" -le 0 ] || [ "${BENCH_BRIEF_TOKENS}" -ge "${BENCH_FULL_TOKENS}" ] || \
       [ "${BENCH_SAVED_TOKENS}" -le 0 ]; then
        BENCH_OK=0
    fi
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -Eq "^brief_emit_us: [0-9]+$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -Eq "^full_emit_us: [0-9]+$" /tmp/vbench-resume.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! "${BIN_ABS}" --bench-project-state="${BENCH_STATE}" --query-name=read_user --impact >/tmp/vbench-query.out 2>/tmp/vbench-query.err; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^mode: query$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^query_name: read_user$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^query_impact: yes$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^query_deps: no$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^seed_matches: 1$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^selection_symbols: 4$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ] && ! grep -q "^include_tests: yes$" /tmp/vbench-query.out; then
    BENCH_OK=0
fi
if [ "${BENCH_OK}" -eq 1 ]; then
    BENCH_QUERY_BRIEF_BYTES="$(awk -F': ' '/^brief_bytes:/{print $2}' /tmp/vbench-query.out)"
    BENCH_QUERY_FULL_BYTES="$(awk -F': ' '/^full_bytes:/{print $2}' /tmp/vbench-query.out)"
    BENCH_QUERY_SAVED_TOKENS="$(awk -F': ' '/^saved_tokens_est:/{print $2}' /tmp/vbench-query.out)"
    if [ -z "${BENCH_QUERY_BRIEF_BYTES}" ] || [ -z "${BENCH_QUERY_FULL_BYTES}" ] || [ -z "${BENCH_QUERY_SAVED_TOKENS}" ] || \
       [ "${BENCH_QUERY_BRIEF_BYTES}" -ge "${BENCH_QUERY_FULL_BYTES}" ] || [ "${BENCH_QUERY_SAVED_TOKENS}" -le 0 ]; then
        BENCH_OK=0
    fi
fi

if [ "${BENCH_OK}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(bench-state)"
    sed -n '1,80p' /tmp/vbench-emit.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vbench-emit.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vbench-resume.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vbench-resume.err 2>/dev/null || true
    sed -n '1,120p' /tmp/vbench-query.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vbench-query.err 2>/dev/null || true
fi
rm -rf "${TMP_BENCH_DIR}"
echo

echo "=== semantic_diff ==="
TOTAL_COUNT=$((TOTAL_COUNT + 1))
: > "${OUT_FILE}"
: > "${ERR_FILE}"
if "${BIN_PATH}" --emit-semantic-diff="tests/fixtures/semantic_diff_before.vp" --focus=read_user --impact "tests/fixtures/semantic_diff_after.vp" >"${OUT_FILE}" 2>"${ERR_FILE}" &&
   grep -q "^SDIFFv2$" "${OUT_FILE}" &&
   grep -q "^focus: read_user$" "${OUT_FILE}" &&
   grep -q "^presence: before=yes after=yes$" "${OUT_FILE}" &&
   grep -q "^summary: before=2 after=3 added=2 removed=1 unchanged=1$" "${OUT_FILE}" &&
   grep -q "^change_plan:$" "${OUT_FILE}" &&
   grep -q "^  1|status=changed|kind=target|name=read_user|score=35|blast=2|depfx=os,panic|checks=contract,effects,callers$" "${OUT_FILE}" &&
   grep -q "^  2|status=added|kind=caller|name=route|score=12|blast=0|depfx=os,panic|checks=new_surface,effects$" "${OUT_FILE}" &&
   grep -q "^test_plan:$" "${OUT_FILE}" &&
   grep -q "^  1|test=tests/scripts/semantic_link_test.vp|hits=2|max_score=35|symbols=read_user,route|checks=contract,new_surface,effects,callers$" "${OUT_FILE}" &&
   grep -q "^removed:$" "${OUT_FILE}" &&
   grep -q "^  target tests/fixtures/semantic_diff_before.vp::pub fn read_user(user_id) -> str effects=os inferred=os calls=os_env/1$" "${OUT_FILE}" &&
   grep -q "^added:$" "${OUT_FILE}" &&
   grep -q "^  target tests/fixtures/semantic_diff_after.vp::pub fn read_user(user_id) -> str effects=os inferred=panic,os calls=panic/1,os_env/1$" "${OUT_FILE}" &&
   grep -q "^  caller tests/fixtures/semantic_diff_after.vp::fn route(user_id) -> str effects=- inferred=- calls=read_user/1$" "${OUT_FILE}"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "PASS"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL(semantic-diff)"
    sed -n '1,80p' "${OUT_FILE}"
    sed -n '1,40p' "${ERR_FILE}"
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
@effect(web)
pub fn webPing() -> int {
    ret 7
}
fn webSecret() -> int {
    ret 99
}
EOF
    "${BIN_ABS}" pkg build >/tmp/vpm-build.out 2>/tmp/vpm-build.err &&
    grep -q "artifact" /tmp/vpm-build.out &&
    grep -q "^fn webPing 0 ret=int eff=web$" .viper/packages/@web/versions/0.1.0/index.vabi &&
    "${BIN_ABS}" pkg abi check >/tmp/vpm-abi-check.out 2>/tmp/vpm-abi-check.err &&
    grep -q "abi check passed" /tmp/vpm-abi-check.out &&
    grep -q "0.1.0" .viper/packages/@web/current &&
    cp -r .viper/packages/@web/versions/0.1.0 .viper/packages/@web/versions/0.2.0 &&
    printf '# viper abi v2\nfn webPing 0 ret=int eff=web\nfn webExtra 1 ret=- eff=-\n' > .viper/packages/@web/versions/0.2.0/index.vabi &&
    "${BIN_ABS}" pkg abi diff @web 0.1.0 0.2.0 >/tmp/vpm-abi-diff.out 2>/tmp/vpm-abi-diff.err &&
    "${BIN_ABS}" pkg abi diff @web 0.1.0 0.2.0 --fail-on-breaking >/tmp/vpm-abi-diff-nb.out 2>/tmp/vpm-abi-diff-nb.err &&
    grep -q "ADDED index.vabi fn webExtra 1" /tmp/vpm-abi-diff.out &&
    grep -q "added=1" /tmp/vpm-abi-diff.out &&
    cp -r .viper/packages/@web/versions/0.1.0 .viper/packages/@web/versions/0.3.0 &&
    printf '# viper abi v2\nfn webPing 1 ret=int eff=web\n' > .viper/packages/@web/versions/0.3.0/index.vabi &&
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
    cat > app_type_bad.vp <<'EOF' &&
use "@web" as web
var pong: str = web.webPing()
EOF
    ! "${BIN_ABS}" app_type_bad.vp >/tmp/vpm-run-type.out 2>/tmp/vpm-run-type.err &&
    cat /tmp/vpm-run-type.out /tmp/vpm-run-type.err | grep -q "Type mismatch for variable 'pong'" &&
    cat > app_effect_bad.vp <<'EOF' &&
use "@web" as web
@effect(os)
fn bad() {
    ret web.webPing()
}
bad()
EOF
    ! "${BIN_ABS}" app_effect_bad.vp >/tmp/vpm-run-effect.out 2>/tmp/vpm-run-effect.err &&
    cat /tmp/vpm-run-effect.out /tmp/vpm-run-effect.err | grep -q "Effect mismatch for function 'bad'" &&
    cat /tmp/vpm-run-effect.out /tmp/vpm-run-effect.err | grep -q "web" &&
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
    sed -n '1,40p' /tmp/vpm-run-type.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-type.err 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-effect.out 2>/dev/null || true
    sed -n '1,40p' /tmp/vpm-run-effect.err 2>/dev/null || true
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
    printf '\nvar total = 0\n' >> "${STAR_DIR}/main.vp" &&
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
