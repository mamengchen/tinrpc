class_name VoxelWorld
extends Node3D

signal block_mined(block_type: int)
signal block_placed(block_type: int)

const GRASS := 1
const DIRT := 2
const STONE := 3
const WOOD := 4
const COPPER := 5
const BLOCK_NAMES := {GRASS: "草方块", DIRT: "泥土", STONE: "石头", WOOD: "木材", COPPER: "铜矿"}

var blocks: Dictionary = {}
var selected_type := DIRT
var _materials: Dictionary = {}
var _highlight: MeshInstance3D
var _target := Vector3i.ZERO


func _ready() -> void:
	_materials = {
		GRASS: _mat(Color("58a84f")), DIRT: _mat(Color("79513a")),
		STONE: _mat(Color("7c8589")), WOOD: _mat(Color("8a5a32")),
		COPPER: _mat(Color("ba6d3c")),
	}
	_generate_base()
	_highlight = MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3.ONE * 1.08
	_highlight.mesh = box
	var highlight_material := StandardMaterial3D.new()
	highlight_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	highlight_material.albedo_color = Color(1, 0.9, 0.2, 0.24)
	highlight_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_highlight.material_override = highlight_material
	add_child(_highlight)


func update_target(player: Node3D) -> void:
	var forward := -player.global_transform.basis.z
	var point := player.global_position + forward * 2.2
	_target = Vector3i(roundi(point.x), 0, roundi(point.z))
	_highlight.global_position = Vector3(_target) + Vector3(0, 0.5, 0)


func mine_target() -> bool:
	var top := _top_block(_target.x, _target.z)
	if top == null: return false
	var kind: int = top.get_meta("block_type", DIRT)
	blocks.erase(_key(top.position))
	top.queue_free()
	block_mined.emit(kind)
	return true


func mine_intent() -> Dictionary:
	var top := _top_block(_target.x, _target.z)
	if top == null: return {}
	var cell := Vector3i(roundi(top.position.x), roundi(top.position.y - 0.5), roundi(top.position.z))
	return {"x": cell.x, "y": cell.y, "z": cell.z, "action": 1, "block_type": int(top.get_meta("block_type", DIRT))}


func place_intent() -> Dictionary:
	var y := 0
	while blocks.has(_key(Vector3i(_target.x, y, _target.z))): y += 1
	if y > 3: return {}
	return {"x": _target.x, "y": y, "z": _target.z, "action": 2, "block_type": selected_type}


func apply_network_edit(edit: Dictionary) -> void:
	var cell := Vector3i(edit.get("x", 0), edit.get("y", 0), edit.get("z", 0))
	var key := _key(cell)
	if edit.get("action", 0) == 1:
		var node: Node = blocks.get(key)
		if node != null: node.queue_free()
		blocks.erase(key)
	elif edit.get("action", 0) == 2 and not blocks.has(key):
		_add_block(cell, clampi(edit.get("block_type", DIRT), GRASS, COPPER))


func place_target() -> bool:
	var y := 0
	while blocks.has(_key(Vector3i(_target.x, y, _target.z))): y += 1
	if y > 3: return false
	_add_block(Vector3i(_target.x, y, _target.z), selected_type)
	block_placed.emit(selected_type)
	return true


func _generate_base() -> void:
	for x in range(-9, 10):
		for z in range(-9, 10):
			_add_block(Vector3i(x, -1, z), DIRT)
			_add_block(Vector3i(x, 0, z), GRASS if (x + z) % 7 else STONE)
	for p in [Vector3i(-5, 1, -3), Vector3i(4, 1, 5), Vector3i(6, 1, -4)]:
		_add_block(p, COPPER)


func _add_block(cell: Vector3i, kind: int) -> void:
	var mesh := MeshInstance3D.new()
	var cube := BoxMesh.new()
	cube.size = Vector3.ONE
	mesh.mesh = cube
	mesh.material_override = _materials[kind]
	mesh.position = Vector3(cell) + Vector3(0, 0.5, 0)
	mesh.set_meta("block_type", kind)
	add_child(mesh)
	blocks[_key(cell)] = mesh


func _top_block(x: int, z: int) -> MeshInstance3D:
	for y in range(3, -2, -1):
		var node = blocks.get(_key(Vector3i(x, y, z)))
		if node != null: return node
	return null


func _key(cell: Vector3i) -> String:
	return "%d:%d:%d" % [cell.x, cell.y, cell.z]


func _mat(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.92
	return material
