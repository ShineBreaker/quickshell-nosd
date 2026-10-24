#!/usr/bin/env bash
# 上游同步脚本：基线文件 + 逐条 cherry-pick，保持 fork 历史线性。
# 设计依据 agenote 经验卡 20261020-195046 / 20261021-190641：
#   - 禁止 merge 上游：merge commit 会破坏后续 rev-list 基线同步；
#   - 基线记录「已处理到的上游 SHA」，跳过的提交也推进基线，避免重复评估；
#   - runner 需要 bot git 身份；gh 命令需显式 --repo。
# 安全性：启动前要求工作区完全干净（含暂存区），因此回滚用的
#         git reset --hard 只会丢弃脚本自己产生的 cherry-pick 状态。
# 用法：
#   scripts/sync-upstream.sh            # 正常同步（提交到本地 master，不推送）
#   DRY_RUN=1 scripts/sync-upstream.sh  # 演练：只输出计划，不改任何状态
set -Eeuo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

UPSTREAM_URL="https://github.com/quickshell-mirror/quickshell.git"
UPSTREAM_REMOTE="upstream"
UPSTREAM_REF="master"
FORK_BRANCH="master"
BASE_FILE=".github/upstream-sync-base"
BOT_NAME="github-actions[bot]"
BOT_EMAIL="41898282+github-actions[bot]@users.noreply.github.com"
DRY_RUN="${DRY_RUN:-0}"

log() { echo "[sync] $*"; }
fail() { echo "[sync] 错误：$*" >&2; exit 1; }

[ "$(git rev-parse --abbrev-ref HEAD)" = "$FORK_BRANCH" ] || fail "请在 $FORK_BRANCH 分支上运行"
git diff --quiet || fail "工作区有未提交改动，先处理再同步"
git diff --cached --quiet || fail "暂存区有未提交改动，先处理再同步"

START_HEAD="$(git rev-parse HEAD)"

if ! git remote | grep -qx "$UPSTREAM_REMOTE"; then
  [ "$DRY_RUN" = "0" ] || fail "DRY_RUN 模式不添加 remote；请先 git remote add $UPSTREAM_REMOTE $UPSTREAM_URL"
  log "添加 remote $UPSTREAM_REMOTE"
  git remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL"
fi
log "fetch 上游 $UPSTREAM_REMOTE/$UPSTREAM_REF"
git fetch "$UPSTREAM_REMOTE" "$UPSTREAM_REF"
UPSTREAM_HEAD="$(git rev-parse "$UPSTREAM_REMOTE/$UPSTREAM_REF")"

# 异常回滚：只丢弃本次脚本产生的 cherry-pick 状态（启动时已确认工作区干净）
cleanup() {
  git cherry-pick --abort >/dev/null 2>&1 || true
  git reset --hard "$START_HEAD" >/dev/null 2>&1 || true
  echo "[sync] 已回滚到 $START_HEAD" >&2
}
trap cleanup ERR

# ---- 确定基线（首次运行以分叉点为初始基线，结束时一次性落盘）----
if [ -f "$BASE_FILE" ]; then
  BASE="$(cat "$BASE_FILE")"
  git cat-file -e "$BASE^{commit}" 2>/dev/null || fail "基线 $BASE 不在对象库中（上游可能改写了历史）"
  git merge-base --is-ancestor "$BASE" "$UPSTREAM_HEAD" || fail "基线 $BASE 不是上游 HEAD 的祖先（上游强推/改写历史），中止"
else
  BASE="$(git merge-base HEAD "$UPSTREAM_HEAD")"
  log "基线文件不存在，以分叉点 $BASE 为初始基线"
fi

# 上游范围内不允许 merge commit（cherry-pick 无法干净处理）
[ -z "$(git rev-list --merges "$BASE..$UPSTREAM_HEAD")" ] || fail "上游范围内存在 merge commit，中止并人工处理"

mapfile -t COMMITS < <(git rev-list --reverse "$BASE..$UPSTREAM_HEAD")
if [ "${#COMMITS[@]}" -eq 0 ]; then
  log "上游无新提交（基线已最新）"
  exit 0
fi
log "待同步 ${#COMMITS[@]} 个提交"

pick() {
  local sha="$1" pid mine_pid r
  # 预检：与 fork 已有提交 patch 等价（外来补丁已被上游收录）→ 跳过
  pid="$(git show "$sha" | git patch-id --stable | awk '{print $1}')"
  for r in $(git rev-list HEAD --not "$UPSTREAM_HEAD"); do
    mine_pid="$(git show "$r" 2>/dev/null | git patch-id --stable | awk '{print $1}')"
    if [ -n "$pid" ] && [ "$pid" = "$mine_pid" ]; then
      log "跳过 $sha：与 fork 提交 $r patch 等价（补丁已被上游收录）"
      return 0
    fi
  done

  if [ "$DRY_RUN" = "1" ]; then
    log "[演练] 将应用 $sha : $(git log -1 --format=%s "$sha")"
    return 0
  fi

  git cherry-pick --no-commit "$sha"
  if git diff --cached --quiet; then
    # 空 cherry-pick：改动已全部存在（上游以不同形式重新收录）
    git cherry-pick --quit
    log "跳过 $sha：改动已存在于当前树（空 cherry-pick）"
    return 0
  fi
  git -c commit.gpgsign=false -c user.name="$BOT_NAME" -c user.email="$BOT_EMAIL" commit -q -C "$sha"
  log "已应用 $sha : $(git log -1 --format=%s "$sha")"
}

for c in "${COMMITS[@]}"; do
  pick "$c"
done
trap - ERR

# ---- 基线推进到本次上游 HEAD（跳过的提交也推进，避免重复评估）----
if [ "$DRY_RUN" = "0" ]; then
  echo "$UPSTREAM_HEAD" >"$BASE_FILE"
  git add "$BASE_FILE"
  if ! git diff --cached --quiet; then
    git -c commit.gpgsign=false -c user.name="$BOT_NAME" -c user.email="$BOT_EMAIL" commit -q -m "ci: advance upstream sync baseline to ${UPSTREAM_HEAD:0:12}"
  fi
fi
log "同步完成（DRY_RUN=$DRY_RUN）"
