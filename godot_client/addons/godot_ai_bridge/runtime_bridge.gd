extends Node
## Runtime automation harness for the Godot AI Bridge.

const MESSAGE_PREFIX := "godot_runtime"

var _capture_callable: Callable
var _mouse_position: Vector2 = Vector2.ZERO
var _mouse_button_mask: int = 0


func _enter_tree() -> void:
	if not EngineDebugger.is_active():
		return

	_capture_callable = Callable(self, "_capture_debugger_message")
	EngineDebugger.register_message_capture(MESSAGE_PREFIX, _capture_callable)
	call_deferred("_announce_ready")


func _exit_tree() -> void:
	if _capture_callable.is_valid():
		EngineDebugger.unregister_message_capture(MESSAGE_PREFIX)
		_capture_callable = Callable()


func _announce_ready() -> void:
	if EngineDebugger.is_active():
		EngineDebugger.send_message("%s:ready" % MESSAGE_PREFIX, [_build_status_payload()])


func _capture_debugger_message(message: String, data: Array) -> bool:
	if message != "request":
		return false

	if data.size() < 3:
		_send_response(-1, {"success": false, "error": "Invalid runtime request payload."})
		return true

	var request_id := int(data[0])
	var method := str(data[1])
	var params: Dictionary = data[2] if data[2] is Dictionary else {}

	call_deferred("_process_request_async", request_id, method, params)
	return true


func _process_request_async(request_id: int, method: String, params: Dictionary) -> void:
	var response: Dictionary

	match method:
		"status":
			response = {"success": true, "result": _build_status_payload()}
		"wait":
			response = {"success": true, "result": await _handle_wait(params)}
		"press_action":
			response = {"success": true, "result": await _handle_press_action(params)}
		"release_action":
			response = {"success": true, "result": await _handle_release_action(params)}
		"tap_action":
			response = {"success": true, "result": await _handle_tap_action(params)}
		"mouse_move":
			response = {"success": true, "result": await _handle_mouse_move(params)}
		"click":
			response = {"success": true, "result": await _handle_click(params)}
		"type_text":
			response = {"success": true, "result": await _handle_type_text(params)}
		"capture_screenshot":
			response = await _handle_capture_screenshot(params)
		_:
			response = {
				"success": false,
				"error": "Unknown runtime automation method: " + method
			}

	_send_response(request_id, response)


func _handle_wait(params: Dictionary) -> Dictionary:
	var frames := max(0, int(params.get("frames", 0)))
	var seconds := max(0.0, float(params.get("seconds", 0.0)))

	if frames == 0 and seconds <= 0.0:
		frames = 1

	for _i in range(frames):
		await get_tree().process_frame

	if seconds > 0.0:
		await get_tree().create_timer(seconds).timeout

	return {
		"frames": frames,
		"seconds": seconds
	}


func _handle_press_action(params: Dictionary) -> Dictionary:
	var action := _require_non_empty_string(params, "action")
	var strength := float(params.get("strength", 1.0))
	_push_action_event(action, true, strength)
	await get_tree().process_frame
	return {
		"action": action,
		"pressed": true,
		"strength": strength
	}


func _handle_release_action(params: Dictionary) -> Dictionary:
	var action := _require_non_empty_string(params, "action")
	_push_action_event(action, false, 0.0)
	await get_tree().process_frame
	return {
		"action": action,
		"pressed": false
	}


func _handle_tap_action(params: Dictionary) -> Dictionary:
	var action := _require_non_empty_string(params, "action")
	var frames := max(1, int(params.get("frames", 1)))
	var strength := float(params.get("strength", 1.0))

	_push_action_event(action, true, strength)
	for _i in range(frames):
		await get_tree().process_frame
	_push_action_event(action, false, 0.0)
	await get_tree().process_frame

	return {
		"action": action,
		"frames": frames,
		"strength": strength
	}


func _handle_mouse_move(params: Dictionary) -> Dictionary:
	var target := _require_position(params)
	await _push_mouse_motion(target)
	return {
		"position": _vector2_dict(_mouse_position)
	}


func _handle_click(params: Dictionary) -> Dictionary:
	var button := int(params.get("button", MOUSE_BUTTON_LEFT))
	var hold_frames := max(1, int(params.get("holdFrames", 1)))

	if params.has("x") or params.has("y"):
		var target := _require_position(params)
		await _push_mouse_motion(target)

	var click_position := _mouse_position
	await _push_mouse_button(click_position, button, true)
	for _i in range(hold_frames):
		await get_tree().process_frame
	await _push_mouse_button(click_position, button, false)

	return {
		"button": button,
		"hold_frames": hold_frames,
		"position": _vector2_dict(click_position)
	}


func _handle_type_text(params: Dictionary) -> Dictionary:
	var text := _require_non_empty_string(params, "text")
	var root := _get_root_viewport()
	root.push_text_input(text)
	await get_tree().process_frame

	return {
		"text": text,
		"focused_control": _focused_control_path()
	}


