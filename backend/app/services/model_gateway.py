from __future__ import annotations

import logging
from abc import ABC, abstractmethod

import httpx

logger = logging.getLogger(__name__)


class ModelProvider(ABC):
    @abstractmethod
    async def chat(self, prompt: str) -> str:
        raise NotImplementedError

    @abstractmethod
    async def embed(self, text: str) -> list[float]:
        raise NotImplementedError

    @abstractmethod
    async def rerank(self, query: str, docs: list[str]) -> list[str]:
        raise NotImplementedError

    @abstractmethod
    async def health_check(self) -> bool:
        raise NotImplementedError


class OllamaProvider(ModelProvider):
    """Connects to a real Ollama instance."""

    def __init__(self, model_name: str, base_url: str = "http://127.0.0.1:11434") -> None:
        self.model_name = model_name
        self.base_url = base_url.rstrip("/")

    async def chat(self, prompt: str) -> str:
        payload = {"model": self.model_name, "prompt": prompt, "stream": False}
        async with httpx.AsyncClient(timeout=120) as client:
            resp = await client.post(f"{self.base_url}/api/generate", json=payload)
            resp.raise_for_status()
            return resp.json().get("response", "")

    async def embed(self, text: str) -> list[float]:
        payload = {"model": self.model_name, "input": text}
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(f"{self.base_url}/api/embed", json=payload)
            resp.raise_for_status()
            data = resp.json()
            embeddings = data.get("embeddings", [[]])
            return embeddings[0] if embeddings else []

    async def rerank(self, query: str, docs: list[str]) -> list[str]:
        # Ollama has no native rerank; fallback to length-based heuristic
        return sorted(docs, key=lambda d: abs(len(d) - len(query)))

    async def health_check(self) -> bool:
        try:
            async with httpx.AsyncClient(timeout=5) as client:
                resp = await client.get(f"{self.base_url}/api/tags")
                return resp.status_code == 200
        except Exception:
            return False


class CloudProvider(ModelProvider):
    """OpenAI-compatible cloud provider (OpenAI / Anthropic / DeepSeek etc.)."""

    def __init__(self, model_name: str, api_key: str, base_url: str = "https://api.openai.com/v1") -> None:
        self.model_name = model_name
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")

    def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"}

    async def chat(self, prompt: str) -> str:
        payload = {
            "model": self.model_name,
            "messages": [{"role": "user", "content": prompt}],
        }
        async with httpx.AsyncClient(timeout=120) as client:
            resp = await client.post(
                f"{self.base_url}/chat/completions", json=payload, headers=self._headers()
            )
            resp.raise_for_status()
            return resp.json()["choices"][0]["message"]["content"]

    async def embed(self, text: str) -> list[float]:
        payload = {"model": self.model_name, "input": text}
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(
                f"{self.base_url}/embeddings", json=payload, headers=self._headers()
            )
            resp.raise_for_status()
            return resp.json()["data"][0]["embedding"]

    async def rerank(self, query: str, docs: list[str]) -> list[str]:
        return sorted(docs, key=lambda d: abs(len(d) - len(query)))

    async def health_check(self) -> bool:
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                resp = await client.get(f"{self.base_url}/models", headers=self._headers())
                return resp.status_code == 200
        except Exception:
            return False


class LocalMockProvider(ModelProvider):
    """Fallback mock when no real model is available."""

    def __init__(self, model_name: str) -> None:
        self.model_name = model_name

    async def chat(self, prompt: str) -> str:
        return f"[local:{self.model_name}] {prompt}"

    async def embed(self, text: str) -> list[float]:
        return [float(len(text) % 11), 0.5, 1.0]

    async def rerank(self, query: str, docs: list[str]) -> list[str]:
        return sorted(docs, key=lambda d: abs(len(d) - len(query)))

    async def health_check(self) -> bool:
        return True


class ModelGateway:
    def __init__(
        self,
        default_local_model: str,
        ollama_base_url: str = "http://127.0.0.1:11434",
        cloud_api_key: str = "",
        cloud_base_url: str = "https://api.openai.com/v1",
    ) -> None:
        self.provider_name = "local"
        self.model_name = default_local_model
        self.provider: ModelProvider = LocalMockProvider(default_local_model)

        self._ollama_base_url = ollama_base_url
        self._cloud_api_key = cloud_api_key
        self._cloud_base_url = cloud_base_url

    def switch(self, provider: str, model_name: str) -> None:
        # Strip provider prefix if present (e.g. "ollama:llama3:8b" -> "llama3:8b")
        clean_name = model_name.split(":", 1)[1] if ":" in model_name and model_name.split(":")[0] in ("ollama", "cloud", "local") else model_name
        if provider == "local":
            self.provider = LocalMockProvider(clean_name)
        elif provider == "ollama":
            self.provider = OllamaProvider(clean_name, self._ollama_base_url)
        elif provider == "cloud":
            if not self._cloud_api_key:
                raise ValueError("cloud_api_key is required for cloud provider")
            self.provider = CloudProvider(clean_name, self._cloud_api_key, self._cloud_base_url)
        else:
            raise ValueError(f"Unsupported provider: {provider}")
        self.provider_name = provider
        self.model_name = clean_name
        logger.info("Model switched to provider=%s model=%s", provider, model_name)

    async def try_ollama_then_fallback(self) -> None:
        """Attempt to connect to Ollama, fall back to local mock if unavailable."""
        ollama = OllamaProvider(self.model_name, self._ollama_base_url)
        if await ollama.health_check():
            self.provider = ollama
            self.provider_name = "ollama"
            logger.info("Connected to Ollama (%s)", self._ollama_base_url)
        else:
            logger.warning("Ollama unavailable, using local mock provider")
