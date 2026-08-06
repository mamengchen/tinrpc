extends Node3D

const VOXEL_WORLD_SCRIPT = preload("res://scripts/game/voxel_world.gd")

@export var host := "127.0.0.1"
@export var port := 8080
@export var token := "player1"
@export var move_speed := 5.0
@export var move_send_interval := 0.10

@onready var _local_player: Node3D = $LocalPlayer
@onready var _spawner: PlayerSpawner = $PlayerSpawner
@onready var _world: SurvivalWorld = $SurvivalWorld
@onready var _voxels = $VoxelWorld
@onready var _voxel_info: Label = $HUD/Margin/VBox/VoxelInfo
@onready var _camera: Camera3D = get_node_or_null("Camera3D") as Camera3D
@onready var _status: Label = $HUD/Margin/VBox/Status
@onready var _inventory: Label = $HUD/Margin/VBox/Inventory
@onready var _build_info: Label = $HUD/Margin/VBox/BuildInfo
@onready var _character_info: Label = $HUD/Margin/VBox/CharacterInfo
@onready var _preview: MeshInstance3D = $BuildPreview
@onready var _hud: CanvasLayer = $HUD
@onready var _login_ui: CanvasLayer = $LoginUI
@onready var _room_ui: CanvasLayer = $RoomUI
@onready var _room_status: Label = $RoomUI/Center/Panel/VBox/RoomStatus
@onready var _login_status: Label = $LoginUI/Center/Panel/VBox/LoginStatus
@onready var _username_input: LineEdit = $LoginUI/Center/Panel/VBox/Username
@onready var _password_input: LineEdit = $LoginUI/Center/Panel/VBox/Password
@onready var _register_button: Button = $LoginUI/Center/Panel/VBox/AccountActions/Register
@onready var _login_button: Button = $LoginUI/Center/Panel/VBox/AccountActions/Login
@onready var _player_buttons: Array[Button] = [
	$LoginUI/Center/Panel/VBox/Player1,
	$LoginUI/Center/Panel/VBox/Player2,
	$LoginUI/Center/Panel/VBox/Player3,
]

var _session: NetSession
var _last_sent_position := Vector3.INF
var _last_move_sent_at := 0.0
var _selected_building := 1
var _build_yaw := 0.0
var _logged_in := false
var _selected_appearance := 0
var _auto_player := 0
var _auto_walk := false
var _auto_map := -1
var _auto_voxel_test := false
var _quick_account_login := false
var _pending_password := ""
var _loaded_voxel_edits := 0
const DEMO_PASSWORD := "123456"


func _ready() -> void:
	if _camera == null:
		_camera = Camera3D.new()
		_camera.name = "RuntimeCamera"
		_camera.current = true
		add_child(_camera)
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--token="):
			token = argument.trim_prefix("--token=")
		elif argument.begins_with("--host="):
			host = argument.trim_prefix("--host=")
		elif argument.begins_with("--auto-player="):
			_auto_player = clampi(argument.trim_prefix("--auto-player=").to_int(), 1, 3)
		elif argument == "--auto-walk":
			_auto_walk = true
		elif argument.begins_with("--auto-map="):
			_auto_map = clampi(argument.trim_prefix("--auto-map=").to_int(), 0, 2)
		elif argument == "--auto-voxel-test":
			_auto_voxel_test = true
	_spawner.local_player_id = token
	_hud.visible = false
	_local_player.visible = false
	_set_login_buttons_enabled(false)
	for index in range(_player_buttons.size()):
		_player_buttons[index].pressed.connect(_select_player.bind(index + 1))
	_register_button.pressed.connect(_register_custom_account)
	_login_button.pressed.connect(_login_custom_account)
	_password_input.text_submitted.connect(func(_value: String) -> void: _login_custom_account())
	_session = NetSession.new()
	add_child(_session)
	_session.connected.connect(_on_connected)
	_session.register_completed.connect(_on_register_completed)
	_session.disconnected.connect(func() -> void: _status.text = "已断开服务器连接")
	_session.login_completed.connect(_on_login_completed)
	_session.map_selected.connect(_on_map_selected)
	for map_id in range(3):
		get_node("RoomUI/Center/Panel/VBox/Map%d" % map_id).pressed.connect(_select_map.bind(map_id))
	_session.world_state_received.connect(_on_world_state)
	_session.player_entered.connect(_spawner.apply_player_transform)
	_session.player_left.connect(_spawner.despawn_player)
	_session.move_completed.connect(_on_move_completed)
	_session.gather_completed.connect(_on_gather_completed)
	_session.building_completed.connect(_on_building_completed)
	_session.resource_changed.connect(_world.apply_resource)
	_session.building_placed.connect(_world.apply_building)
	_session.voxel_edit_received.connect(_on_voxel_edit_received)
	var connect_error := _session.connect_to_server(host, port)
	_status.text = "正在连接服务器 %s:%d……" % [host, port]
	if connect_error != OK:
		_status.text = "连接失败，错误码：%d" % connect_error
	_update_build_label()


