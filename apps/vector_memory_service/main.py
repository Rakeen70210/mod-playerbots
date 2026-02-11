import os
import time
import uuid
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Tuple

from fastapi import FastAPI, Request, Response
from qdrant_client import QdrantClient
from qdrant_client.http import models
from sentence_transformers import SentenceTransformer


DEFAULT_COLLECTION = "playerbots_memory"
DEFAULT_MODEL = "BAAI/bge-small-en-v1.5"
DEFAULT_QDRANT_URL = "http://127.0.0.1:6333"
TTL_DAYS = 30
TTL_MS = TTL_DAYS * 24 * 60 * 60 * 1000
CAP_BY_KIND = {"route_episode": 2000, "quest_episode": 2000}


def parse_text_body(body: str) -> Dict[str, str]:
    data: Dict[str, str] = {}
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        data[key.strip()] = value.strip()
    return data


def to_int(data: Dict[str, str], key: str, required: bool = True, default: int = 0) -> int:
    if key not in data:
        if required:
            raise ValueError(f"missing field '{key}'")
        return default
    return int(data[key])


def to_str(data: Dict[str, str], key: str, required: bool = True, default: str = "") -> str:
    if key not in data:
        if required:
            raise ValueError(f"missing field '{key}'")
        return default
    return data[key]


def ok_response(lines: List[str] = None) -> Response:
    body_lines = ["ok: 1"]
    if lines:
        body_lines.extend(lines)
    return Response("\n".join(body_lines) + "\n", media_type="text/plain")


def error_response(message: str, status: int = 400) -> Response:
    return Response(f"ok: 0\nerror: {message}\n", status_code=status, media_type="text/plain")


@dataclass
class MemoryService:
    client: QdrantClient
    model: SentenceTransformer
    collection_name: str

    @classmethod
    def create(cls) -> "MemoryService":
        qdrant_url = os.getenv("QDRANT_URL", DEFAULT_QDRANT_URL)
        collection_name = os.getenv("COLLECTION_NAME", DEFAULT_COLLECTION)
        model_name = os.getenv("MODEL_NAME", DEFAULT_MODEL)

        client = QdrantClient(url=qdrant_url)
        model = SentenceTransformer(model_name)

        service = cls(client=client, model=model, collection_name=collection_name)
        service.ensure_collection()
        return service

    def ensure_collection(self) -> None:
        if not self.client.collection_exists(self.collection_name):
            self.client.create_collection(
                collection_name=self.collection_name,
                vectors_config=models.VectorParams(size=384, distance=models.Distance.COSINE),
            )

        for field_name, schema in [
            ("bot_guid_low", models.PayloadSchemaType.INTEGER),
            ("kind", models.PayloadSchemaType.KEYWORD),
            ("ts_ms", models.PayloadSchemaType.INTEGER),
            ("map_id", models.PayloadSchemaType.INTEGER),
            ("zone_id", models.PayloadSchemaType.INTEGER),
            ("quest_id", models.PayloadSchemaType.INTEGER),
        ]:
            try:
                self.client.create_payload_index(
                    collection_name=self.collection_name,
                    field_name=field_name,
                    field_schema=schema,
                )
            except Exception:
                # Index may already exist; safe to continue.
                pass

    def embed_passage(self, text: str) -> List[float]:
        return self.model.encode(f"passage: {text}", normalize_embeddings=True).tolist()

    def embed_query(self, text: str) -> List[float]:
        return self.model.encode(f"query: {text}", normalize_embeddings=True).tolist()

    def _base_filter(self, bot_guid_low: int, kind: str) -> models.Filter:
        return models.Filter(
            must=[
                models.FieldCondition(key="bot_guid_low", match=models.MatchValue(value=bot_guid_low)),
                models.FieldCondition(key="kind", match=models.MatchValue(value=kind)),
            ]
        )

    def _ttl_filter(self, bot_guid_low: int, kind: str, cutoff_ms: int) -> models.Filter:
        return models.Filter(
            must=[
                models.FieldCondition(key="bot_guid_low", match=models.MatchValue(value=bot_guid_low)),
                models.FieldCondition(key="kind", match=models.MatchValue(value=kind)),
                models.FieldCondition(key="ts_ms", range=models.Range(lt=cutoff_ms)),
            ]
        )

    def enforce_retention(self, bot_guid_low: int, kind: str, now_ms: int) -> None:
        cutoff_ms = now_ms - TTL_MS
        ttl_filter = self._ttl_filter(bot_guid_low, kind, cutoff_ms)
        self.client.delete(
            collection_name=self.collection_name,
            points_selector=models.FilterSelector(filter=ttl_filter),
            wait=False,
        )

        cap = CAP_BY_KIND.get(kind)
        if not cap:
            return

        base_filter = self._base_filter(bot_guid_low, kind)
        count = self.client.count(
            collection_name=self.collection_name,
            count_filter=base_filter,
            exact=True,
        ).count

        if count <= cap:
            return

        all_points = []
        offset = None
        while True:
            points, offset = self.client.scroll(
                collection_name=self.collection_name,
                scroll_filter=base_filter,
                with_payload=["ts_ms"],
                with_vectors=False,
                limit=1000,
                offset=offset,
            )
            all_points.extend(points)
            if offset is None:
                break

        all_points.sort(key=lambda point: int((point.payload or {}).get("ts_ms", 0)))
        to_delete_count = max(0, len(all_points) - cap)
        if to_delete_count == 0:
            return

        point_ids = [point.id for point in all_points[:to_delete_count] if point.id is not None]
        if point_ids:
            self.client.delete(
                collection_name=self.collection_name,
                points_selector=models.PointIdsList(points=point_ids),
                wait=False,
            )


