class_name MapEnvironment
extends Node3D

const PINE_A: Mesh = preload("res://assets/kenney_nature_kit/tree_pineTallA_detailed.obj")
const PINE_C: Mesh = preload("res://assets/kenney_nature_kit/tree_pineTallC.obj")
const OAK: Mesh = preload("res://assets/kenney_nature_kit/tree_oak.obj")
const ROCK_A: Mesh = preload("res://assets/kenney_nature_kit/rock_largeA.obj")
const ROCK_C: Mesh = preload("res://assets/kenney_nature_kit/rock_largeC.obj")
const CAMPFIRE: Mesh = preload("res://assets/kenney_nature_kit/campfire_logs.obj")
const BRIDGE: Mesh = preload("res://assets/kenney_nature_kit/bridge_wood.obj")
const TENT: Mesh = preload("res://assets/kenney_nature_kit/tent_detailedOpen.obj")
const BUSH: Mesh = preload("res://assets/kenney_nature_kit/plant_bushDetailed.obj")
const GRASS: Mesh = preload("res://assets/kenney_nature_kit/grass_large.obj")
const FLOWER: Mesh = preload("res://assets/kenney_nature_kit/flower_yellowA.obj")

var _materials: Dictionary = {}


func _ready() -> void:
	load_map(0)


func load_map(map_id: int) -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()
	var palettes := [
		[Color("8f7651"), Color("287da1"), Color("4d8a46"), Color("bfa56b"), Color("566055")],
		[Color("9a6b3e"), Color("447a88"), Color("b98245"), Color("d8b36a"), Color("75543b")],
		[Color("b8c7cf"), Color("4b7894"), Color("dbe8e8"), Color("c8d5d8"), Color("71818b")],
	]
	var p: Array = palettes[clampi(map_id, 0, 2)]
	_materials = {
		"path": _material(p[0], 1.0),
		"water": _material(p[1], 0.25, 0.18),
		"wood": _material(Color("70452d"), 0.9),
		"cliff": _material(p[4], 1.0),
		"meadow": _material(p[2], 1.0),
		"sand": _material(p[3], 1.0),
	}
	var floor := get_node_or_null("../Floor") as MeshInstance3D
	if floor != null:
		floor.material_override = _materials["meadow"]
	_box("MapGround", Vector3(0, -0.18, 0), Vector3(82, 0.25, 82), _materials["meadow"])
	_build_ground_zones()
	_build_river_and_bridge()
	_build_landmarks()
	_build_perimeter()


func _material(color: Color, roughness: float, metallic := 0.0) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	return material


func _box(name: String, position: Vector3, size: Vector3, material: Material, yaw := 0.0) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var node := MeshInstance3D.new()
	node.name = name
	node.mesh = mesh
	node.material_override = material
	node.position = position
	node.rotation_degrees.y = yaw
	add_child(node)
	return node


func _disc(name: String, position: Vector3, radius: float, material: Material, height := 0.12) -> MeshInstance3D:
	var mesh := CylinderMesh.new()
	mesh.top_radius = radius
	mesh.bottom_radius = radius * 1.08
	mesh.height = height
	mesh.radial_segments = 32
	var node := MeshInstance3D.new()
	node.name = name
	node.mesh = mesh
	node.material_override = material
	node.position = position
	add_child(node)
	return node


func _model(name: String, source: Mesh, position: Vector3, scale_value := 1.0, yaw := 0.0) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	node.name = name
	node.mesh = source
	node.position = position
	node.scale = Vector3.ONE * scale_value
	node.rotation_degrees.y = yaw
	add_child(node)
	return node


func _build_ground_zones() -> void:
	_disc("CentralBuildClearing", Vector3(0, 0.035, 0), 12.0, _materials["meadow"])
	_disc("StoneQuarry", Vector3(-20, 0.055, 15), 8.0, _materials["sand"])
	_box("NorthTrail", Vector3(0, 0.08, -18), Vector3(3.2, 0.12, 28), _materials["path"])
	_box("WestTrail", Vector3(-11, 0.085, 4), Vector3(23, 0.12, 2.6), _materials["path"], -12)
	_box("BridgeApproach", Vector3(15, 0.09, 0), Vector3(16, 0.12, 2.8), _materials["path"])