func _process(delta: float) -> void:
	if not _logged_in:
		return
	var movement := Vector2(
		float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
		float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W)))
	if _auto_walk:
		movement = Vector2(1.0, 0.0)
	if movement.length_squared() > 1.0:
		movement = movement.normalized()
	if movement.length_squared() > 0.0:
		_local_player.position += Vector3(movement.x, 0.0, movement.y) * move_speed * delta
		_local_player.rotation.y = atan2(-movement.x, -movement.y)
	CharacterAppearance.set_moving(_local_player, movement.length_squared() > 0.0)
	_camera.global_position = _local_player.global_position + Vector3(0, 14, 18)
	_camera.look_at(_local_player.global_position, Vector3.UP)
	_handle_actions()
	_voxels.update_target(_local_player)
	_update_preview()
	_send_current_move_if_due()


func _handle_actions() -> void:
	if Input.is_key_pressed(KEY_4): _voxels.selected_type = VOXEL_WORLD_SCRIPT.DIRT
	elif Input.is_key_pressed(KEY_5): _voxels.selected_type = VOXEL_WORLD_SCRIPT.STONE
	elif Input.is_key_pressed(KEY_6): _voxels.selected_type = VOXEL_WORLD_SCRIPT.WOOD
	_voxel_info.text = "快捷栏：[4] 泥土  [5] 石头  [6] 木材｜当前：%s｜鼠标左键挖掘  右键放置" % VOXEL_WORLD_SCRIPT.BLOCK_NAMES[_voxels.selected_type]
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) and not get_meta("mine_block_lock", false):
		var edit: Dictionary = _voxels.mine_intent()
		if not edit.is_empty(): _session.edit_voxel(edit)
		set_meta("mine_block_lock", true)
	if not Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT): set_meta("mine_block_lock", false)
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) and not get_meta("place_block_lock", false):
		var edit: Dictionary = _voxels.place_intent()
		if not edit.is_empty(): _session.edit_voxel(edit)
		set_meta("place_block_lock", true)
	if not Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT): set_meta("place_block_lock", false)
	for appearance in range(4):
		if Input.is_key_pressed(KEY_F1 + appearance) and _selected_appearance != appearance:
			_selected_appearance = appearance
			CharacterAppearance.apply_to(_local_player, _selected_appearance)
			_update_character_label()
			_send_current_move_if_due(true)
	if Input.is_key_pressed(KEY_1): _selected_building = 1
	elif Input.is_key_pressed(KEY_2): _selected_building = 2
	elif Input.is_key_pressed(KEY_3): _selected_building = 3
	if Input.is_key_pressed(KEY_R) and not get_meta("rotate_lock", false):
		_build_yaw = fmod(_build_yaw + 90.0, 360.0)
		set_meta("rotate_lock", true)
	if not Input.is_key_pressed(KEY_R): set_meta("rotate_lock", false)
	if Input.is_key_pressed(KEY_E) and not get_meta("gather_lock", false):
		var resource_id := _world.nearest_resource(_local_player.global_position, 4.0)
		if resource_id.is_empty(): _status.text = "请靠近树木或岩石后再采集"
		else: _session.gather(resource_id)
		set_meta("gather_lock", true)
	if not Input.is_key_pressed(KEY_E): set_meta("gather_lock", false)
	if Input.is_key_pressed(KEY_B) and not get_meta("build_lock", false):
		_session.place_building(_selected_building, _build_position(), _build_yaw)
		set_meta("build_lock", true)
	if not Input.is_key_pressed(KEY_B): set_meta("build_lock", false)
	_update_build_label()


func _build_position() -> Vector3:
	var forward := -_local_player.global_transform.basis.z
	var target := _local_player.global_position + forward * 3.0
	return Vector3(round(target.x), 0.0, round(target.z))


