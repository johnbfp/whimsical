"""
三层持久化记忆系统
─────────────────────────────────────────────────────────────────────────────
Layer 1  对话历史 (Conversation History)
         · 按 user_id 存 JSONL 文件，追加写入，重启不丢失
         · 注入 Planner/合成 prompt，实现跨任务上下文感知

Layer 2  键值记忆 (Key-Value Store)
         · JSON 文件，按 tenant_id/key 寻址
         · 支持 BM25 关键词语义搜索

Layer 3  进程内缓存 (In-process Cache)
         · 避免每次读磁盘，同 session 内高速读取

存储目录：
  sandbox/memory/
    conversations/
      {safe_user_id}.jsonl        ← 每行一个 ConversationTurn
    kv/
      {safe_tenant_id}/
        {safe_key}.json           ← 一个 MemoryEntry
"""

from __future__ import annotations

import json
import pathlib
import re
from collections import defaultdict
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from typing import Any

MEMORY_BASE = pathlib.Path(r"E:\new_work_space\whimsical_ideas\sandbox\memory")


# ── Data models ───────────────────────────────────────────────────────────────

@dataclass
class ConversationTurn:
    role: str        # "user" | "assistant"
    content: str
    timestamp: str
    task_id: str = ""


@dataclass
class MemoryEntry:
    key: str
    value: str
    timestamp: str
    tenant_id: str
    user_id: str


# ── Helpers ───────────────────────────────────────────────────────────────────

def _safe_name(s: str) -> str:
    """Sanitize a string to a safe filename component."""
    return re.sub(r"[^a-zA-Z0-9_\-]", "_", s)[:80]


def _tokenize(text: str) -> list[str]:
    """Tokenize for BM25 scoring (supports Chinese & ASCII)."""
    return re.findall(r"[\w\u4e00-\u9fff]+", text.lower())


def _bm25_score(query_tokens: list[str], doc: str, k1: float = 1.5, b: float = 0.75) -> float:
    """BM25-style relevance score (single-doc variant, no IDF)."""
    doc_tokens = _tokenize(doc)
    if not doc_tokens:
        return 0.0
    avgdl, dl = 50, len(doc_tokens)
    tf_map: dict[str, int] = defaultdict(int)
    for t in doc_tokens:
        tf_map[t] += 1
    score = 0.0
    for q in set(query_tokens):
        tf = tf_map.get(q, 0)
        if tf:
            score += (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * dl / avgdl))
    return score


# ── Service ───────────────────────────────────────────────────────────────────

