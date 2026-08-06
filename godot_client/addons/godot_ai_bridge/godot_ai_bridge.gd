@tool
extends EditorPlugin
## Main plugin for Godot AI Bridge

const WSServer = preload("res://addons/godot_ai_bridge/ws_server.gd")
const MessageHandler = preload("res://addons/godot_ai_bridge/message_handler.gd")
const RUNTIME_AUTOLOAD_NAME := "GodotAIBridgeRuntime"
const RUNTIME_AUTOLOAD_PATH := "res://addons/godot_ai_bridge/runtime_bridge.gd"

var _ws_server: Node
var _message_handler: RefCounted
var _debugger_plugin: AIBridgeDebuggerPlugin
var _port: int = 6550


func _enter_tree() -> void:
	_ensure_runtime_autoload()

	_message_handler = MessageHandler.new()
	_message_handler.editor_interface = get_editor_interface()
	_message_handler.undo_redo = get_undo_redo()

	# Capture runtime output, errors, and automation responses.
	_debugger_plugin = AIBridgeDebuggerPlugin.new()
	_debugger_plugin.message_handler = _message_handler
	_message_handler.debugger_plugin = _debugger_plugin
	add_debugger_plugin(_debugger_plugin)

	_ws_server = WSServer.new()
	_ws_server.port = _port
	_ws_server.message_received.connect(_on_message_received)
	_ws_server.client_connected.connect(_on_client_connected)
	_ws_server.client_disconnected.connect(_on_client_disconnected)

	add_child(_ws_server)

	var err = _ws_server.start()
	if err == OK:
		print("[AI Bridge] Server started on port ", _port)
	else:
		push_error("[AI Bridge] Failed to start server: " + str(err))


func _exit_tree() -> void:
	if _debugger_plugin:
		remove_debugger_plugin(_debugger_plugin)
		_debugger_plugin = null
	if _ws_server:
		_ws_server.stop()
		_ws_server.queue_free()
		_ws_server = null
	_message_handler = null


func _ensure_runtime_autoload() -> void:
	var autoload_key := "autoload/%s" % RUNTIME_AUTOLOAD_NAME
	var existing_value := str(ProjectSettings.get_setting(autoload_key, ""))
	var normalized_existing := existing_value.trim_prefix("*")

	if normalized_existing.is_empty():
		add_autoload_singleton(RUNTIME_AUTOLOAD_NAME, RUNTIME_AUTOLOAD_PATH)
		return

	if normalized_existing != RUNTIME_AUTOLOAD_PATH:
		push_warning(
			"[AI Bridge] Autoload name conflict for %s: %s"
			% [RUNTIME_AUTOLOAD_NAME, existing_value]
		)


func _on_message_received(peer_id: int, message: String) -> void:
	call_deferred("_send_response_async", peer_id, message)


func _send_response_async(peer_id: int, message: String) -> void:
	var response: String = await _message_handler.handle_message(message)
	if _ws_server:
		_ws_server.send_message(peer_id, response)


func _on_client_connected(peer_id: int) -> void:
	print("[AI Bridge] Client connected: ", peer_id)


func _on_client_disconnected(peer_id: int) -> void:
	print("[AI Bridge] Client disconnected: ", peer_id)


