#!/usr/bin/env bash
# Deploy FunTrade on the Pi (run by the self-hosted runner, or by hand):
#   deploy/deploy.sh <git-sha>
# Waits out US market hours (restarting mid-session cancels the day's MOC orders), installs
# the compose file into $FUNTRADE_HOME, pulls ghcr.io/eladsez/funtrade:<sha>, restarts, and
# rolls back to the previous tag if the bot doesn't reach "Running N baskets" in 5 minutes.
set -euo pipefail

SHA=${1:?usage: deploy.sh <git-sha>}
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST=${FUNTRADE_HOME:-$HOME/funtrade}
IMAGE=ghcr.io/eladsez/funtrade

mkdir -p "$DEST"/{state,logs,gateway}
if [[ ! -f "$DEST/.env" ]]; then
    echo "::error::$DEST/.env is missing. Copy deploy/env.example there and fill in the paper login."
    exit 1
fi
chmod 600 "$DEST/.env"

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

# --- 2. Install files, pull the image --------------------------------------------------------
install -m 644 "$SRC/deploy/docker-compose.yml" "$DEST/docker-compose.yml"
install -m 755 "$SRC/scripts/state_report.py" "$DEST/state_report.py"
cd "$DEST"
docker pull -q "$IMAGE:$SHA"

set_tag() {
    if grep -q '^FUNTRADE_TAG=' .env; then sed -i "s|^FUNTRADE_TAG=.*|FUNTRADE_TAG=$1|" .env
    else echo "FUNTRADE_TAG=$1" >> .env; fi
}
PREV=$(sed -n 's/^FUNTRADE_TAG=//p' .env)

# --- 3. Restart and verify ------------------------------------------------------------------
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

set_tag "$SHA"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d --remove-orphans
if started_ok "$since"; then
    echo "Deployed $SHA."
    docker image prune -f >/dev/null || true
    exit 0
fi

echo "::error::FunTrade $SHA did not start within 5 minutes. Last log lines:"
docker compose logs --tail 40 funtrade || true
if [[ -n "$PREV" && "$PREV" != "$SHA" ]]; then
    echo "Rolling back to $PREV."
    set_tag "$PREV"
    docker compose up -d
fi
exit 1
