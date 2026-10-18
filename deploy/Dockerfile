# The whole stack in one image: Build A's venue (C++), Build B's gateway + lab (Python), the page.
#   docker build -f deploy/Dockerfile -t bounded-key .
#   docker run -p 8080:8080 --env-file .env bounded-key
FROM ubuntu:24.04 AS venue
RUN apt-get update && apt-get install -y --no-install-recommends clang make libstdc++-13-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src/minsky
COPY stage1-orderbook-computer/src/ /src/stage1-orderbook-computer/src/
COPY minsky/ /src/minsky/
COPY fixtures/ /src/fixtures/
RUN rm -rf build && make -j"$(nproc)" CXX=clang++ ENGINE_SHA="$(cat src/fastbook.cpp src/fastbook.hpp | sha256sum | cut -c1-12)" \
      build/policyc build/verify build/replay build/venued build/exchange 2>&1 | tail -20 && ls build

FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 curl && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY gateway/requirements.txt /app/gateway/requirements.txt
RUN pip install --no-cache-dir -r /app/gateway/requirements.txt
COPY --from=venue /src/minsky/build /app/minsky/build
COPY fixtures/ /app/fixtures/
COPY gateway/ /app/gateway/
COPY agent/ /app/agent/
COPY chalk/ /app/chalk/
COPY evals/ /app/evals/
COPY lab/ /app/lab/
COPY race/ /app/race/
COPY site/ /app/site/
COPY scripts/serve.sh /app/scripts/serve.sh
ENV VENUE=real PORT=8080 HOST=0.0.0.0 GATEWAY_DB=/data/gateway.db PYTHONUNBUFFERED=1
VOLUME ["/data"]
EXPOSE 8080
CMD ["sh", "/app/scripts/serve.sh"]
