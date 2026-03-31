from __future__ import annotations

from collections import defaultdict


class MemoryService:
    def __init__(self) -> None:
        self.session_memory: dict[str, list[str]] = defaultdict(list)
        self.user_memory: dict[str, list[str]] = defaultdict(list)
        self.org_memory: dict[str, list[str]] = defaultdict(list)

    def persist_summary(self, tenant_id: str, user_id: str, summary: str) -> None:
        self.session_memory[user_id].append(summary)
        self.user_memory[user_id].append(summary)
        self.org_memory[tenant_id].append(summary)

    def recall(self, tenant_id: str, user_id: str, top_k: int) -> list[str]:
        ordered = (
            list(reversed(self.session_memory[user_id]))
            + list(reversed(self.user_memory[user_id]))
            + list(reversed(self.org_memory[tenant_id]))
        )
        dedup: list[str] = []
        for item in ordered:
            if item not in dedup:
                dedup.append(item)
            if len(dedup) >= top_k:
                break
        return dedup