func _build_river_and_bridge() -> void:
	_box("River", Vector3(20, -0.08, 0), Vector3(7.5, 0.16, 78), _materials["water"])
	_box("WestBank", Vector3(15.8, 0.0, 0), Vector3(1.0, 0.28, 78), _materials["sand"])
	_box("EastBank", Vector3(24.2, 0.0, 0), Vector3(1.0, 0.28, 78), _materials["sand"])
	_model("KenneyWoodBridge", BRIDGE, Vector3(20, 0.15, 0), 5.8, 90)


func _build_landmarks() -> void:
	_disc("CampClearing", Vector3(-17, 0.05, -13), 6.5, _materials["sand"])
	_model("CampfireLandmark", CAMPFIRE, Vector3(-17, 0.15, -13), 3.4, 25)
	_model("ExplorerTent", TENT, Vector3(-21, 0.1, -15), 4.2, 28)
	for offset in [Vector3(-3, 0, -1.5), Vector3(3, 0, -1.2), Vector3(0, 0, 3.2)]:
		_box("CampBench", Vector3(-17, 0.35, -13) + offset, Vector3(2.8, 0.45, 0.65), _materials["wood"], 20)
	for i in range(10):
		var angle := TAU * float(i) / 10.0
		var tree_scene := OAK if i % 3 == 0 else (PINE_A if i % 2 == 0 else PINE_C)
		_model("GroveTree%02d" % i, tree_scene, Vector3(-23 + cos(angle) * 9, 0, -12 + sin(angle) * 8), 3.8 + (i % 3) * 0.35, i * 37)
	for i in range(7):
		_model("QuarryRock%02d" % i, ROCK_A if i % 2 == 0 else ROCK_C, Vector3(-24 + (i % 3) * 4.0, 0, 11 + (i / 3) * 4.0), 3.0 + (i % 2) * 0.55, i * 51)
	for i in range(14):
		var scatter := Vector3(-10 + (i * 13) % 31, 0.05, -25 + (i * 17) % 48)
		_model("Bush%02d" % i, BUSH if i % 2 == 0 else GRASS, scatter, 2.2 + (i % 3) * 0.25, i * 29)
	for i in range(8):
		_model("Wildflower%02d" % i, FLOWER, Vector3(-8 + i * 2.1, 0.06, 8 + (i % 3) * 1.8), 1.8, i * 41)


func _build_perimeter() -> void:
	for i in range(16):
		var angle := TAU * float(i) / 16.0
		var radius := 36.0 + float(i % 3)
		var position := Vector3(cos(angle) * radius, -0.2, sin(angle) * radius)
		var hill_radius := 4.8 + float(i % 3)
		var hill_height := 3.5 + float(i % 3)
		_hill("BoundaryHill%02d" % i, position, hill_radius, hill_height, _materials["cliff"], i * 17)
		if i % 2 == 0:
			_model("BoundaryTree%02d" % i, PINE_A if i % 4 == 0 else PINE_C, position + Vector3(0, hill_height - 0.35, 0), 4.3, i * 43)


func _hill(node_name: String, position: Vector3, radius: float, height: float, material: Material, yaw := 0.0) -> MeshInstance3D:
	var mesh := CylinderMesh.new()
	mesh.top_radius = radius * 0.42
	mesh.bottom_radius = radius
	mesh.height = height
	mesh.radial_segments = 8
	mesh.rings = 2
	var node := MeshInstance3D.new()
	node.name = node_name
	node.mesh = mesh
	node.material_override = material
	node.position = position + Vector3(0, height * 0.5, 0)
	node.rotation_degrees.y = yaw
	add_child(node)
	return node