## Debugger plugin to capture output, errors, and runtime automation responses.
class AIBridgeDebuggerPlugin extends EditorDebuggerPlugin:
	var message_handler: RefCounted

	const FILTERED_MESSAGES := [
		"game_view:cursor_set_shape",
		"game_view:mouse_over",
	]
	const RUNTIME_PREFIX := "godot_runtime"
	const DEFAULT_RUNTIME_TIMEOUT_SEC := 5.0

	var _tracked_session_ids: Array[int] = []
	var _session_runtime_ready: Dictionary = {}
	var _pending_runtime_requests: Dictionary = {}
	var _next_runtime_request_id: int = 1

	func _has_capture(_prefix: String) -> bool:
		return true

	func _capture(message: String, data: Array, session_id: int) -> bool:
		if not message_handler:
			return false

		match message:
			"%s:ready" % RUNTIME_PREFIX:
				_handle_runtime_ready(data, session_id)
				return true
			"%s:response" % RUNTIME_PREFIX:
				_handle_runtime_response(data, session_id)
				return true

		for filtered in FILTERED_MESSAGES:
			if message == filtered:
				return false

		match message:
			"output":
				_handle_output(data)
			"error":
				_handle_error(data)
			"debug_enter":
				_handle_debug_enter(data)
			"stack_dump":
				_handle_stack_dump(data)
			"stack_frame_vars":
				pass
			"debug_exit":
				message_handler.log_output("[DEBUG] Resumed execution", "debug", "debugger")
			_:
				if data.size() > 0 and not message.begins_with("performance") and not message.begins_with("servers"):
					message_handler.log_output(
						"[MSG:%s] %s" % [message, _format_data(data)],
						"debug",
						"debugger"
					)

		return false

	func send_runtime_request(
		method: String,
		params: Dictionary,
		timeout_seconds: float = DEFAULT_RUNTIME_TIMEOUT_SEC
	) -> Dictionary:
		var session_id := _get_active_session_id()
		if session_id == -1:
			return {
				"error": {
					"code": -32603,
					"message": "No active runtime debug session. Run a scene before using runtime automation tools."
				}
			}

		var session = get_session(session_id)
		if not session:
			return {
				"error": {
					"code": -32603,
					"message": "Failed to access the active runtime debug session."
				}
			}

		var request_id := _next_runtime_request_id
		_next_runtime_request_id += 1

		_pending_runtime_requests[request_id] = {
			"ready": false,
			"payload": {},
			"session_id": session_id
		}

		session.send_message("%s:request" % RUNTIME_PREFIX, [request_id, method, params])

		var deadline := Time.get_ticks_msec() + int(max(timeout_seconds, 0.1) * 1000.0)
		while Time.get_ticks_msec() < deadline:
			var pending = _pending_runtime_requests.get(request_id)
			if pending is Dictionary and bool(pending.get("ready", false)):
				_pending_runtime_requests.erase(request_id)
				return _coerce_runtime_reply(pending.get("payload", {}), method)
			await Engine.get_main_loop().process_frame

		_pending_runtime_requests.erase(request_id)
		return {
			"error": {
				"code": -32603,
				"message": (
					"Timed out waiting for runtime automation response for %s. "
					+ "Make sure the scene is running and the runtime harness is loaded."
				) % method
			}
		}

	func _setup_session(session_id: int) -> void:
		if not _tracked_session_ids.has(session_id):
			_tracked_session_ids.append(session_id)

		_session_runtime_ready[session_id] = false

		var session = get_session(session_id)
		if not session:
			return

		var started_callable := Callable(self, "_on_session_started").bind(session_id)
		if not session.started.is_connected(started_callable):
			session.started.connect(started_callable)

		var stopped_callable := Callable(self, "_on_session_stopped").bind(session_id)
		if not session.stopped.is_connected(stopped_callable):
			session.stopped.connect(stopped_callable)

		if session.is_active():
			_on_session_started(session_id)

	func _on_session_started(session_id: int) -> void:
		_session_runtime_ready[session_id] = false
		if message_handler:
			message_handler.log_output(
				"[DEBUG] Game started (session %d)" % session_id,
				"info",
				"debugger"
			)

	func _on_session_stopped(session_id: int) -> void:
		_session_runtime_ready.erase(session_id)
		_fail_pending_requests_for_session(
			session_id,
			"Runtime debug session stopped before the automation request completed."
		)

		if message_handler:
			message_handler.log_output(
				"[DEBUG] Game stopped (session %d)" % session_id,
				"info",
				"debugger"
			)

	func _get_active_session_id() -> int:
		for i in range(_tracked_session_ids.size() - 1, -1, -1):
			var session_id = _tracked_session_ids[i]
			var session = get_session(session_id)
			if session and session.is_active():
				return session_id
		return -1

	func _handle_runtime_ready(data: Array, session_id: int) -> void:
		_session_runtime_ready[session_id] = true

		var details := ""
		if data.size() > 0 and data[0] is Dictionary:
			var payload: Dictionary = data[0]
			var scene_root := str(payload.get("scene_root", ""))
			if not scene_root.is_empty():
				details = " for %s" % scene_root

		if message_handler:
			message_handler.log_output(
				"[RUNTIME] Automation harness ready%s (session %d)" % [details, session_id],
				"info",
				"debugger"
			)

	func _handle_runtime_response(data: Array, session_id: int) -> void:
		if data.size() < 2:
			if message_handler:
				message_handler.log_output(
					"[RUNTIME] Ignoring malformed runtime response for session %d" % session_id,
					"warning",
					"debugger"
				)
			return

		var request_id := int(data[0])
		if not _pending_runtime_requests.has(request_id):
			return

		var pending: Dictionary = _pending_runtime_requests[request_id]
		pending["ready"] = true
		pending["payload"] = data[1]
		pending["session_id"] = session_id
		_pending_runtime_requests[request_id] = pending

	func _coerce_runtime_reply(payload, method: String) -> Dictionary:
		if payload is Dictionary:
			var payload_dict: Dictionary = payload
			if bool(payload_dict.get("success", false)):
				return {"result": payload_dict.get("result", {})}
			return {
				"error": {
					"code": -32603,
					"message": str(
						payload_dict.get(
							"error",
							"Runtime automation request failed for " + method
						)
					)
				}
			}

		return {"result": payload}

	func _fail_pending_requests_for_session(session_id: int, error_message: String) -> void:
		var pending_ids := _pending_runtime_requests.keys()
		for request_id in pending_ids:
			var pending: Dictionary = _pending_runtime_requests.get(request_id, {})
			if int(pending.get("session_id", -1)) != session_id:
				continue
			pending["ready"] = true
			pending["payload"] = {
				"success": false,
				"error": error_message
			}
			_pending_runtime_requests[request_id] = pending

	func _handle_output(data: Array) -> void:
		for item in data:
			if item is String:
				var clean: String = (item as String).strip_edges()
				if not clean.is_empty():
					message_handler.log_output(clean, "info", "runtime")

	func _handle_error(data: Array) -> void:
		if data.size() >= 5:
			var is_warning: bool = data[0] if data[0] is bool else false
			var func_name: String = str(data[1]) if data.size() > 1 else ""
			var file_path: String = str(data[2]) if data.size() > 2 else ""
			var line_num: int = data[3] if data.size() > 3 and data[3] is int else 0
			var error_msg: String = str(data[4]) if data.size() > 4 else ""

			var prefix := "[WARN]" if is_warning else "[ERROR]"
			var location := ""
			if not file_path.is_empty():
				location = " @ %s:%d" % [file_path.get_file(), line_num]
				if not func_name.is_empty():
					location += " in %s()" % func_name

			message_handler.log_output(
				"%s%s: %s" % [prefix, location, error_msg],
				"warning" if is_warning else "error",
				"runtime"
			)

			message_handler.log_error({
				"type": "warning" if is_warning else "error",
				"file": file_path,
				"line": line_num,
				"function": func_name,
				"message": error_msg
			})
		else:
			message_handler.log_output("[ERROR] %s" % _format_data(data), "error", "runtime")

	func _handle_debug_enter(data: Array) -> void:
		if data.size() >= 2:
			var reason: String = str(data[1]) if data.size() > 1 else "unknown"
			message_handler.log_output("[BREAK] Debugger paused: %s" % reason, "warning", "debugger")

	func _handle_stack_dump(data: Array) -> void:
		message_handler.log_output("[STACK] Call stack:", "debug", "debugger")
		for i in range(0, data.size(), 3):
			if i + 2 < data.size():
				var file: String = str(data[i])
				var line: int = data[i + 1] if data[i + 1] is int else 0
				var func_name: String = str(data[i + 2])
				message_handler.log_output(
					"  → %s:%d in %s()" % [file.get_file(), line, func_name],
					"debug",
					"debugger"
				)

	func _format_data(data: Array) -> String:
		if data.size() == 0:
			return "(empty)"
		if data.size() == 1:
			return str(data[0])
		var parts: PackedStringArray = []
		for item in data:
			parts.append(str(item).left(100))
		return ", ".join(parts)
