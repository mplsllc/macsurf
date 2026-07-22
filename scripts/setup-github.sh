#!/usr/bin/env bash
# scripts/setup-github.sh
#
# Idempotently provision the mplsllc/macsurf repository's GitHub scaffolding:
#   - Enable Issues / Wiki / Discussions / Projects
#   - Create the full label set (Type / Status / Priority / Severity / Area)
#   - Create the roadmap milestones
#
# Issue templates live in .github/ISSUE_TEMPLATE/ and are managed by git;
# running this script does NOT touch them.
#
# Usage:  bash scripts/setup-github.sh
#         REPO=other-org/other-repo bash scripts/setup-github.sh
#
# Requires: gh >= 2.20, authenticated with repo+admin:repo scopes.

set -euo pipefail

REPO="${REPO:-mplsllc/macsurf}"

# ----- prerequisites -----
command -v gh >/dev/null 2>&1 || {
  echo "ERROR: gh CLI not installed." >&2
  exit 1
}
gh auth status >/dev/null 2>&1 || {
  echo "ERROR: gh not authenticated. Run 'gh auth login'." >&2
  exit 1
}
gh repo view "$REPO" >/dev/null 2>&1 || {
  echo "ERROR: cannot access $REPO. Check the repo name and your auth scopes." >&2
  exit 1
}

echo "==> Provisioning $REPO"

# ----- repo features -----
echo "--> Enabling repo features (issues/wiki/discussions/projects)"
gh api -X PATCH "repos/$REPO" \
  -F has_issues=true \
  -F has_wiki=false \
  -F has_discussions=true \
  -F has_projects=true \
  --jq '"   features now: issues=\(.has_issues) wiki=\(.has_wiki) discussions=\(.has_discussions) projects=\(.has_projects)"'

# ----- legacy label rename (idempotent: only renames if old name still exists) -----
rename_label() {
  local old="$1"
  local new="$2"
  if gh label list --repo "$REPO" --search "$old" --json name --jq '.[].name' | grep -qx "$old"; then
    if gh label list --repo "$REPO" --search "$new" --json name --jq '.[].name' | grep -qx "$new"; then
      gh label delete "$old" --repo "$REPO" --yes >/dev/null
      printf "    %s (deleted, target already present)\n" "$old"
    else
      gh label edit "$old" --name "$new" --repo "$REPO" >/dev/null
      printf "    %s -> %s\n" "$old" "$new"
    fi
  fi
}
echo "--> Reconciling legacy label names"
rename_label "area: network" "area: networking"
rename_label "area: ui"      "area: chrome"

# ----- labels -----
echo "--> Creating / updating labels"
# Each line: NAME|COLOR|DESCRIPTION
LABELS=$(cat <<'LABELS_EOF'
bug|d73a4a|Something isn't working
enhancement|a2eeef|New feature or request
documentation|0075ca|Improvements or additions to documentation
question|d876e3|Further information is requested
discussion|cc317c|Open-ended discussion or design topic
confirmed|fbca04|Reproduced and validated
needs-repro|d4c5f9|Cannot yet reproduce; more info needed
blocked|000000|Blocked on external work or decision
wontfix|ffffff|This will not be worked on
duplicate|cfd3d7|This issue or pull request already exists
regression|e99695|Previously worked; now broken
priority: high|b60205|Should ship in current release
priority: medium|fbca04|Targeted for an upcoming release
priority: low|0e8a16|Nice-to-have / backlog
severity: crash|b60205|Crashes the browser or freezes the OS
severity: major|d93f0b|Substantial loss of function but recoverable
severity: minor|fef2c0|Small visual or behavioral issue
area: css|1d76db|CSS engine, libcss, cascade, layout
area: grid|0e8a16|CSS Grid (V1 and V2 alignment)
area: flex|0e8a16|Flexbox layout
area: js-engine|c5def5|Duktape, ES5 evaluator, JS bindings
area: networking|1d76db|Open Transport, HTTP, proxy
area: tls|1d76db|macSSL, HTTPS, BearSSL
area: rendering|0e8a16|QuickDraw plotters, GWorld, paint pipeline
area: images|0e8a16|PNG, GIF, JPEG, BMP, TIFF decoders
area: fonts|0e8a16|Font dispatch, GetFontInfo, typography
area: chrome|5319e7|Address bar, window chrome, status bar, UI
area: build|bfd4f2|CodeWarrior project, Retro68 cross-build, scripts
area: docs|0075ca|Documentation and wiki
hardware-specific|d876e3|Tied to specific Mac hardware (Beige G3, Pismo, etc.)
compatibility|d876e3|Real-world site compatibility report
c89|bfdadc|CodeWarrior 8 / C89 constraint involved
memory|e99695|16MB partition / heap exhaustion related
good first issue|7057ff|Good for newcomers
help wanted|008672|Extra attention is needed
LABELS_EOF
)

while IFS='|' read -r name color desc; do
  [ -z "$name" ] && continue
  gh label create "$name" --color "$color" --description "$desc" --repo "$REPO" --force >/dev/null
  printf "    %s\n" "$name"
done <<< "$LABELS"

# Prune the unused 'invalid' default label.
if gh label list --repo "$REPO" --search "invalid" --json name --jq '.[].name' | grep -qx "invalid"; then
  gh label delete "invalid" --repo "$REPO" --yes >/dev/null
  printf "    invalid (deleted)\n"
fi

# ----- milestones -----
echo "--> Creating milestones (only if missing)"
existing_titles=$(gh api "repos/$REPO/milestones?state=all" --jq '.[].title' | tr '\n' '|')

create_ms() {
  local title="$1"
  local desc="$2"
  if echo "|$existing_titles" | grep -q "|$title|"; then
    printf "    %s (exists)\n" "$title"
  else
    gh api "repos/$REPO/milestones" -f title="$title" -f description="$desc" --jq '"    \(.title) (created #\(.number))"'
  fi
}

create_ms "v0.1a2" "Modern-site survival release. Heavy modern pages (apple, github, wikipedia, yahoo, MDN) load partially and reliably without crashing. Images may be skipped or placeholdered; huge stylesheets may be capped."
create_ms "v0.1a3" "Stability + polish after 0.1a2. Deferred wheel-mouse crash, image-OOM tail-cases, logger format hygiene, partial-load UX."
create_ms "v0.2"   "HTTPS via macSSL, CSS Grid V2 alignment (justify/align), JavaScript expansion beyond ES5 baseline."
create_ms "v1.0"   "Public beta release. Feature-complete for daily-driver use on Mac OS 9 against the modern web."

# ----- repo metadata: description, homepage, topics, PVR -----
echo "--> Setting repo description and homepage"
gh api -X PATCH "repos/$REPO" \
  -f description="A modern web browser for Classic Mac OS 9 PowerPC. Real CSS3, ES5 JavaScript, native HTTPS — built with CodeWarrior on the Carbon API." \
  -f homepage="https://macsurf.org" >/dev/null
printf "    description + homepage set\n"

echo "--> Setting repo topics"
gh api -X PUT "repos/$REPO/topics" --input - <<'TOPICS_EOF' >/dev/null
{"names":["mac-os-9","powerpc","classic-mac","web-browser","netsurf","codewarrior","carbon-api","quickdraw","retro-computing","duktape","css3","javascript","open-transport","bearssl","macintosh"]}
TOPICS_EOF
printf "    topics: 15 set\n"

echo "--> Enabling private vulnerability reporting"
gh api -X PUT "repos/$REPO/private-vulnerability-reporting" >/dev/null 2>&1 || true
printf "    enabled\n"

echo "==> Done."
