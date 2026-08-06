class_name NetSession
extends Node

signal connected
signal disconnected
signal register_completed(success: bool, player_id: String, error_msg: String)
signal login_completed(success: bool, player_id: String, error_msg: String)
signal map_selected(success: bool, map_id: int, error_msg: String)
signal world_state_received(state: Dictionary)
signal player_entered(player: Dictionary)
signal player_left(player_id: String)
signal move_completed(success: bool, error_msg: String, corrected_position: Vector3)
signal gather_completed(result: Dictionary)
signal building_completed(result: Dictionary)
signal resource_changed(resource: Dictionary)
signal building_placed(building: Dictionary)
signal voxel_edit_received(edit: Dictionary)
signal voxel_edit_completed(result: Dictionary)
signal craft_completed(result: Dictionary)
signal transport_error(method: String)

var _rpc: RpcClient
var _pending_requests: Dictionary = {}
var _move_request_in_flight := false


func _ready() -> void:
	_ensure_client()


func _process(_delta: float) -> void:
	if _rpc != null:
		_rpc.poll()


func connect_to_server(host: String, port: int) -> Error:
	_ensure_client()
	return _rpc.connect_to_host(host, port)


func disconnect_from_server() -> void:
	if _rpc != null:
		_rpc.disconnect_from_host()


func register(username: String, password: String) -> int:
	return _send("Register", ProtoBridge.encode_register_req(username, password), "register")


func login(username: String, password: String) -> int:
	return _send("Login", ProtoBridge.encode_login_req(username, password), "login")


func select_map(room_id: String, map_id: int) -> int:
	return _send("SelectMap", ProtoBridge.encode_select_map_req(room_id, map_id), "select_map")


func send_move(position: Vector3, yaw: float, appearance: int) -> int:
	if _move_request_in_flight:
		return 0
	var request_id := _send("Move", ProtoBridge.encode_move_req(position, yaw, appearance), "move")
	if request_id != 0:
		_move_request_in_flight = true
	return request_id


func gather(resource_id: String) -> int:
	return _send("Gather", ProtoBridge.encode_gather_req(resource_id), "gather")


func place_building(building_type: int, position: Vector3, yaw: float) -> int:
	return _send("PlaceBuilding", ProtoBridge.encode_place_building_req(building_type, position, yaw), "building")


func edit_voxel(edit: Dictionary) -> int:
	return _send("VoxelEdit", ProtoBridge.encode_voxel_edit(edit), "voxel")


func craft(recipe_id: int) -> int:
	return _send("Craft", ProtoBridge.encode_craft_req(recipe_id), "craft")


func _ensure_client() -> void:
	if _rpc != null:
		return
	_rpc = RpcClient.new()
	add_child(_rpc)
	_rpc.connected.connect(func() -> void: connected.emit())
	_rpc.disconnected.connect(_on_disconnected)
	_rpc.notify.connect(_on_notify)
	_rpc.response.connect(_on_response)


func _send(method: String, body: PackedByteArray, request_kind: String) -> int:
	if _rpc == null:
		return 0
	var request_id := _rpc.send_request(method, body)
	if request_id != 0:
		_pending_requests[request_id] = request_kind
	return request_id


func _on_disconnected() -> void:
	_pending_requests.clear()
	_move_request_in_flight = false
	disconnected.emit()


func _on_response(request_id: int, method: String, body: PackedByteArray, is_error: bool) -> void:
	var request_kind: String = _pending_requests.get(request_id, "")
	if request_kind.is_empty():
		return
	_pending_requests.erase(request_id)
	if request_kind == "move":
		_move_request_in_flight = false
	if is_error:
		transport_error.emit(method)
		if request_kind == "register": register_completed.emit(false, "", "服务器拒绝注册")
		elif request_kind == "login": login_completed.emit(false, "", "服务器拒绝登录")
		elif request_kind == "select_map": map_selected.emit(false, 0, "服务器拒绝房间选择")
		elif request_kind == "move": move_completed.emit(false, "服务器拒绝移动请求", Vector3.ZERO)
		elif request_kind == "gather": gather_completed.emit({"success": false, "error_msg": "服务器拒绝采集请求"})
		elif request_kind == "building": building_completed.emit({"success": false, "error_msg": "服务器拒绝建造请求"})
		elif request_kind == "voxel": voxel_edit_completed.emit({"success": false, "error_msg": "服务器拒绝方块操作"})
		elif request_kind == "craft": craft_completed.emit({"success": false, "error_msg": "服务器拒绝制作请求"})
		return
	if request_kind == "register":
		var result := ProtoBridge.decode_register_res(body)
		register_completed.emit(result["success"], result["player_id"], result["error_msg"])
	elif request_kind == "login":
		var result := ProtoBridge.decode_login_res(body)
		login_completed.emit(result["success"], result["player_id"], result["error_msg"])
	elif request_kind == "select_map":
		var result := ProtoBridge.decode_select_map_res(body)
		map_selected.emit(result["success"], result["map_id"], result["error_msg"])
	elif request_kind == "move":
		var result := ProtoBridge.decode_move_res(body)
		move_completed.emit(result["success"], result["error_msg"], result["corrected"])
	elif request_kind == "gather":
		gather_completed.emit(ProtoBridge.decode_gather_res(body))
	elif request_kind == "building":
		building_completed.emit(ProtoBridge.decode_place_building_res(body))
	elif request_kind == "voxel":
		voxel_edit_completed.emit(ProtoBridge.decode_voxel_edit_res(body))
	elif request_kind == "craft":
		craft_completed.emit(ProtoBridge.decode_craft_res(body))


func _on_notify(method: String, body: PackedByteArray) -> void:
	match method:
		"WorldStateNtf": world_state_received.emit(ProtoBridge.decode_world_state(body))
		"PlayerEnterNtf": player_entered.emit(ProtoBridge.decode_player_transform(body))
		"PlayerLeaveNtf": player_left.emit(ProtoBridge.decode_world_player_leave(body))
		"ResourceChangedNtf": resource_changed.emit(ProtoBridge.decode_resource_changed(body))
		"BuildingPlacedNtf": building_placed.emit(ProtoBridge.decode_building_placed(body))
		"VoxelEditNtf": voxel_edit_received.emit(ProtoBridge.decode_voxel_edit_ntf(body))
