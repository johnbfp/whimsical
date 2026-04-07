package com.whimsical.agent.data

import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class ApiClientTest {

    private lateinit var server: MockWebServer
    private lateinit var api: ApiClient

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
        api = ApiClient(server.url("/").toString().trimEnd('/'))
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    // ── Task ────────────────────────────────────────────────────────

    @Test
    fun `createTask sends correct body and parses response`() {
        server.enqueue(MockResponse().setBody("""
            {"task_id":"t-1","tenant_id":"default","user_id":"u1",
             "prompt":"hi","state":"CREATED","created_at":"","updated_at":""}
        """.trimIndent()))

        val task = api.createTask("default", "u1", "hi")
        assertEquals("t-1", task.taskId)
        assertEquals(TaskState.CREATED, task.state)

        val req = server.takeRequest()
        assertEquals("/agents/tasks", req.path)
        assertEquals("POST", req.method)
        assertTrue(req.body.readUtf8().contains("\"user_id\":\"u1\""))
    }

    @Test
    fun `getTask returns parsed task`() {
        server.enqueue(MockResponse().setBody("""
            {"task_id":"t-2","tenant_id":"default","user_id":"u1",
             "prompt":"go","state":"COMPLETED","result":"done",
             "created_at":"","updated_at":""}
        """.trimIndent()))

        val task = api.getTask("t-2")
        assertEquals(TaskState.COMPLETED, task.state)
        assertEquals("done", task.result)
        assertEquals("/agents/tasks/t-2", server.takeRequest().path)
    }

    @Test
    fun `cancelTask returns true on success`() {
        server.enqueue(MockResponse().setBody("""{"task_id":"t-3","cancelled":true}"""))
        assertTrue(api.cancelTask("t-3"))
    }

    @Test(expected = ApiException::class)
    fun `getTask throws ApiException on 404`() {
        server.enqueue(MockResponse().setResponseCode(404).setBody("""{"detail":"not found"}"""))
        api.getTask("nonexistent")
    }

    // ── Memory ──────────────────────────────────────────────────────

    @Test
    fun `recallMemory parses items`() {
        server.enqueue(MockResponse().setBody("""{"items":["mem1","mem2"]}"""))
        val items = api.recallMemory(MemoryRecallRequest(userId = "u1", query = "test"))
        assertEquals(listOf("mem1", "mem2"), items)
    }

    @Test
    fun `writeMemory sends request`() {
        server.enqueue(MockResponse().setBody("""{"ok":true,"key":"k1"}"""))
        api.writeMemory(MemoryWriteRequest(userId = "u1", key = "k1", value = "v1"))
        val req = server.takeRequest()
        assertEquals("POST", req.method)
        assertTrue(req.body.readUtf8().contains("\"key\":\"k1\""))
    }

    @Test
    fun `searchMemory parses results`() {
        server.enqueue(MockResponse().setBody("""
            {"query":"q","results":[{"key":"k1","value":"v1"},{"key":"k2","value":"v2"}]}
        """.trimIndent()))
        val results = api.searchMemory("default", "q", 5)
        assertEquals(2, results.size)
        assertEquals("k1", results[0].key)
        assertEquals("v2", results[1].value)
    }

    @Test
    fun `readMemory returns value`() {
        server.enqueue(MockResponse().setBody("""{"key":"k1","value":"hello"}"""))
        assertEquals("hello", api.readMemory("default", "k1"))
    }

    @Test
    fun `getHistory returns list of maps`() {
        server.enqueue(MockResponse().setBody("""[{"role":"user","text":"hi"},{"role":"agent","text":"hello"}]"""))
        val history = api.getHistory("u1", 20)
        assertEquals(2, history.size)
        assertEquals("user", history[0]["role"])
    }

    // ── Plugin ──────────────────────────────────────────────────────

    @Test
    fun `listPlugins parses array`() {
        server.enqueue(MockResponse().setBody("""[
            {"plugin_id":"p1","version":"1.0.0","tool_name":"echo","permissions":["tool.execute"],"enabled":true,"timeout_ms":1000},
            {"plugin_id":"p2","version":"1.0.0","tool_name":"file","permissions":["tool.execute"],"enabled":false,"timeout_ms":2000}
        ]"""))
        val plugins = api.listPlugins()
        assertEquals(2, plugins.size)
        assertTrue(plugins[0].enabled)
        assertFalse(plugins[1].enabled)
    }

    @Test
    fun `enablePlugin returns true`() {
        server.enqueue(MockResponse().setBody("""{"plugin_id":"p1","enabled":true}"""))
        assertTrue(api.enablePlugin("p1"))
    }

    @Test
    fun `disablePlugin returns true`() {
        server.enqueue(MockResponse().setBody("""{"plugin_id":"p1","enabled":false}"""))
        assertTrue(api.disablePlugin("p1"))
    }

    // ── Model ───────────────────────────────────────────────────────

    @Test
    fun `switchModel returns ModelInfo`() {
        server.enqueue(MockResponse().setBody("""{"provider":"ollama","model_name":"llama3"}"""))
        val info = api.switchModel("ollama", "llama3")
        assertEquals("ollama", info.provider)
        assertEquals("llama3", info.modelName)
    }

    // ── Health ──────────────────────────────────────────────────────

    @Test
    fun `healthCheck returns true when ok`() {
        server.enqueue(MockResponse().setBody("""{"status":"ok"}"""))
        assertTrue(api.healthCheck())
    }

    @Test
    fun `healthCheck returns false on server error`() {
        server.enqueue(MockResponse().setResponseCode(500).setBody(""))
        assertFalse(api.healthCheck())
    }
}
