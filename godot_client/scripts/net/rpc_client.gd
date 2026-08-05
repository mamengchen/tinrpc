class_name RpcClient
extends Node

## The game loop must call poll() once per frame to advance the TCP connection
## and dispatch received TinRpc frames.

signal connected
signal disconnected
signal notify(method: String, body: PackedByteArray)
signal rpc_error(method: String, body: PackedByteArray)
signal response(request_id: int, method: String, body: PackedByteArray, is_error: bool)

var _peer: StreamPeerTCP
var _receive_buffer := PackedByteArray()
var _next_request_id := 1
var _is_connected := false


func connect_to_host(host: String, port: int) -> Error:
	_peer = StreamPeerTCP.new()
	_receive_buffer.clear()
	_is_connected = false
	return _peer.connect_to_host(host, port)


func disconnect_from_host() -> void:
	if _peer == null:
		return
	_peer.disconnect_from_host()
	_handle_disconnected()


## Advances connection state, reads available bytes, and emits received frames.
## Call this from the game's per-frame update; this node does not poll itself.
func poll() -> void:
	if _peer == null:
		return

	var poll_error := _peer.poll()
	if poll_error != OK:
		_handle_disconnected()
		return

	var status := _peer.get_status()
	if status == StreamPeerTCP.STATUS_CONNECTED:
		if not _is_connected:
			_is_connected = true
			connected.emit()
		_read_available_bytes()
		return

	if _is_connected or status == StreamPeerTCP.STATUS_ERROR:
		_handle_disconnected()


## Sends a request frame and returns its non-zero request ID.
## Returns 0 when the peer is not connected or the frame cannot be sent.
func send_request(method: String, body: PackedByteArray) -> int:
	if _peer == null or _peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		return 0

	var request_id := _take_request_id()
	var frame := FrameCodec.encode(request_id, FrameCodec.TYPE_REQUEST, method, body)
	if frame.is_empty() or _peer.put_data(frame) != OK:
		return 0
	return request_id


func _read_available_bytes() -> void:
	while _peer.get_available_bytes() > 0:
		var read_result := _peer.get_data(_peer.get_available_bytes())
		if read_result[0] != OK:
			_handle_disconnected()
			return
		_receive_buffer.append_array(read_result[1])

	var decoded := FrameCodec.decode_frames(_receive_buffer)
	_receive_buffer = decoded["leftover"]
	for frame in decoded["frames"]:
		_dispatch_frame(frame)


func _dispatch_frame(frame: Dictionary) -> void:
	var request_id: int = frame["request_id"]
	var msg_type: int = frame["msg_type"]
	var method: String = frame["method"]
	var body: PackedByteArray = frame["body"]

	# TinRpc server pushes use Response frames with request_id 0, while peers may
	# also send request-shaped notifications. Neither is an RPC response.
	if msg_type == FrameCodec.TYPE_REQUEST or request_id == 0:
		notify.emit(method, body)
		return

	var is_error := msg_type == FrameCodec.TYPE_ERROR
	response.emit(request_id, method, body, is_error)
	if is_error:
		rpc_error.emit(method, body)


func _take_request_id() -> int:
	var request_id := _next_request_id
	_next_request_id = (_next_request_id + 1) & 0xFFFFFFFF
	if _next_request_id == 0:
		_next_request_id = 1
	return request_id


func _handle_disconnected() -> void:
	if _is_connected:
		_is_connected = false
		disconnected.emit()
