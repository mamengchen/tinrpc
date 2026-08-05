class_name FrameCodec
extends RefCounted

## TinRpc protocol constants. Integer fields are always big-endian.
const MAGIC: int = 0xBABE
const HEADER_SIZE: int = 13
const MAX_FRAME_SIZE: int = 10 * 1024 * 1024

const TYPE_REQUEST: int = 0x01
const TYPE_RESPONSE: int = 0x02
const TYPE_ERROR: int = 0x03


static func encode(
		request_id: int,
		msg_type: int,
		method: String,
		body: PackedByteArray,
	) -> PackedByteArray:
	var method_bytes := method.to_utf8_buffer()
	if method_bytes.size() > 0xFFFF:
		push_error("TinRpc method name exceeds the 65535-byte protocol limit.")
		return PackedByteArray()

	var total_length := HEADER_SIZE + method_bytes.size() + body.size()
	if total_length > MAX_FRAME_SIZE:
		push_error("TinRpc frame exceeds the %d-byte protocol limit." % MAX_FRAME_SIZE)
		return PackedByteArray()

	var frame := PackedByteArray()
	frame.resize(total_length)
	_write_u16_be(frame, 0, MAGIC)
	_write_u32_be(frame, 2, total_length)
	_write_u32_be(frame, 6, request_id)
	frame[10] = msg_type & 0xFF
	_write_u16_be(frame, 11, method_bytes.size())

	var offset := HEADER_SIZE
	for byte in method_bytes:
		frame[offset] = byte
		offset += 1
	for byte in body:
		frame[offset] = byte
		offset += 1
	return frame


## Splits complete TinRpc frames from [param buffer].
##
## Returns a Dictionary with [code]frames[/code] (an Array of
## {request_id, msg_type, method, body} dictionaries) and [code]leftover[/code]
## (the incomplete trailing bytes). Invalid complete frames are discarded so a
## malformed peer cannot stall the rolling receive buffer indefinitely.
static func decode_frames(buffer: PackedByteArray) -> Dictionary:
	var frames: Array[Dictionary] = []
	var offset := 0

	while buffer.size() - offset >= HEADER_SIZE:
		if _read_u16_be(buffer, offset) != MAGIC:
			push_error("TinRpc frame has an invalid magic value.")
			offset += 1
			continue

		var total_length := _read_u32_be(buffer, offset + 2)
		if total_length < HEADER_SIZE or total_length > MAX_FRAME_SIZE:
			push_error("TinRpc frame has an invalid total length: %d." % total_length)
			offset += 1
			continue
		if buffer.size() - offset < total_length:
			break

		var msg_type := buffer[offset + 10]
		var method_length := _read_u16_be(buffer, offset + 11)
		var payload_length := total_length - HEADER_SIZE
		if not _is_message_type(msg_type) or method_length > payload_length:
			push_error("TinRpc frame has an invalid message type or method length.")
			offset += total_length
			continue

		var method_start := offset + HEADER_SIZE
		var body_start := method_start + method_length
		frames.append({
			"request_id": _read_u32_be(buffer, offset + 6),
			"msg_type": msg_type,
			"method": buffer.slice(method_start, body_start).get_string_from_utf8(),
			"body": buffer.slice(body_start, offset + total_length),
		})
		offset += total_length

	return {
		"frames": frames,
		"leftover": buffer.slice(offset),
	}


static func _is_message_type(msg_type: int) -> bool:
	return msg_type == TYPE_REQUEST or msg_type == TYPE_RESPONSE or msg_type == TYPE_ERROR


static func _write_u16_be(bytes: PackedByteArray, offset: int, value: int) -> void:
	bytes[offset] = (value >> 8) & 0xFF
	bytes[offset + 1] = value & 0xFF


static func _write_u32_be(bytes: PackedByteArray, offset: int, value: int) -> void:
	bytes[offset] = (value >> 24) & 0xFF
	bytes[offset + 1] = (value >> 16) & 0xFF
	bytes[offset + 2] = (value >> 8) & 0xFF
	bytes[offset + 3] = value & 0xFF


static func _read_u16_be(bytes: PackedByteArray, offset: int) -> int:
	return (bytes[offset] << 8) | bytes[offset + 1]


static func _read_u32_be(bytes: PackedByteArray, offset: int) -> int:
	return (
		(bytes[offset] << 24)
		| (bytes[offset + 1] << 16)
		| (bytes[offset + 2] << 8)
		| bytes[offset + 3]
	)
