# Playerbots Vector Memory Service

Local/offline memory service for `mod-playerbots` route and quest adaptation.

## What It Does

- Receives route/quest episode events from worldserver.
- Embeds episode text with `BAAI/bge-small-en-v1.5`.
- Stores vectors in Qdrant.
- Serves per-bot avoid hints:
  - `avoid_grid` lines for route memory
  - `avoid_quest` lines for quest memory

Retention is enforced on each upsert:
- TTL: 30 days
- Cap: 2000 `route_episode` + 2000 `quest_episode` per bot

## Endpoints

- `GET /healthz`
- `POST /v1/upsert`
- `POST /v1/query_avoid_grids`
- `POST /v1/query_quest_avoid`

Request and response format is `text/plain` with `key: value` lines.

## Run

1. Start Qdrant:
```bash
docker run -p 6333:6333 qdrant/qdrant
```

2. Install dependencies:
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

3. Start service:
```bash
export QDRANT_URL=http://127.0.0.1:6333
export COLLECTION_NAME=playerbots_memory
export MODEL_NAME=BAAI/bge-small-en-v1.5
uvicorn main:app --host 127.0.0.1 --port 7788
```

4. Quick check:
```bash
curl -s http://127.0.0.1:7788/healthz
```
