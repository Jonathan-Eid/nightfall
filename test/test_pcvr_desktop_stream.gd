extends SceneTree
const Transport = preload("res://src/pcvr_desktop_stream.gd")
const Lifecycle = preload("res://src/session_lifecycle.gd")
class Backend extends RefCounted:
	var stops := 0
	func stop_play_stream() -> void: stops += 1
class Manager extends RefCounted:
	var starts: Array = []
	func start_stream(host: int, app: int) -> void: starts.append([host, app])
class Owner extends RefCounted:
	var current_host_id := 7
	var _selected_app_id := 42
	var session_lifecycle := Lifecycle.new()
	var stream_backend := Backend.new()
	var stream_manager := Manager.new()
	var is_streaming: bool:
		get: return session_lifecycle.media_active
	func _log(_message: String) -> void: pass
class Harness extends Transport:
	var token := 0
	func _drain(value: int) -> void: token = value
	func drained() -> void:
		if token != generation or state != State.DRAINING: return
		state = State.STOPPING
		main.stream_backend.stop_play_stream()

func _initialize() -> void:
	var owner := Owner.new()
	owner.session_lifecycle.stream_started()
	var transport := Harness.new(owner)
	transport.process(false, .1)
	assert(owner.stream_backend.stops == 0)
	transport.process(true, .1)
	assert(transport.state == Transport.State.DRAINING)
	# Opening the desktop during GPU drain cancels retirement without tearing
	# down the connection. A stale draw continuation cannot stop it afterward.
	transport.process(false, .1)
	transport.drained()
	assert(owner.stream_backend.stops == 0 and owner.is_streaming)
	transport.process(true, .1)
	transport.drained()
	assert(owner.stream_backend.stops == 1)
	assert(transport.stream_terminated())
	assert(not owner.is_streaming and owner.session_lifecycle.is_restarting())
	transport.process(true, 30.0)
	assert(owner.stream_manager.starts.is_empty(), "Hidden desktop must remain disconnected")
	# Opening desktop or losing independent audio requests the same saved host
	# and application, preserving the XR lease and selection.
	transport.process(false, 1.0, false)
	assert(owner.stream_manager.starts.is_empty(), "Wait for native XR retirement before changing desktop/display resources")
	transport.process(false, .6)
	assert(owner.stream_manager.starts == [[7, 42]])
	owner.session_lifecycle.stream_started()
	transport.process(false, .1)
	assert(transport.state == Transport.State.RUNNING)
	assert(not transport.stream_terminated())
	transport.process(true, .1)
	transport.drained()
	assert(transport.stream_terminated())
	transport.process(false, .6)
	assert(owner.stream_manager.starts.size() == 2)
	print("All pcvr_desktop_stream tests passed")
	quit()