func _update_preview() -> void:
	_preview.global_position = _build_position()
	_preview.rotation_degrees.y = _build_yaw
	_preview.scale = Vector3(2.5, 0.2, 2.5) if _selected_building == 1 else Vector3(2.5, 2.5, 0.2)
	if _selected_building == 3: _preview.scale = Vector3(1.2, 0.5, 1.2)


func _on_connected() -> void:
	_login_status.text = "服务器已连接，请选择玩家"
	_set_login_buttons_enabled(true)
	if _auto_player > 0:
		_select_player.call_deferred(_auto_player)


func _select_player(number: int) -> void:
	token = "player%d" % number
	_pending_password = DEMO_PASSWORD
	_quick_account_login = true
	_selected_appearance = clampi(number - 1, 0, 3)
	_login_status.text = "正在注册或登录玩家 %d……" % number
	_set_login_buttons_enabled(false)
	_session.register(token, DEMO_PASSWORD)


func _register_custom_account() -> void:
	var credentials := _read_credentials()
	if credentials.is_empty():
		return
	token = credentials["username"]
	_pending_password = credentials["password"]
	_quick_account_login = false
	_login_status.text = "正在注册账号：%s……" % token
	_set_account_controls_enabled(false)
	_session.register(token, _pending_password)


func _login_custom_account() -> void:
	var credentials := _read_credentials()
	if credentials.is_empty():
		return
	token = credentials["username"]
	_pending_password = credentials["password"]
	_quick_account_login = false
	_login_status.text = "正在登录账号：%s……" % token
	_set_account_controls_enabled(false)
	_session.login(token, _pending_password)


func _read_credentials() -> Dictionary:
	var username := _username_input.text.strip_edges()
	var password := _password_input.text
	if username.length() < 3:
		_login_status.text = "账号至少需要 3 个字符"
		return {}
	if password.length() < 6:
		_login_status.text = "密码至少需要 6 个字符"
		return {}
	return {"username": username, "password": password}


func _on_register_completed(success: bool, _player_id: String, error_msg: String) -> void:
	if success or (_quick_account_login and error_msg == "username taken"):
		_login_status.text = "账号就绪，正在进入世界……"
		_session.login(token, _pending_password)
		return
	if error_msg == "username taken":
		_login_status.text = "该账号已存在，请点击登录"
		_set_account_controls_enabled(true)
		return
	_login_status.text = "注册失败：%s" % _localized_error(error_msg)
	_set_account_controls_enabled(true)


func _set_login_buttons_enabled(enabled: bool) -> void:
	for button in _player_buttons:
		button.disabled = not enabled
	_register_button.disabled = not enabled
	_login_button.disabled = not enabled


func _set_account_controls_enabled(enabled: bool) -> void:
	_set_login_buttons_enabled(enabled)
	_username_input.editable = enabled
	_password_input.editable = enabled


func _on_login_completed(success: bool, player_id: String, error_msg: String) -> void:
	if not success:
		_login_status.text = "登录失败：%s" % _localized_error(error_msg)
		_set_account_controls_enabled(true)
		return
	_spawner.local_player_id = player_id
	# WorldState can arrive before LoginRes. In that short window the newly
	# authenticated player may have been spawned as a remote placeholder.
	_spawner.despawn_player(player_id)
	_status.text = "在线玩家：%s" % player_id
	_logged_in = true
	_login_ui.visible = false
	_room_ui.visible = true
	_hud.visible = false
	_local_player.visible = false
	CharacterAppearance.apply_to(_local_player, _selected_appearance)
	_update_character_label()
	if _auto_map >= 0:
		_select_map.call_deferred(_auto_map)


func _select_map(map_id: int) -> void:
	_room_status.text = "正在进入地图……"
	for player_id in _spawner.remote_players.keys().duplicate():
		_spawner.despawn_player(player_id)
	_session.select_map("map_%d" % map_id, map_id)


func _on_map_selected(success: bool, map_id: int, error_msg: String) -> void:
	if not success:
		_room_status.text = "进入失败：%s" % _localized_error(error_msg)
		return
	$MapEnvironment.load_map(map_id)
	_room_ui.visible = false
	_hud.visible = true
	_local_player.visible = true
	_status.text = "在线玩家：%s｜地图：%s" % [_spawner.local_player_id, ["青翠河谷", "荒漠遗迹", "雪松高地"][map_id]]
	if _loaded_voxel_edits > 0:
		_status.text += "｜已同步 %d 条方块修改" % _loaded_voxel_edits
	_send_current_move_if_due(true)
	if _auto_voxel_test:
		_run_auto_voxel_test.call_deferred()


