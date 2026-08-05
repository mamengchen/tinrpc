class_name PlayerSpawner
extends Node3D

signal remote_player_spawned(player_id: String, player_node: Node3D)
signal remote_player_despawned(player_id: String)

var local_player_id := ""
var remote_players: Dictionary = {}


func apply_world_state(players: Array[Dictionary]) -> void:
	var received_ids: Dictionary = {}
	for player in players:
		var player_id: String = player["player_id"]
		if player_id.is_empty() or player_id == local_player_id:
			continue
		received_ids[player_id] = true
		apply_player_transform(player)

	for player_id in remote_players.keys():
		if not received_ids.has(player_id):
			despawn_player(player_id)


func apply_player_transform(player: Dictionary) -> void:
	var player_id: String = player.get("player_id", "")
	if player_id.is_empty() or player_id == local_player_id:
		return

	var player_node: Node3D = remote_players.get(player_id)
	if player_node == null:
		player_node = _spawn_placeholder(player_id)
		remote_players[player_id] = player_node
		remote_player_spawned.emit(player_id, player_node)

	player_node.global_position = player.get("position", Vector3.ZERO)
	player_node.rotation_degrees.y = player.get("yaw", 0.0)


func despawn_player(player_id: String) -> void:
	var player_node: Node3D = remote_players.get(player_id)
	if player_node == null:
		return
	remote_players.erase(player_id)
	player_node.queue_free()
	remote_player_despawned.emit(player_id)


func _spawn_placeholder(player_id: String) -> Node3D:
	var player_node := Node3D.new()
	player_node.name = "Remote_%s" % player_id
	add_child(player_node)

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = BoxMesh.new()
	player_node.add_child(mesh_instance)
	return player_node
