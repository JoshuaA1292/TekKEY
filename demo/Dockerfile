FROM python:3.12-slim

WORKDIR /srv

COPY gateway /srv/gateway
COPY fixtures /srv/fixtures
COPY evals/data/attacks.json /srv/evals/data/attacks.json
COPY demo /srv/demo

RUN pip install --no-cache-dir -r /srv/demo/requirements.txt

ENV PYTHONPATH=/srv \
    PYTHONUNBUFFERED=1

EXPOSE 8080

CMD ["sh", "-c", "python -m uvicorn demo.server:app --host 0.0.0.0 --port ${PORT:-8080}"]