func _run_auto_voxel_test() -> void:
	await get_tree().create_timer(0.25).timeout
	_voxels.update_target(_local_player)
	var mine: Dictionary = _voxels.mine_intent()
	if not mine.is_empty(): _session.edit_voxel(mine)
	await get_tree().create_timer(0.2).timeout
	_voxels.selected_type = VOXEL_WORLD_SCRIPT.WOOD
	var place: Dictionary = _voxels.place_intent()
	if not place.is_empty(): _session.edit_voxel(place)
	_status.text = "体素验证完成：已挖掘并放置木方块"


func _on_world_state(state: Dictionary) -> void:
	for player in state.get("players", []):
		if player.get("player_id", "") == _spawner.local_player_id:
			_local_player.global_position = player.get("position", Vector3.ZERO)
			_local_player.rotation_degrees.y = player.get("yaw", 0.0)
			_last_sent_position = _local_player.global_position
			break
	_spawner.apply_world_state(state.get("players", []))
	_world.apply_world_state(state)
	_loaded_voxel_edits = state.get("voxel_edits", []).size()
	for edit in state.get("voxel_edits", []): _voxels.apply_network_edit(edit)


func _on_voxel_edit_received(edit: Dictionary) -> void:
	_voxels.apply_network_edit(edit)
	_status.text = "方块同步成功：服务端已广播本次修改"


func _on_move_completed(success: bool, error_msg: String, corrected_position: Vector3) -> void:
	if success:
		if _status.text.begins_with("位置已由服务器校正"):
			_status.text = "在线玩家：%s｜位置同步正常" % _spawner.local_player_id
		return
	_local_player.global_position = corrected_position
	_last_sent_position = corrected_position
	_status.text = "位置已由服务器校正：%s" % _localized_error(error_msg)


func _on_gather_completed(result: Dictionary) -> void:
	if result.get("success", false):
		_update_inventory(result.get("inventory", {}))
		_status.text = "采集成功：获得 1 份资源"
	else: _status.text = "采集失败：%s" % _localized_error(result.get("error_msg", "unknown"))


func _on_building_completed(result: Dictionary) -> void:
	if result.get("success", false):
		_update_inventory(result.get("inventory", {}))
		_status.text = "建造成功"
	else: _status.text = "建造失败：%s" % _localized_error(result.get("error_msg", "unknown"))


func _update_inventory(value: Dictionary) -> void:
	_inventory.text = "木材：%d    石料：%d" % [value.get("wood", 0), value.get("stone", 0)]


func _update_build_label() -> void:
	var names := {1: "地板（2 木材）", 2: "墙壁（3 木材）", 3: "篝火（3 石料）"}
	_build_info.text = "建造：%s｜[1/2/3] 选择  [R] 旋转  [B] 放置  [E] 采集  [W/A/S/D] 移动" % names[_selected_building]


func _update_character_label() -> void:
	_character_info.text = "人物：%s｜[F1] 骑士  [F2] 野蛮人  [F3] 法师  [F4] 盗贼" % CharacterAppearance.NAMES[_selected_appearance]


func _localized_error(error_msg: String) -> String:
	var known := {
		"unknown": "未知错误",
		"server rejected login": "服务器拒绝登录",
		"server rejected move": "服务器拒绝移动请求",
		"server rejected gather": "服务器拒绝采集请求",
		"server rejected building": "服务器拒绝建造请求",
		"服务器拒绝登录": "服务器拒绝登录",
		"服务器拒绝移动请求": "服务器拒绝移动请求",
		"服务器拒绝采集请求": "服务器拒绝采集请求",
		"服务器拒绝建造请求": "服务器拒绝建造请求",
		"username taken": "玩家名称已经注册",
		"invalid username/password": "玩家名称或密码格式不正确",
		"invalid credentials": "账号或密码错误",
		"movement too fast": "移动同步过快，位置已重新同步",
		"position outside world": "不能移动到地图边界之外",
		"player is not in world": "玩家尚未进入世界",
		"db unavailable": "账号数据库暂时不可用",
		"服务器拒绝注册": "服务器拒绝注册",
	}
	return known.get(error_msg, "服务器返回错误")


func _send_current_move_if_due(force := false) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if not force and now - _last_move_sent_at < move_send_interval: return
	if not force and _local_player.global_position.is_equal_approx(_last_sent_position): return
	if _session.send_move(_local_player.global_position, _local_player.rotation_degrees.y, _selected_appearance) != 0:
		_last_move_sent_at = now
		_last_sent_position = _local_player.global_position
