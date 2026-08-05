class_name ProtoBridge
extends RefCounted

## Minimal protobuf3 bridge for the default-world Login and Move flow.
## Floats use protobuf's fixed32 little-endian wire representation.
const WIRE_VARINT := 0
const WIRE_FIXED32 := 5
const WIRE_LENGTH_DELIMITED := 2


static func encode_login_req(token: String) -> PackedByteArray:
	return _encode_length_delimited_field(1, token.to_utf8_buffer())


static func decode_login_res(body: PackedByteArray) -> Dictionary:
	var result := {"success": false, "player_id": ""}
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
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func encode_move_req(position: Vector3, yaw: float) -> PackedByteArray:
	var result := PackedByteArray()
	result.append_array(_encode_length_delimited_field(1, _encode_vec3(position)))
	result.append_array(_encode_fixed32_field(2, yaw))
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
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return result


static func decode_world_state(body: PackedByteArray) -> Array[Dictionary]:
	var players: Array[Dictionary] = []
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
		else:
			offset = _skip_value(body, offset, field["wire_type"])
			if offset < 0:
				break
	return players


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
	var end := length["offset"] + length["value"]
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
