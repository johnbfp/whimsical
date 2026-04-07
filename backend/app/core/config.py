from pydantic import AliasChoices, Field
from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    app_name: str = "Whimsical Agent Runtime"
    executor_url: str = "http://127.0.0.1:8088"
    default_local_model: str = "llama3:8b"

    # Ollama
    ollama_base_url: str = "http://127.0.0.1:11434"

    # Cloud (OpenAI-compatible)
    cloud_api_key: str = Field(
        default="sk-sp-95a5b61ee8274353af86620c12c9eac0",
        validation_alias=AliasChoices("CLOUD_API_KEY", "DASHSCOPE_API_KEY", "OPENAI_API_KEY"),
    )
    cloud_base_url: str = Field(
        default="https://coding.dashscope.aliyuncs.com/v1",
        validation_alias=AliasChoices("CLOUD_BASE_URL", "DASHSCOPE_BASE_URL", "OPENAI_BASE_URL"),
    )

    # Postgres
    postgres_dsn: str = "postgresql+asyncpg://agent:agent@127.0.0.1:5432/agentdb"

    # Redis
    redis_url: str = "redis://127.0.0.1:6379/0"

    # Qdrant
    qdrant_url: str = "http://127.0.0.1:6333"
    qdrant_collection: str = "agent_memory"

    # Logging
    log_level: str = "INFO"

    # Workspace (sandbox directory for coding)
    workspace_dir: str = r"E:\new_work_space\whimsical_ideas\sandbox"


settings = Settings()
