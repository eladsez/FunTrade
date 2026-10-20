# FunTrade dashboard image: the React app built and linted, the FastAPI backend tested, one
# process serving both on :8080. Build from the repo root:
#   docker build -f dashboard/Dockerfile .
# Native arm64 on CI for the Pi, like the bot image.
FROM node:22-slim AS web
WORKDIR /web
COPY dashboard/frontend/package.json dashboard/frontend/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY dashboard/frontend/ ./
RUN npm run lint && npm run build

FROM python:3.12-slim AS app
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY dashboard/backend/requirements.txt ./
RUN pip install --no-cache-dir --require-hashes -r requirements.txt
COPY config /app/config
COPY dashboard/backend/ /app/
COPY --from=web /web/dist /app/static

# Tests run at build time; the final stage copies their marker, so an image only exists if
# they passed (same rule as the bot image).
FROM app AS test
RUN FUNTRADE_CONFIG=/app/config python -m pytest -q -p no:cacheprovider tests && touch /tests-passed

FROM app
COPY --from=test /tests-passed /tmp/tests-passed
# uid 1000 = the Pi user owning state/ (the bot image runs as the same uid).
RUN mkdir -p /data/dashboard && chown 1000:1000 /data/dashboard
USER 1000:1000
ARG GIT_SHA=unknown
ENV DASHBOARD_GIT_SHA=$GIT_SHA
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/api/health', timeout=4)"
CMD ["uvicorn", "app.main:create_app", "--factory", "--host", "0.0.0.0", "--port", "8080", "--no-access-log"]
