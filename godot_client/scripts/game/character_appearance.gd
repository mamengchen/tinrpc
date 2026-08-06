class_name CharacterAppearance
extends RefCounted

const MODELS: Array[PackedScene] = [
	preload("res://assets/kaykit_characters/Knight.glb"),
	preload("res://assets/kaykit_characters/Barbarian.glb"),
	preload("res://assets/kaykit_characters/Mage.glb"),
	preload("res://assets/kaykit_characters/Rogue.glb"),
]
const NAMES := ["骑士", "野蛮人", "法师", "盗贼"]


static func apply_to(root: Node3D, appearance: int) -> void:
	var safe_id := clampi(appearance, 0, MODELS.size() - 1)
	if root.get_meta("appearance", -1) == safe_id:
		return
	root.set_meta("appearance", safe_id)
	var old := root.get_node_or_null("CharacterVisual")
	if old != null:
		root.remove_child(old)
		old.queue_free()
	for legacy_name in ["Body", "Axe", "Placeholder"]:
		var legacy := root.get_node_or_null(legacy_name)
		if legacy != null:
			legacy.visible = false
	var visual := MODELS[safe_id].instantiate() as Node3D
	visual.name = "CharacterVisual"
	visual.scale = Vector3.ONE * 1.35
	visual.rotation_degrees.y = 180.0
	root.add_child(visual)
	_configure_animations(visual)
	set_moving(root, false)


static func set_moving(root: Node3D, moving: bool) -> void:
	var visual := root.get_node_or_null("CharacterVisual")
	if visual == null:
		return
	var animation_player := _find_animation_player(visual)
	if animation_player == null:
		return
	var target: StringName = visual.get_meta("walk_animation", &"") if moving else visual.get_meta("idle_animation", &"")
	if target == &"":
		return
	if (visual.get_meta("motion_is_moving", not moving) == moving
			and animation_player.current_animation == target
			and animation_player.is_playing()):
		return
	visual.set_meta("motion_is_moving", moving)
	animation_player.play(target, 0.15)


static func _configure_animations(visual: Node) -> void:
	var animation_player := _find_animation_player(visual)
	if animation_player == null:
		return
	var idle := _find_animation(animation_player, ["idle_a", "idle"])
	var walk := _find_animation(animation_player, ["walking_a", "walking", "walk", "running_a", "run"])
	_set_looping(animation_player, idle)
	_set_looping(animation_player, walk)
	visual.set_meta("idle_animation", idle)
	visual.set_meta("walk_animation", walk)
	visual.set_meta("motion_is_moving", true)


static func _set_looping(player: AnimationPlayer, animation_name: StringName) -> void:
	if animation_name == &"":
		return
	var animation := player.get_animation(animation_name)
	if animation != null:
		animation.loop_mode = Animation.LOOP_LINEAR


static func _find_animation(player: AnimationPlayer, candidates: Array[String]) -> StringName:
	for candidate in candidates:
		for animation_name in player.get_animation_list():
			if candidate in String(animation_name).to_lower():
				return animation_name
	return &""


static func _find_animation_player(node: Node) -> AnimationPlayer:
	if node is AnimationPlayer:
		return node as AnimationPlayer
	for child in node.get_children():
		var found := _find_animation_player(child)
		if found != null:
			return found
	return null
