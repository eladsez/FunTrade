#!/usr/bin/env bash
# Deploy FunTrade on the Pi (run by the self-hosted runner, or by hand):
#   deploy/deploy.sh <git-sha>                    bot + dashboard
#   DASHBOARD_ONLY=1 deploy/deploy.sh <git-sha>   dashboard only (CI: nothing but dashboard/ changed)
# A bot deploy waits out US market hours (restarting mid-session cancels the day's MOC
# orders), installs the compose file into $FUNTRADE_HOME, pulls the images for <sha>,
# restarts, and rolls back to the previous tag if the bot doesn't reach "Running N baskets"
# in 5 minutes. The dashboard never touches the bot: it deploys at any time, and a dashboard
# that fails its health check is rolled back on its own.
set -euo pipefail

SHA=${1:?usage: deploy.sh <git-sha>}
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST=${FUNTRADE_HOME:-$HOME/funtrade}
IMAGE=ghcr.io/eladsez/funtrade
DASH_IMAGE=ghcr.io/eladsez/funtrade-dashboard
DASHBOARD_ONLY=${DASHBOARD_ONLY:-}

mkdir -p "$DEST"/{state,logs,gateway,dashboard}
if [[ ! -f "$DEST/.env" ]]; then
    echo "::error::$DEST/.env is missing. Copy deploy/env.example there and fill in the paper login."
    exit 1
fi
chmod 600 "$DEST/.env"
cd "$DEST"

get_var() { sed -n "s/^$1=//p" .env; }
set_var() {
    if grep -q "^$1=" .env; then sed -i "s|^$1=.*|$1=$2|" .env
    else echo "$1=$2" >> .env; fi
}

dashboard_healthy() {  # the image's HEALTHCHECK polls /api/health
    for _ in $(seq 24); do
        local id status
        id=$(docker compose ps -q dashboard)
        status=$(docker inspect --format '{{.State.Health.Status}}' "$id" 2>/dev/null || echo none)
        [[ "$status" == healthy ]] && return 0
        sleep 5
    done
    return 1
}

# Dashboard: pull, switch, verify, roll back on its own. Never restarts the bot (--no-deps).
deploy_dashboard() {
    local prev
    prev=$(get_var DASHBOARD_TAG)
    docker pull -q "$DASH_IMAGE:$SHA"
    set_var DASHBOARD_TAG "$SHA"
    docker compose up -d --no-deps dashboard
    if dashboard_healthy; then
        echo "Dashboard $SHA is up."
        return 0
    fi
    echo "::error::Dashboard $SHA is not healthy. Last log lines:"
    docker compose logs --tail 30 dashboard || true
    if [[ -n "$prev" && "$prev" != "$SHA" ]]; then
        echo "Rolling the dashboard back to $prev."
        set_var DASHBOARD_TAG "$prev"
        docker compose up -d --no-deps dashboard
    fi
    return 1
}

if [[ "$DASHBOARD_ONLY" == "1" ]]; then
    if [[ -z "$(get_var FUNTRADE_TAG)" || -z "$(get_var DASHBOARD_TAG)" ]]; then
        echo "::error::Bot or dashboard never deployed here; run a full deploy first."
        exit 1
    fi
    deploy_dashboard
    docker image prune -f >/dev/null || true
    exit 0
fi

# --- 1. Not during the session (09:00-16:15 New York, Mon-Fri) ------------------------------
if [[ "${FORCE_DEPLOY:-}" != "1" ]]; then
    while :; do
        read -r dow hm < <(TZ=America/New_York date '+%u %H%M')
        if (( dow <= 5 && 10#$hm >= 900 && 10#$hm < 1615 )); then
            echo "US market session ($hm New York). Waiting until 16:15 ET to deploy..."
            sleep 300
        else
            break
        fi
    done
fi

# --- 2. Install files, pull the bot image ----------------------------------------------------
install -m 644 "$SRC/deploy/docker-compose.yml" "$DEST/docker-compose.yml"
install -m 755 "$SRC/scripts/state_report.py" "$DEST/state_report.py"
docker pull -q "$IMAGE:$SHA"

PREV=$(get_var FUNTRADE_TAG)
# compose interpolates every service's tag; the first deploy with a dashboard has none yet.
[[ -n "$(get_var DASHBOARD_TAG)" ]] || set_var DASHBOARD_TAG "$SHA"

# --- 3. Restart and verify the bot ----------------------------------------------------------
started_ok() {  # the bot logs this line once every basket is warmed up and running
    local since=$1
    for _ in $(seq 60); do
        if docker compose logs --since "$since" funtrade 2>/dev/null | grep -q '\[LIVE\] Running [0-9]* baskets'; then
            return 0
        fi
        sleep 5
    done
    return 1
}

set_var FUNTRADE_TAG "$SHA"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d --remove-orphans ib-gateway funtrade
if started_ok "$since"; then
    echo "Deployed bot $SHA."
else
    echo "::error::FunTrade $SHA did not start within 5 minutes. Last log lines:"
    docker compose logs --tail 40 funtrade || true
    if [[ -n "$PREV" && "$PREV" != "$SHA" ]]; then
        echo "Rolling back to $PREV."
        set_var FUNTRADE_TAG "$PREV"
        docker compose up -d funtrade
    fi
    exit 1
fi

# --- 4. Dashboard ---------------------------------------------------------------------------
deploy_dashboard
docker image prune -f >/dev/null || true
