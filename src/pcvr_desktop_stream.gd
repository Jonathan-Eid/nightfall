class_name PCVRDesktopStream
extends RefCounted
const Lifecycle = preload("res://src/session_lifecycle.gd")
## Retire only Moonlight's decoder/connection. The XR lease, native projection,
## pairing, selected app and independent audio belong to other owners.
enum State { RUNNING, DRAINING, STOPPING, SUSPENDED, RESUMING }
var main
var state := State.RUNNING
var wanted := false
var generation := 0
var host_id := -1
var app_id := -1
var elapsed := 0.0
var cooldown := 0.0
var attempts := 0

func _init(owner) -> void:
	main = owner

func process(suspend: bool, delta: float, allow_resume: bool = true) -> void:
	wanted = suspend
	elapsed += delta
	cooldown = maxf(0.0, cooldown - delta)
	# The normal restart callback retires media without welcome-screen cleanup.
	# Observe that acknowledgment rather than replacing its signal ownership.
	if state == State.STOPPING and not main.is_streaming:
		stream_terminated()
	if state == State.DRAINING and not wanted:
		generation += 1
		state = State.RUNNING
		main.session_lifecycle.stream_started()
	if state == State.RESUMING and main.is_streaming and main.session_lifecycle.phase == Lifecycle.Phase.STREAMING:
		state = State.RUNNING
		attempts = 0
		main._log("[XR-DESKTOP] Desktop reconnected")
	if state == State.RUNNING and wanted and main.is_streaming and main.current_host_id >= 0:
		host_id = main.current_host_id
		app_id = main._selected_app_id
		state = State.DRAINING
		generation += 1
		main.session_lifecycle.request_restart()
		_drain(generation)
	elif state == State.SUSPENDED and not wanted and allow_resume and cooldown <= 0.0 and attempts < 3:
		state = State.RESUMING
		elapsed = 0.0
		attempts += 1
		main.session_lifecycle.request_restart()
		main._log("[XR-DESKTOP] Reconnecting desktop")
		main.stream_manager.start_stream(host_id, app_id)
	elif state == State.RESUMING and elapsed > 20.0:
		main._log("[XR-DESKTOP] Desktop reconnect timed out")
		main.stream_backend.stop_play_stream()
		state = State.SUSPENDED
		cooldown = 2.0

func _drain(token: int) -> void:
	if main.video_presentation: main.video_presentation.deactivate_native(false)
	main._clear_comp_yuv_textures()
	for _frame in 2:
		await main.get_tree().process_frame
		if token != generation or state != State.DRAINING: return
	state = State.STOPPING
	elapsed = 0.0
	main._log("[XR-DESKTOP] Stopping hidden desktop stream; independent VR audio active")
	main.stream_backend.stop_play_stream()

func stream_terminated() -> bool:
	if state not in [State.STOPPING, State.SUSPENDED, State.RESUMING]: return false
	main.session_lifecycle.stream_terminated(false, 0)
	state = State.SUSPENDED
	elapsed = 0.0
	cooldown = 0.5
	main._log("[XR-DESKTOP] Desktop stream stopped; XR lease retained")
	return true
