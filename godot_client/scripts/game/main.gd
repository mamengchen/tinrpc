extends Node3D

@export var host := "127.0.0.1"
@export var port := 8080
@export var token := "player_a"
@export var move_speed := 4.0
@export var move_send_interval := 0.10

@onready var _local_player: Node3D = $LocalPlayer
@onready var _spawner: PlayerSpawner = $PlayerSpawner

var _session: NetSession
var _last_sent_position := Vector3.INF
var _last_move_sent_at := 0.0


func _ready() -> void:
	_spawner.local_player_id = token
	_session = NetSession.new()
	add_child(_session)
	_session.connected.connect(_on_connected)
	_session.login_completed.connect(_on_login_completed)
	_session.world_state_received.connect(_spawner.apply_world_state)
	_session.player_entered.connect(_spawner.apply_player_transform)
	_session.player_left.connect(_spawner.despawn_player)
	_session.move_completed.connect(_on_move_completed)

	var connect_error := _session.connect_to_server(host, port)
	if connect_error != OK:
		push_error("无法连接 TinRpc %s:%d（错误码 %d）" % [host, port, connect_error])


func _process(delta: float) -> void:
	var movement := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	if movement.length_squared() > 0.0:
		_local_player.position += Vector3(movement.x, 0.0, movement.y) * move_speed * delta
		_local_player.rotation.y = atan2(-movement.x, -movement.y)

	_send_current_move_if_due()


func _on_connected() -> void:
	_session.login(token)


func _on_login_completed(success: bool, player_id: String, error_msg: String) -> void:
	if not success:
		push_error("登录失败：%s" % error_msg)
		return
	_spawner.local_player_id = player_id
	_send_current_move_if_due(true)


func _on_move_completed(success: bool, _error_msg: String, corrected_position: Vector3) -> void:
	if not success:
		_local_player.global_position = corrected_position
		_last_sent_position = corrected_position


func _send_current_move_if_due(force := false) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if not force and now - _last_move_sent_at < move_send_interval:
		return
	if not force and _local_player.global_position.is_equal_approx(_last_sent_position):
		return

	if _session.send_move(_local_player.global_position, _local_player.rotation_degrees.y) != 0:
		_last_move_sent_at = now
		_last_sent_position = _local_player.global_position