func _handle_capture_screenshot(params: Dictionary) -> Dictionary:
	var raw_path := _require_non_empty_string(params, "path")
	var output_path := _resolve_output_path(raw_path)
	var output_dir := output_path.get_base_dir()

	if not output_dir.is_empty():
		var dir_err := DirAccess.make_dir_recursive_absolute(output_dir)
		if dir_err != OK and dir_err != ERR_ALREADY_EXISTS:
			return {
				"success": false,
				"error": "Failed to create screenshot directory: " + output_dir
			}

	await RenderingServer.frame_post_draw

	var root := _get_root_viewport()
	var image := root.get_texture().get_image()
	if image == null:
		return {
			"success": false,
			"error": "Failed to capture screenshot image from the root viewport."
		}

	var save_err := image.save_png(output_path)
	if save_err != OK:
		return {
			"success": false,
			"error": "Failed to save screenshot: " + error_string(save_err)
		}

	return {
		"success": true,
		"result": {
			"path": output_path,
			"width": image.get_width(),
			"height": image.get_height()
		}
	}


func _push_action_event(action: String, pressed: bool, strength: float) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = pressed
	event.strength = strength if pressed else 0.0
	Input.parse_input_event(event)


func _push_mouse_motion(target: Vector2) -> void:
	var root := _get_root_viewport()
	var event := InputEventMouseMotion.new()
	event.position = target
	event.global_position = target
	event.relative = target - _mouse_position
	event.velocity = Vector2.ZERO
	event.button_mask = _mouse_button_mask
	root.push_input(event, true)
	_mouse_position = target
	await get_tree().process_frame


func _push_mouse_button(target: Vector2, button: int, pressed: bool) -> void:
	var root := _get_root_viewport()
	var mask := _button_mask_for(button)
	var next_mask := _mouse_button_mask
	if pressed:
		next_mask |= mask
	else:
		next_mask &= ~mask

	var event := InputEventMouseButton.new()
	event.position = target
	event.global_position = target
	event.button_index = button
	event.pressed = pressed
	event.button_mask = next_mask
	root.push_input(event, true)

	_mouse_button_mask = next_mask
	_mouse_position = target
	await get_tree().process_frame


func _button_mask_for(button: int) -> int:
	match button:
		MOUSE_BUTTON_LEFT:
			return MOUSE_BUTTON_MASK_LEFT
		MOUSE_BUTTON_RIGHT:
			return MOUSE_BUTTON_MASK_RIGHT
		MOUSE_BUTTON_MIDDLE:
			return MOUSE_BUTTON_MASK_MIDDLE
		MOUSE_BUTTON_XBUTTON1:
			return MOUSE_BUTTON_MASK_MB_XBUTTON1
		MOUSE_BUTTON_XBUTTON2:
			return MOUSE_BUTTON_MASK_MB_XBUTTON2
		_:
			return 0


func _get_root_viewport() -> Viewport:
	var tree := get_tree()
	if tree == null or tree.root == null:
		push_error("[AI Bridge] Runtime automation requires an active scene tree root viewport.")
		return null
	return tree.root


func _build_status_payload() -> Dictionary:
	var root := _get_root_viewport()
	var current_scene := get_tree().current_scene
	var focused_control := _focused_control_path()

	return {
		"available": true,
		"scene_root": current_scene.name if current_scene else "",
		"scene_path": current_scene.scene_file_path if current_scene else "",
		"viewport_size": _vector2_dict(root.get_visible_rect().size) if root else _vector2_dict(Vector2.ZERO),
		"mouse_position": _vector2_dict(_mouse_position),
		"focused_control": focused_control
	}


func _focused_control_path() -> String:
	var root := _get_root_viewport()
	if root == null:
		return ""
	var focused = root.gui_get_focus_owner()
	if focused:
		return str(focused.get_path())
	return ""


func _require_non_empty_string(params: Dictionary, key: String) -> String:
	var value := str(params.get(key, "")).strip_edges()
	if value.is_empty():
		push_error("[AI Bridge] Missing required runtime parameter: " + key)
	return value


func _require_position(params: Dictionary) -> Vector2:
	var has_x := params.has("x")
	var has_y := params.has("y")
	if not has_x or not has_y:
		push_error("[AI Bridge] Runtime position requires both x and y values.")
		return _mouse_position
	return Vector2(float(params.get("x", _mouse_position.x)), float(params.get("y", _mouse_position.y)))


func _resolve_output_path(raw_path: String) -> String:
	var path := raw_path.strip_edges()
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	if path.begins_with("/") or path.contains(":/"):
		return path
	return ProjectSettings.globalize_path("res://" + path)


func _vector2_dict(value: Vector2) -> Dictionary:
	return {
		"x": value.x,
		"y": value.y
	}


func _send_response(request_id: int, payload: Dictionary) -> void:
	if EngineDebugger.is_active():
		EngineDebugger.send_message("%s:response" % MESSAGE_PREFIX, [request_id, payload])