class MemoryService:
    def __init__(self, base_dir: pathlib.Path = MEMORY_BASE) -> None:
        self._conv_dir = base_dir / "conversations"
        self._kv_dir = base_dir / "kv"
        self._conv_dir.mkdir(parents=True, exist_ok=True)
        self._kv_dir.mkdir(parents=True, exist_ok=True)
        # In-process cache: {user_id: [ConversationTurn, ...]}
        self._conv_cache: dict[str, list[ConversationTurn]] = {}

    # ── Conversation History ──────────────────────────────────────────────────

    def add_turn(self, user_id: str, role: str, content: str, task_id: str = "") -> None:
        """Append one turn and immediately persist to JSONL file."""
        turn = ConversationTurn(
            role=role,
            content=content,
            timestamp=datetime.now(timezone.utc).isoformat(),
            task_id=task_id,
        )
        path = self._conv_dir / f"{_safe_name(user_id)}.jsonl"
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(asdict(turn), ensure_ascii=False) + "\n")
        self._conv_cache.setdefault(user_id, []).append(turn)

    def get_history(self, user_id: str, last_n: int = 20) -> list[ConversationTurn]:
        """Return the last N turns for the user (loads from disk on first access)."""
        if user_id not in self._conv_cache:
            self._load_turns(user_id)
        return self._conv_cache.get(user_id, [])[-last_n:]

    def get_history_dicts(self, user_id: str, last_n: int = 20) -> list[dict[str, Any]]:
        return [asdict(t) for t in self.get_history(user_id, last_n)]

    def get_context_str(self, user_id: str, last_n: int = 6) -> str:
        """Format the last N turns as a prompt-injectable string."""
        turns = self.get_history(user_id, last_n)
        if not turns:
            return ""
        return "\n".join(
            f"[{'用户' if t.role == 'user' else '助手'}] {t.content[:400]}"
            for t in turns
        )

    def _load_turns(self, user_id: str) -> None:
        path = self._conv_dir / f"{_safe_name(user_id)}.jsonl"
        turns: list[ConversationTurn] = []
        if path.exists():
            for line in path.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if line:
                    try:
                        turns.append(ConversationTurn(**json.loads(line)))
                    except Exception:
                        pass
        self._conv_cache[user_id] = turns

    # ── Key-Value Memory ──────────────────────────────────────────────────────

    def kv_write(self, tenant_id: str, user_id: str, key: str, value: str) -> None:
        """Write or overwrite a named memory entry (persistent)."""
        entry = MemoryEntry(
            key=key, value=value,
            timestamp=datetime.now(timezone.utc).isoformat(),
            tenant_id=tenant_id, user_id=user_id,
        )
        kv_dir = self._kv_dir / _safe_name(tenant_id)
        kv_dir.mkdir(parents=True, exist_ok=True)
        (kv_dir / f"{_safe_name(key)}.json").write_text(
            json.dumps(asdict(entry), ensure_ascii=False, indent=2), encoding="utf-8"
        )

    def kv_read(self, tenant_id: str, key: str) -> str | None:
        path = self._kv_dir / _safe_name(tenant_id) / f"{_safe_name(key)}.json"
        if not path.exists():
            return None
        try:
            return MemoryEntry(**json.loads(path.read_text(encoding="utf-8"))).value
        except Exception:
            return None

    def kv_delete(self, tenant_id: str, key: str) -> bool:
        path = self._kv_dir / _safe_name(tenant_id) / f"{_safe_name(key)}.json"
        if path.exists():
            path.unlink()
            return True
        return False

    def kv_search(self, tenant_id: str, query: str, top_k: int = 5) -> list[dict[str, Any]]:
        """BM25 keyword search over all KV entries in the tenant."""
        kv_dir = self._kv_dir / _safe_name(tenant_id)
        if not kv_dir.exists():
            return []
        q_tokens = _tokenize(query)
        results: list[dict[str, Any]] = []
        for f in kv_dir.glob("*.json"):
            try:
                entry = MemoryEntry(**json.loads(f.read_text(encoding="utf-8")))
                score = _bm25_score(q_tokens, f"{entry.key} {entry.value}")
                if score > 0:
                    results.append({"key": entry.key, "value": entry.value, "score": round(score, 3)})
            except Exception:
                pass
        results.sort(key=lambda x: x["score"], reverse=True)
        return results[:top_k]

    # ── Backward-compat API ───────────────────────────────────────────────────

    def persist_summary(self, tenant_id: str, user_id: str, prompt: str, summary: str, task_id: str = "") -> None:
        """Save a completed task as a user+assistant conversation turn pair."""
        self.add_turn(user_id, "user", prompt, task_id)
        self.add_turn(user_id, "assistant", summary, task_id)

    def recall(self, tenant_id: str, user_id: str, top_k: int, query: str = "") -> list[str]:
        """Semantic recall: searches KV store + conversation history by BM25."""
        results: list[str] = []
        if query:
            kv_hits = [f"[记忆:{r['key']}] {r['value']}" for r in self.kv_search(tenant_id, query, top_k)]
            q_tokens = _tokenize(query)
            conv_hits = [
                f"[{'用户' if t.role == 'user' else '助手'}] {t.content}"
                for t in self.get_history(user_id, top_k * 4)
                if _bm25_score(q_tokens, t.content) > 0
            ]
            results = kv_hits + conv_hits
        else:
            results = [
                f"[{'用户' if t.role == 'user' else '助手'}] {t.content}"
                for t in self.get_history(user_id, top_k)
            ]
        seen: list[str] = []
        for item in results:
            if item not in seen:
                seen.append(item)
            if len(seen) >= top_k:
                break
        return seen
