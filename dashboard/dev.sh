#!/usr/bin/env bash
# Dashboard development, all in containers (nothing to install on the host but Docker).
#   dashboard/dev.sh up        web (:5173, hot reload) + api (:8090) on sample data
#   dashboard/dev.sh down      stop them
#   dashboard/dev.sh sample    reset the sample state in dashboard/dev/data
#   dashboard/dev.sh test      backend tests + frontend lint/typecheck/build (the image's build)
#   dashboard/dev.sh image     build the production image and run it on :8091 with the sample
#   dashboard/dev.sh lock      re-pin backend/requirements.txt (with hashes) from requirements.in
set -euo pipefail
cd "$(dirname "$0")"
GID=$(id -g)
export UID GID
compose() { docker compose -f compose.dev.yml "$@"; }

case "${1:-up}" in
    up)     mkdir -p dev/data; compose up --build ;;
    down)   compose down ;;
    sample) rm -rf dev/data; mkdir -p dev/data
            compose run --rm --no-deps api python -m app.sample /data/state /data/logs ;;
    test)   docker build -f Dockerfile --target test -t funtrade-dashboard:test .. ;;
    image)  docker build -f Dockerfile -t funtrade-dashboard:local ..
            mkdir -p dev/data
            [[ -f dev/data/state/status.json ]] || "$0" sample
            echo "http://localhost:8091"
            docker run --rm -p 127.0.0.1:8091:8080 -u "$UID:$GID" -v "$PWD/dev/data:/data" \
                -e BOT_WS_URL= -e BROKER_ENABLED=0 funtrade-dashboard:local ;;
    lock)   docker run --rm -u "$UID:$GID" -e HOME=/tmp -v "$PWD/backend:/w" -w /w python:3.12-slim sh -c \
                "pip install -q --user pip-tools && python -m piptools compile -q --generate-hashes --allow-unsafe --strip-extras -o requirements.txt requirements.in" ;;
    *)      sed -n '2,9p' "$0"; exit 1 ;;
esac
