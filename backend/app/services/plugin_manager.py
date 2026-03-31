from __future__ import annotations

import logging
from typing import Any

import jsonschema  # type: ignore[import-untyped]

from app.models.schemas import PluginManifest

logger = logging.getLogger(__name__)


class PluginValidationError(Exception):
    pass


class PluginManager:
    def __init__(self) -> None:
        self._manifests: dict[str, PluginManifest] = {}
        self._enabled: set[str] = set()

    def install(self, manifest: PluginManifest) -> None:
        self._manifests[manifest.plugin_id] = manifest
        logger.info("Plugin installed: %s v%s (tool=%s)", manifest.plugin_id, manifest.version, manifest.tool_name)

    def enable(self, plugin_id: str) -> bool:
        if plugin_id not in self._manifests:
            return False
        self._enabled.add(plugin_id)
        logger.info("Plugin enabled: %s", plugin_id)
        return True

    def disable(self, plugin_id: str) -> bool:
        if plugin_id not in self._manifests:
            return False
        self._enabled.discard(plugin_id)
        logger.info("Plugin disabled: %s", plugin_id)
        return True

    def uninstall(self, plugin_id: str) -> bool:
        if plugin_id not in self._manifests:
            return False
        self._enabled.discard(plugin_id)
        del self._manifests[plugin_id]
        logger.info("Plugin uninstalled: %s", plugin_id)
        return True

    def get_manifest_by_tool(self, tool_name: str) -> PluginManifest | None:
        for manifest in self._manifests.values():
            if manifest.tool_name == tool_name and manifest.plugin_id in self._enabled:
                return manifest
        return None

    def validate_input(self, tool_name: str, payload: dict[str, Any]) -> None:
        """Validate tool input against the plugin's input_schema. Raises PluginValidationError."""
        manifest = self.get_manifest_by_tool(tool_name)
        if not manifest or not manifest.input_schema:
            return
        try:
            jsonschema.validate(instance=payload, schema=manifest.input_schema)
        except jsonschema.ValidationError as exc:
            raise PluginValidationError(f"Schema validation failed for tool '{tool_name}': {exc.message}") from exc

    def list_plugins(self) -> list[dict[str, Any]]:
        """Return a summary of all installed plugins."""
        return [
            {
                "plugin_id": m.plugin_id,
                "version": m.version,
                "tool_name": m.tool_name,
                "enabled": m.plugin_id in self._enabled,
            }
            for m in self._manifests.values()
        ]
