class_name NetSession
extends Node

signal connected
signal disconnected
signal login_completed(success: bool, player_id: String, error_msg: String)
signal world_state_received(players: Array[Dictionary])
signal player_entered(player: Dictionary)
signal player_left(player_id: String)
signal move_completed(success: bool, error_msg: String, corrected_position: Vector3)
signal transport_error(method: String)

var _rpc: RpcClient
var _pending_requests: Dictionary = {}


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


func login(token: String) -> int:
	return _send("Login", ProtoBridge.encode_login_req(token), "login")


func send_move(position: Vector3, yaw: float) -> int:
	return _send("Move", ProtoBridge.encode_move_req(position, yaw), "move")


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
	disconnected.emit()


func _on_response(request_id: int, method: String, body: PackedByteArray, is_error: bool) -> void:
	var request_kind: String = _pending_requests.get(request_id, "")
	if request_kind.is_empty():
		return
	_pending_requests.erase(request_id)

	if is_error:
		transport_error.emit(method)
		if request_kind == "login":
			login_completed.emit(false, "", "服务端拒绝登录请求")
		elif request_kind == "move":
			move_completed.emit(false, "服务端拒绝移动请求", Vector3.ZERO)
		return

	if request_kind == "login":
		var login_result := ProtoBridge.decode_login_res(body)
		login_completed.emit(login_result["success"], login_result["player_id"], "")
	elif request_kind == "move":
		var move_result := ProtoBridge.decode_move_res(body)
		move_completed.emit(move_result["success"], move_result["error_msg"], move_result["corrected"])


func _on_notify(method: String, body: PackedByteArray) -> void:
	match method:
		"WorldStateNtf":
			world_state_received.emit(ProtoBridge.decode_world_state(body))
		"PlayerEnterNtf":
			player_entered.emit(ProtoBridge.decode_player_transform(body))
		"PlayerLeaveNtf":
			player_left.emit(ProtoBridge.decode_world_player_leave(body))
