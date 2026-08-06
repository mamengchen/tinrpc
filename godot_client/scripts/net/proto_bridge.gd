class_name ProtoBridge
extends RefCounted

## Minimal protobuf3 bridge for the default-world Login and Move flow.
## Floats use protobuf's fixed32 little-endian wire representation.
const WIRE_VARINT := 0
const WIRE_FIXED32 := 5
const WIRE_LENGTH_DELIMITED := 2


static func encode_register_req(username: String, password: String) -> PackedByteArray:
	return _encode_credentials(username, password)


static func decode_register_res(body: PackedByteArray) -> Dictionary:
	var result := {"success": false, "error_msg": "", "player_id": ""}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["success"] = value["value"] != 0
			offset = value["offset"]
		elif field["number"] in [2, 3] and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var text := _read_length_delimited(body, offset)
			if not text["ok"]: break
			result["error_msg" if field["number"] == 2 else "player_id"] = text["value"].get_string_from_utf8()
			offset = text["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0: break
	return result


static func encode_login_req(username: String, password := "") -> PackedByteArray:
	return _encode_credentials(username, password)


static func _encode_credentials(username: String, password: String) -> PackedByteArray:
	var result := PackedByteArray()
	result.append_array(_encode_length_delimited_field(1, username.to_utf8_buffer()))
	if not password.is_empty():
		result.append_array(_encode_length_delimited_field(2, password.to_utf8_buffer()))
	return result


static func decode_login_res(body: PackedByteArray) -> Dictionary:
	var result := {"success": false, "player_id": "", "error_msg": ""}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]:
				break
			result["success"] = value["value"] != 0
			offset = value["offset"]
		elif field["number"] == 2 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var nested := _read_length_delimited(body, offset)
			if not nested["ok"]:
				break
			result["player_id"] = _decode_player_info(nested["value"])["player_id"]
			offset = nested["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var text := _read_length_delimited(body, offset)
			if not text["ok"]: break
			result["error_msg"] = text["value"].get_string_from_utf8()
			offset = text["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func encode_move_req(position: Vector3, yaw: float, appearance := 0) -> PackedByteArray:
	var result := PackedByteArray()
	result.append_array(_encode_length_delimited_field(1, _encode_vec3(position)))
	result.append_array(_encode_fixed32_field(2, yaw))
	if appearance != 0:
		result.append_array(_encode_varint_field(4, clampi(appearance, 0, 3)))
	return result


static func encode_voxel_edit(edit: Dictionary) -> PackedByteArray:
	var result := PackedByteArray()
	for field_number in range(1, 6):
		var raw: int = edit.get(["x", "y", "z", "action", "block_type"][field_number - 1], 0)
		var encoded := ((raw << 1) ^ (raw >> 31)) if field_number <= 3 else raw
		result.append_array(_encode_varint_field(field_number, encoded))
	return result


static func decode_voxel_edit(body: PackedByteArray) -> Dictionary:
	var result := {"x": 0, "y": 0, "z": 0, "action": 0, "block_type": 0}
	var names := ["x", "y", "z", "action", "block_type"]
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if field["number"] >= 1 and field["number"] <= 5 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			var decoded: int = value["value"]
			if field["number"] <= 3: decoded = (decoded >> 1) ^ -(decoded & 1)
			result[names[field["number"] - 1]] = decoded
			offset = value["offset"]
		else: offset = _skip_value(body, offset, field["wire_type"])
	return result


static func decode_voxel_edit_ntf(body: PackedByteArray) -> Dictionary:
	return decode_voxel_edit(_first_nested(body))


static func encode_select_map_req(room_id: String, map_id: int) -> PackedByteArray:
	var result := _encode_length_delimited_field(1, room_id.to_utf8_buffer())
	result.append_array(_encode_varint_field(2, map_id))
	return result


static func decode_select_map_res(body: PackedByteArray) -> Dictionary:
	var result := {"success": false, "map_id": 0, "error_msg": ""}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if field["number"] in [1, 2] and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["success" if field["number"] == 1 else "map_id"] = value["value"] != 0 if field["number"] == 1 else value["value"]
			offset = value["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["error_msg"] = value["value"].get_string_from_utf8()
			offset = value["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
	return result


static func decode_move_res(body: PackedByteArray) -> Dictionary:
	var result := {"success": false, "error_msg": "", "corrected": Vector3.ZERO}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]:
				break
			result["success"] = value["value"] != 0
			offset = value["offset"]
		elif field["number"] == 2 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var message := _read_length_delimited(body, offset)
			if not message["ok"]:
				break
			result["error_msg"] = message["value"].get_string_from_utf8()
			offset = message["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var vec3 := _read_length_delimited(body, offset)
			if not vec3["ok"]:
				break
			result["corrected"] = _decode_vec3(vec3["value"])
			offset = vec3["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func decode_player_transform(body: PackedByteArray) -> Dictionary:
	var result := {
		"player_id": "",
		"player_name": "",
		"position": Vector3.ZERO,
		"yaw": 0.0,
		"appearance": 0,
	}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if (field["number"] == 1 or field["number"] == 2) and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var text := _read_length_delimited(body, offset)
			if not text["ok"]:
				break
			if field["number"] == 1:
				result["player_id"] = text["value"].get_string_from_utf8()
			else:
				result["player_name"] = text["value"].get_string_from_utf8()
			offset = text["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var position := _read_length_delimited(body, offset)
			if not position["ok"]:
				break
			result["position"] = _decode_vec3(position["value"])
			offset = position["offset"]
		elif field["number"] == 4 and field["wire_type"] == WIRE_FIXED32:
			if offset + 4 > body.size():
				break
			result["yaw"] = _read_float32_le(body, offset)
			offset += 4
		elif field["number"] == 5 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["appearance"] = clampi(value["value"], 0, 3)
			offset = value["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func decode_world_state(body: PackedByteArray) -> Dictionary:
	var players: Array[Dictionary] = []
	var resources: Array[Dictionary] = []
	var buildings: Array[Dictionary] = []
	var voxel_edits: Array[Dictionary] = []
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var transform := _read_length_delimited(body, offset)
			if not transform["ok"]:
				break
			players.append(decode_player_transform(transform["value"]))
			offset = transform["offset"]
		elif field["number"] == 2 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var resource := _read_length_delimited(body, offset)
			if not resource["ok"]:
				break
			resources.append(decode_resource(resource["value"]))
			offset = resource["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var building := _read_length_delimited(body, offset)
			if not building["ok"]:
				break
			buildings.append(decode_building(building["value"]))
			offset = building["offset"]
		elif field["number"] == 4 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var edit := _read_length_delimited(body, offset)
			if not edit["ok"]: break
			voxel_edits.append(decode_voxel_edit(edit["value"]))
			offset = edit["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return {"players": players, "resources": resources, "buildings": buildings, "voxel_edits": voxel_edits}


static func encode_gather_req(resource_id: String) -> PackedByteArray:
	return _encode_length_delimited_field(1, resource_id.to_utf8_buffer())


static func encode_place_building_req(building_type: int, position: Vector3, yaw: float) -> PackedByteArray:
	var result := PackedByteArray()
	result.append_array(_encode_varint_field(1, building_type))
	result.append_array(_encode_length_delimited_field(2, _encode_vec3(position)))
	result.append_array(_encode_fixed32_field(3, yaw))
	return result


static func decode_resource(body: PackedByteArray) -> Dictionary:
	var result := {"resource_id": "", "resource_type": 0, "position": Vector3.ZERO, "remaining": 0}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["resource_id"] = value["value"].get_string_from_utf8()
			offset = value["offset"]
		elif (field["number"] == 2 or field["number"] == 4) and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["resource_type" if field["number"] == 2 else "remaining"] = value["value"]
			offset = value["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["position"] = _decode_vec3(value["value"])
			offset = value["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0: break
	return result


static func decode_building(body: PackedByteArray) -> Dictionary:
	var result := {"building_id": "", "owner_id": "", "building_type": 0, "position": Vector3.ZERO, "yaw": 0.0}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if (field["number"] == 1 or field["number"] == 2) and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["building_id" if field["number"] == 1 else "owner_id"] = value["value"].get_string_from_utf8()
			offset = value["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["building_type"] = value["value"]
			offset = value["offset"]
		elif field["number"] == 4 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["position"] = _decode_vec3(value["value"])
			offset = value["offset"]
		elif field["number"] == 5 and field["wire_type"] == WIRE_FIXED32:
			result["yaw"] = _read_float32_le(body, offset)
			offset += 4
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0: break
	return result


static func decode_gather_res(body: PackedByteArray) -> Dictionary:
	return _decode_action_res(body, true)


static func decode_place_building_res(body: PackedByteArray) -> Dictionary:
	return _decode_action_res(body, false)


static func decode_resource_changed(body: PackedByteArray) -> Dictionary:
	var nested := _first_nested(body)
	return decode_resource(nested) if not nested.is_empty() else {}


static func decode_building_placed(body: PackedByteArray) -> Dictionary:
	var nested := _first_nested(body)
	return decode_building(nested) if not nested.is_empty() else {}


static func _decode_action_res(body: PackedByteArray, is_gather: bool) -> Dictionary:
	var result := {"success": false, "error_msg": "", "inventory": {"wood": 0, "stone": 0}}
	result["resource" if is_gather else "building"] = {}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["success"] = value["value"] != 0
			offset = value["offset"]
		elif field["number"] == 2 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["error_msg"] = value["value"].get_string_from_utf8()
			offset = value["offset"]
		elif field["number"] == 3 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["resource" if is_gather else "building"] = decode_resource(value["value"]) if is_gather else decode_building(value["value"])
			offset = value["offset"]
		elif field["number"] == 4 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var value := _read_length_delimited(body, offset)
			if not value["ok"]: break
			result["inventory"] = _decode_inventory(value["value"])
			offset = value["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0: break
	return result


static func _decode_inventory(body: PackedByteArray) -> Dictionary:
	var result := {"wood": 0, "stone": 0}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]: break
		offset = field["offset"]
		if (field["number"] == 1 or field["number"] == 2) and field["wire_type"] == WIRE_VARINT:
			var value := _read_varint(body, offset)
			if not value["ok"]: break
			result["wood" if field["number"] == 1 else "stone"] = value["value"]
			offset = value["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0: break
	return result


static func _first_nested(body: PackedByteArray) -> PackedByteArray:
	var field := _read_field_header(body, 0)
	if not field["ok"] or field["number"] != 1 or field["wire_type"] != WIRE_LENGTH_DELIMITED:
		return PackedByteArray()
	var value := _read_length_delimited(body, field["offset"])
	return value["value"] if value["ok"] else PackedByteArray()


static func decode_world_player_leave(body: PackedByteArray) -> String:
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var player_id := _read_length_delimited(body, offset)
			return player_id["value"].get_string_from_utf8() if player_id["ok"] else ""
		offset = _skip_value(body, offset, field["wire_type"])
		if offset < 0:
			break
	return ""


static func _encode_vec3(value: Vector3) -> PackedByteArray:
	var result := PackedByteArray()
	result.append_array(_encode_fixed32_field(1, value.x))
	result.append_array(_encode_fixed32_field(2, value.y))
	result.append_array(_encode_fixed32_field(3, value.z))
	return result


static func _decode_vec3(body: PackedByteArray) -> Vector3:
	var result := Vector3.ZERO
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["wire_type"] == WIRE_FIXED32 and field["number"] >= 1 and field["number"] <= 3:
			if offset + 4 > body.size():
				break
			var value := _read_float32_le(body, offset)
			if field["number"] == 1:
				result.x = value
			elif field["number"] == 2:
				result.y = value
			else:
				result.z = value
			offset += 4
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func _decode_player_info(body: PackedByteArray) -> Dictionary:
	var result := {"player_id": ""}
	var offset := 0
	while offset < body.size():
		var field := _read_field_header(body, offset)
		if not field["ok"]:
			break
		offset = field["offset"]
		if field["number"] == 1 and field["wire_type"] == WIRE_LENGTH_DELIMITED:
			var player_id := _read_length_delimited(body, offset)
			if not player_id["ok"]:
				break
			result["player_id"] = player_id["value"].get_string_from_utf8()
			offset = player_id["offset"]
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func _encode_length_delimited_field(field_number: int, value: PackedByteArray) -> PackedByteArray:
	var result := _write_varint((field_number << 3) | WIRE_LENGTH_DELIMITED)
	result.append_array(_write_varint(value.size()))
	result.append_array(value)
	return result


static func _encode_fixed32_field(field_number: int, value: float) -> PackedByteArray:
	var result := _write_varint((field_number << 3) | WIRE_FIXED32)
	result.append_array(_write_float32_le(value))
	return result


static func _encode_varint_field(field_number: int, value: int) -> PackedByteArray:
	var result := _write_varint((field_number << 3) | WIRE_VARINT)
	result.append_array(_write_varint(value))
	return result


static func _write_varint(value: int) -> PackedByteArray:
	var result := PackedByteArray()
	while value > 0x7F:
		result.append((value & 0x7F) | 0x80)
		value >>= 7
	result.append(value)
	return result


static func _read_varint(body: PackedByteArray, offset: int) -> Dictionary:
	var value := 0
	var shift := 0
	while offset < body.size() and shift < 64:
		var byte := body[offset]
		offset += 1
		value |= (byte & 0x7F) << shift
		if (byte & 0x80) == 0:
			return {"ok": true, "value": value, "offset": offset}
		shift += 7
	return {"ok": false, "value": 0, "offset": offset}


static func _read_field_header(body: PackedByteArray, offset: int) -> Dictionary:
	var tag := _read_varint(body, offset)
	if not tag["ok"] or tag["value"] == 0:
		return {"ok": false, "offset": offset}
	return {
		"ok": true,
		"number": tag["value"] >> 3,
		"wire_type": tag["value"] & 0x07,
		"offset": tag["offset"],
	}


static func _read_length_delimited(body: PackedByteArray, offset: int) -> Dictionary:
	var length := _read_varint(body, offset)
	if not length["ok"] or length["value"] < 0:
		return {"ok": false, "offset": offset}
	var end: int = int(length["offset"]) + int(length["value"])
	if end > body.size():
		return {"ok": false, "offset": offset}
	return {"ok": true, "value": body.slice(length["offset"], end), "offset": end}


static func _skip_value(body: PackedByteArray, offset: int, wire_type: int) -> int:
	if wire_type == WIRE_VARINT:
		var value := _read_varint(body, offset)
		return value["offset"] if value["ok"] else -1
	if wire_type == WIRE_FIXED32:
		return offset + 4 if offset + 4 <= body.size() else -1
	if wire_type == WIRE_LENGTH_DELIMITED:
		var message := _read_length_delimited(body, offset)
		return message["offset"] if message["ok"] else -1
	return -1


static func _write_float32_le(value: float) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = false
	buffer.put_float(value)
	return buffer.data_array


static func _read_float32_le(body: PackedByteArray, offset: int) -> float:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = false
	buffer.data_array = body.slice(offset, offset + 4)
	return buffer.get_float()
