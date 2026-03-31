from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    app_name: str = "Whimsical Agent Runtime"
    executor_url: str = "http://127.0.0.1:8088"
    default_local_model: str = "llama3:8b"

    # Ollama
    ollama_base_url: str = "http://127.0.0.1:11434"

    # Cloud (OpenAI-compatible)
    cloud_api_key: str = ""
    cloud_base_url: str = "https://api.openai.com/v1"

    # Postgres
    postgres_dsn: str = "postgresql+asyncpg://agent:agent@127.0.0.1:5432/agentdb"

    # Redis
    redis_url: str = "redis://127.0.0.1:6379/0"

    # Qdrant
    qdrant_url: str = "http://127.0.0.1:6333"
    qdrant_collection: str = "agent_memory"

    # Logging
    log_level: str = "INFO"


settings = Settings()
