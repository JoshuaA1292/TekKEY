# The delegate sandbox image (BUILD_B_GATEWAY.md B8).
#
# Contents: the delegate and its client library (agent/), the LLM router (chalk/), the MPL
# reference and the gateway's OpenAPI spec (agent/image/). No gateway code, no venue address,
# no keys: AUTHOR_KEY, GATEWAY_URL and CHALK_AI_GATEWAY_URL arrive as environment variables
# when the sandbox is created (agent/sandbox.py), never at build time.
#
# Build from the repo root:   docker build -f agent/image/Dockerfile -t delegate .
# agent/sandbox.py builds the same image declaratively for Daytona (Image.base ... add_local_dir);
# keep the two in sync.
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPATH=/app

RUN pip install --no-cache-dir "httpx>=0.27" "anthropic>=0.40"

WORKDIR /app
COPY agent/ /app/agent/
COPY chalk/ /app/chalk/

RUN useradd --create-home --uid 10001 delegate && chown -R delegate:delegate /app
USER delegate

ENTRYPOINT ["python", "-m", "agent.delegate"]
