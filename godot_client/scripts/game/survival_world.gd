class_name SurvivalWorld
extends Node3D

const TREE_MESH: Mesh = preload("res://assets/kenney_survival/tree.obj")
const ROCK_MESH: Mesh = preload("res://assets/kenney_survival/rock-a.obj")
const FLOOR_MESH: Mesh = preload("res://assets/kenney_survival/structure-floor.obj")
const WALL_MESH: Mesh = preload("res://assets/kenney_survival/structure.obj")
const CAMPFIRE_MESH: Mesh = preload("res://assets/kenney_survival/campfire-pit.obj")

var resources: Dictionary = {}
var buildings: Dictionary = {}


func apply_world_state(state: Dictionary) -> void:
	for resource in state.get("resources", []): apply_resource(resource)
	for building in state.get("buildings", []): apply_building(building)


func apply_resource(resource: Dictionary) -> void:
	var resource_id: String = resource.get("resource_id", "")
	if resource_id.is_empty(): return
	var node: Node3D = resources.get(resource_id)
	if resource.get("remaining", 0) <= 0:
		if node != null: node.queue_free()
		resources.erase(resource_id)
		return
	if node == null:
		var mesh_node := MeshInstance3D.new()
		mesh_node.mesh = TREE_MESH if resource.get("resource_type", 0) == 1 else ROCK_MESH
		node = mesh_node
		node.name = resource_id
		add_child(node)
		resources[resource_id] = node
	node.global_position = resource.get("position", Vector3.ZERO)


func apply_building(building: Dictionary) -> void:
	var building_id: String = building.get("building_id", "")
	if building_id.is_empty() or buildings.has(building_id): return
	var meshes := {1: FLOOR_MESH, 2: WALL_MESH, 3: CAMPFIRE_MESH}
	var node := MeshInstance3D.new()
	node.mesh = meshes.get(building.get("building_type", 0), FLOOR_MESH)
	node.name = building_id
	add_child(node)
	node.global_position = building.get("position", Vector3.ZERO)
	node.rotation_degrees.y = building.get("yaw", 0.0)
	buildings[building_id] = node


func nearest_resource(from: Vector3, max_distance: float) -> String:
	var best_id := ""
	var best_distance := max_distance
	for resource_id in resources:
		var node: Node3D = resources[resource_id]
		var distance := from.distance_to(node.global_position)
		if distance < best_distance:
			best_distance = distance
			best_id = resource_id
	return best_id