service = MemoryService.create()
app = FastAPI()


@app.get("/healthz")
def healthz() -> Response:
    return ok_response()


@app.post("/v1/upsert")
async def upsert(request: Request) -> Response:
    try:
        data = parse_text_body((await request.body()).decode("utf-8"))
        kind = to_str(data, "kind")
        if kind not in ("route_episode", "quest_episode"):
            return error_response("kind must be route_episode or quest_episode")

        bot_guid_low = to_int(data, "bot_guid_low")
        ts_ms = to_int(data, "ts_ms")
        text = to_str(data, "text")

        payload = {
            "bot_guid_low": bot_guid_low,
            "kind": kind,
            "ts_ms": ts_ms,
            "text": text,
        }

        if kind == "route_episode":
            payload.update(
                {
                    "map_id": to_int(data, "map_id"),
                    "zone_id": to_int(data, "zone_id"),
                    "start_grid_x": to_int(data, "start_grid_x"),
                    "start_grid_y": to_int(data, "start_grid_y"),
                    "end_grid_x": to_int(data, "end_grid_x"),
                    "end_grid_y": to_int(data, "end_grid_y"),
                    "teleport_fallback_used": to_int(data, "teleport_fallback_used"),
                    "stuck_attempts": to_int(data, "stuck_attempts"),
                }
            )
        else:
            payload.update(
                {
                    "quest_id": to_int(data, "quest_id"),
                    "zone_id": to_int(data, "zone_id"),
                    "level": to_int(data, "level"),
                    "event": to_str(data, "event"),
                    "drop_reason": to_str(data, "drop_reason", required=False, default=""),
                }
            )

        vector = service.embed_passage(text)
        service.client.upsert(
            collection_name=service.collection_name,
            points=[
                models.PointStruct(
                    id=str(uuid.uuid4()),
                    vector=vector,
                    payload=payload,
                )
            ],
            wait=False,
        )
        service.enforce_retention(bot_guid_low=bot_guid_low, kind=kind, now_ms=ts_ms)
        return ok_response()
    except Exception as ex:
        return error_response(str(ex), status=500)


@app.post("/v1/query_avoid_grids")
async def query_avoid_grids(request: Request) -> Response:
    try:
        data = parse_text_body((await request.body()).decode("utf-8"))
        bot_guid_low = to_int(data, "bot_guid_low")
        map_id = to_int(data, "map_id")
        zone_id = to_int(data, "zone_id")
        text = to_str(data, "text")
        now_ms = to_int(data, "ts_ms", required=False, default=0) or int(time.time() * 1000)

        query_filter = models.Filter(
            must=[
                models.FieldCondition(key="bot_guid_low", match=models.MatchValue(value=bot_guid_low)),
                models.FieldCondition(key="kind", match=models.MatchValue(value="route_episode")),
                models.FieldCondition(key="map_id", match=models.MatchValue(value=map_id)),
                models.FieldCondition(key="zone_id", match=models.MatchValue(value=zone_id)),
                models.FieldCondition(key="ts_ms", range=models.Range(gte=now_ms - TTL_MS)),
            ]
        )

        hits = service.client.search(
            collection_name=service.collection_name,
            query_vector=service.embed_query(text),
            query_filter=query_filter,
            limit=50,
            with_payload=True,
            with_vectors=False,
        )

        counts: Dict[Tuple[int, int], int] = defaultdict(int)
        for hit in hits:
            payload = hit.payload or {}
            if int(payload.get("teleport_fallback_used", 0)) != 1:
                continue
            grid_x = int(payload.get("start_grid_x", -1))
            grid_y = int(payload.get("start_grid_y", -1))
            if grid_x < 0 or grid_y < 0:
                continue
            counts[(grid_x, grid_y)] += 1

        ranked = sorted(counts.items(), key=lambda item: item[1], reverse=True)[:12]
        lines = [f"avoid_grid: {grid_x} {grid_y} {min(10, count)}" for (grid_x, grid_y), count in ranked]
        return ok_response(lines)
    except Exception as ex:
        return error_response(str(ex), status=500)


@app.post("/v1/query_quest_avoid")
async def query_quest_avoid(request: Request) -> Response:
    try:
        data = parse_text_body((await request.body()).decode("utf-8"))
        bot_guid_low = to_int(data, "bot_guid_low")
        zone_id = to_int(data, "zone_id")
        text = to_str(data, "text")
        now_ms = to_int(data, "ts_ms", required=False, default=0) or int(time.time() * 1000)

        query_filter = models.Filter(
            must=[
                models.FieldCondition(key="bot_guid_low", match=models.MatchValue(value=bot_guid_low)),
                models.FieldCondition(key="kind", match=models.MatchValue(value="quest_episode")),
                models.FieldCondition(key="zone_id", match=models.MatchValue(value=zone_id)),
                models.FieldCondition(key="ts_ms", range=models.Range(gte=now_ms - TTL_MS)),
            ]
        )

        hits = service.client.search(
            collection_name=service.collection_name,
            query_vector=service.embed_query(text),
            query_filter=query_filter,
            limit=200,
            with_payload=True,
            with_vectors=False,
        )

        dropped_by_quest: Dict[int, int] = defaultdict(int)
        turned_in_by_quest: Dict[int, int] = defaultdict(int)

        for hit in hits:
            payload = hit.payload or {}
            quest_id = int(payload.get("quest_id", 0))
            event = str(payload.get("event", ""))
            if quest_id <= 0:
                continue
            if event == "dropped":
                dropped_by_quest[quest_id] += 1
            elif event == "turned_in":
                turned_in_by_quest[quest_id] += 1

        avoid: List[Tuple[int, int]] = []
        for quest_id, dropped_count in dropped_by_quest.items():
            if dropped_count >= 2 and turned_in_by_quest.get(quest_id, 0) == 0:
                avoid.append((quest_id, dropped_count))

        avoid.sort(key=lambda entry: entry[1], reverse=True)
        lines = [f"avoid_quest: {quest_id} {drop_count}" for quest_id, drop_count in avoid[:50]]
        return ok_response(lines)
    except Exception as ex:
        return error_response(str(ex), status=500)
