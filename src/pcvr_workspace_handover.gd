class_name PCVRWorkspaceHandover
extends RefCounted
## DEV handover over a prepared standby receiver. Never replaces PCVRSession's
## clean-welcome test policy or owns a second OpenXR session/frame loop.
enum State { IDLE, WAITING, DRAINING, ACTIVE, STOPPING }
# From the launch to the game's first video. VRChat spends about 40 s in
# launch.exe and EasyAntiCheat before it opens a real session, and the desktop
# stays usable meanwhile. Matches the 120 s the Steam launch wrapper waits.
const STARTUP_TIMEOUT := 120.0
var main
var state := State.IDLE
var generation := 0
var error := ""
var _control
var _lease := ""
var _elapsed := 0.0
var _application_wait := 0.0
var _drain_started := 0.0
var _saved: Dictionary = {}
var desktop_visible := false
var menu_only := false
var _menu_hidden: Array[Node3D] = []
var _menu_collisions: Dictionary = {}
var _menu_previous_ui := false
var _watch
var desktop_stream
var _independent_audio := false

func _init(owner) -> void:
	main = owner
	desktop_stream = preload("res://src/pcvr_desktop_stream.gd").new(main)

func blocks_workspace() -> bool:
	return owns_workspace() and ((not desktop_visible and not menu_only) or (_watch != null and _watch.interaction_blocked()))

func toggle_pcvr_menu() -> void:
	if state != State.ACTIVE or not owns_workspace(): return
	if menu_only:
		close_pcvr_menu()
	elif desktop_visible and main.ui_visible and main.ui_controller.is_pcvr_page_open():
		main._toggle_ui()
	else:
		open_pcvr_menu()

func open_pcvr_menu() -> void:
	if state != State.ACTIVE or not owns_workspace(): return
	if not desktop_visible and not menu_only:
		_menu_previous_ui = main.ui_visible
		menu_only = true
		_hide_desktop_visuals(main)
	main.xr_interaction.cancel_transient_interactions("PCVR menu focus")
	main.input_handler.release_host_inputs()
	if main.get("controller_mapper") != null: main.controller_mapper.release_for_focus_loss()
	_enforce_hidden()
	main.ui_controller.open_pcvr_page()
	if not main.ui_visible: main._toggle_ui()
	main.pcvr_test.backend.set_workspace_input_focus(true)

func close_pcvr_menu() -> void:
	if not menu_only: return
	main.ui_visible = false
	main._set_ui_visible(false)
	main.composition_panels.hide_ui()
	_restore_menu_visuals()
	menu_only = false
	main.ui_visible = _menu_previous_ui
	main._set_ui_visible(_menu_previous_ui)
	main.pcvr_test.backend.set_workspace_input_focus(desktop_visible)
	_enforce_hidden()

func _hide_desktop_visuals(node: Node) -> void:
	# Preserve the menu, tracking rig and its interaction visuals. Other visual
	# leaves are temporarily hidden; do not change stream/renderer ownership.
	if node == main.ui_panel_3d: return
	if node is CollisionObject3D and node.collision_layer != 0:
		_menu_collisions[node] = node.collision_layer
		node.collision_layer = 0
	var allowed: Array = [main.composition_panels.ui_layer,
		main.composition_pointers.cursor_layer, main.composition_pointers.secondary_cursor_layer]
	if node in allowed: return
	if node is Node3D and (node is GeometryInstance3D or node.get_class().begins_with("OpenXRCompositionLayer")):
		if not main.xr_origin.is_ancestor_of(node) or node.get_class().begins_with("OpenXRCompositionLayer"):
			if node.visible:
				_menu_hidden.append(node)
				node.visible = false
	for child in node.get_children(): _hide_desktop_visuals(child)

func _restore_menu_visuals() -> void:
	for node in _menu_hidden:
		if is_instance_valid(node): node.visible = true
	_menu_hidden.clear()
	for node in _menu_collisions:
		if is_instance_valid(node) and node.collision_layer == 0:
			node.collision_layer = _menu_collisions[node]
	_menu_collisions.clear()

func owns_workspace() -> bool:
	return not _saved.is_empty()

func set_desktop_visible(value: bool) -> void:
	if state != State.ACTIVE or not owns_workspace() or desktop_visible == value: return
	if menu_only: close_pcvr_menu()
	main.xr_interaction.cancel_transient_interactions("XR desktop focus changed")
	main.input_handler.release_host_inputs()
	if main.get("controller_mapper") != null: main.controller_mapper.release_for_focus_loss()
	if main.virtual_keyboard: main.virtual_keyboard.release_for_focus_loss()
	desktop_visible = value
	main.pcvr_test.backend.set_workspace_input_focus(value)
	main._log("[XR-WATCH] Desktop %s" % ("shown; game actions neutral" if value else "hidden; game actions resume after release"))
	_enforce_hidden()

func arm(control) -> bool:
	if state != State.IDLE or not main.pcvr_test.standby_active: return false
	if not main.pcvr_test.backend.has_method("activate_meteor_projection"): return false
	# Any number of desktop/virtual screens; a browser workspace is not covered yet.
	if main.browser_content_active(): return false
	generation += 1
	state = State.WAITING
	error = ""
	_armed_screen_count = main.screens.size()
	_control = control
	_lease = control.session
	_elapsed = 0.0
	_application_wait = 0.0
	if main.get("settings_controller") != null:
		var rate = control.get("session_fps")
		if not main.settings_controller.pcvr_timing_active() and not main.settings_controller.begin_pcvr_timing(int(rate) if rate != null else 60):
			request_stop("Cannot establish PCVR display timing")
			return false
	main._log("[XR-HANDOVER] Armed; desktop retained until application and decoded frame ready")
	return true

func process(delta: float) -> void:
	var native: Dictionary = {}
	var runtime: Dictionary = {}
	# Resolve lease/native termination before audio failure can request a desktop
	# reconnect. A canceled lease must enter STOPPING before that decision.
	if state not in [State.IDLE, State.STOPPING]:
		if not main.pcvr_test.standby_active or not _control or _control.state != "prepared" or _control.session != _lease:
			request_stop("XR control or receiver ended")
		else:
			native = main.pcvr_test.backend.get_test_status()
			runtime = _control.runtime_status
			var phase := str(runtime.get("phase", ""))
			if str(native.get("state", "failed")) in ["idle", "failed", "stopping"]:
				request_stop("XR native receiver unavailable")
			elif phase in ["completed", "cancelled", "failed"] or (phase == "session_ended" and state != State.WAITING):
				# Preliminary Steam/Unity sessions may end before the first frame.
				request_stop("XR application ended")
	var audio_ready := false
	if owns_workspace() and state in [State.DRAINING, State.ACTIVE]:
		audio_ready = bool(native.get("meteor_audio_ready", false)) and not bool(native.get("meteor_audio_failed", false))
	if main.pcvr_test.backend != null and main.pcvr_test.backend.has_method("set_meteor_audio_enabled"):
		# Switch exactly one player on. Never unmute the independent stream while
		# the ordinary desktop audio player remains audible.
		if audio_ready: main.audio_player.stream_paused = true
		main.pcvr_test.backend.set_meteor_audio_enabled(audio_ready and not bool(_saved.get("audio_paused", true)))
	_independent_audio = audio_ready
	if state == State.STOPPING and not main.pcvr_test.standby_active:
		if owns_workspace(): _restore_workspace()
		_reset() # Restore desktop timing before reconnect allocates any resources.
	# A desktop reconnect can change the display rate and allocate decoder/EGL
	# resources. On VR exit, wait for the native receiver to finish retiring its
	# resources before beginning that transition.
	var allow_resume: bool = state != State.STOPPING or not main.pcvr_test.standby_active
	desktop_stream.process(state == State.ACTIVE and audio_ready and not desktop_visible, delta, allow_resume)
	if state == State.IDLE: return
	_elapsed += delta
	if state == State.ACTIVE:
		if not _watch: _watch = preload("res://src/pcvr_wristwatch.gd").new(main)
		_watch.process(delta, self)
	if owns_workspace(): _enforce_hidden()
	if state == State.STOPPING:
		return
	if state == State.DRAINING and _elapsed - _drain_started >= 3.0:
		request_stop("Desktop layer retirement timed out")
		return
	if state == State.WAITING:
		if runtime.get("phase", "") == "ready_for_application":
			_application_wait += delta
			_elapsed = 0.0
			if _application_wait >= 35.0: request_stop("XR application launch wait timed out")
			return # Runtime readiness is not permission to hide the desktop.
		if _elapsed >= STARTUP_TIMEOUT:
			request_stop("XR handover startup timed out")
		elif runtime.get("phase", "") == "session_active" and native.get("state", "") == "running" and bool(native.get("meteor_frame_ready", false)):
			if main.get("settings_controller") != null and not main.settings_controller.pcvr_desktop_timing_ready():
				return # Finish desktop rate restart before presenting the XR world.
			if main.screens.size() != _armed_screen_count or main.browser_content_active():
				request_stop("Workspace changed during XR startup")
				return
			state = State.DRAINING
			_drain_started = _elapsed
			if not _capture_and_hide():
				request_stop("Cannot safely suspend desktop presentation")
				return
			_queue_layer_drain(generation)
		return

func request_stop(reason: String = "Local exit") -> void:
	if state in [State.IDLE, State.STOPPING]: return
	if menu_only: close_pcvr_menu()
	generation += 1 # Invalidate every frame/draw continuation before cleanup.
	error = reason
	state = State.STOPPING
	desktop_visible = false
	if _watch: _watch.reset()
	main._log("[XR-HANDOVER] Stopping: " + reason)
	main.pcvr_test.request_stop()

func handle_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		request_stop()

func _capture_and_hide() -> bool:
	var interface = _xr_interface()
	if not interface or not interface.has_method("get_submit_projection_layer") or not interface.has_method("set_submit_projection_layer"):
		main._log("[XR-HANDOVER] Patched OpenXR projection getter/setter unavailable")
		return false
	var renderer = main.native_xr_renderer
	_saved = {"visible": main.visible, "projection": interface.get_submit_projection_layer(),
		"blend": interface.environment_blend_mode, "transparent": main.get_viewport().transparent_bg,
		"audio_paused": main.audio_player.stream_paused, "mic_paused": main.meteor_microphone._paused,
		"native_suspended": renderer.presentation_suspended}
	if not renderer.set_presentation_suspended(true):
		main._log("[XR-HANDOVER] Native desktop presentation suspension unavailable")
		_saved.clear()
		return false
	main.xr_interaction.cancel_transient_interactions("XR handover")
	main.input_handler.release_host_inputs()
	if main.get("controller_mapper") != null: main.controller_mapper.release_for_focus_loss()
	if main.virtual_keyboard: main.virtual_keyboard.release_for_focus_loss()
	_enforce_hidden()
	return true

func _enforce_hidden() -> void:
	# Hide the parent, not every child: stream termination and other legitimate
	# lifecycle callbacks can still update child visibility/content while hidden.
	main.visible = desktop_visible or menu_only
	_set_screen_rendering(desktop_visible or menu_only)
	# Keep desktop audio unless the independent XR audio transport is active.
	main.audio_player.stream_paused = _independent_audio or not main.is_streaming or bool(_saved.get("audio_paused", true))
	# Keep the microphone in its prior state for in-game voice chat.
	main.native_xr_renderer.set_presentation_suspended(not desktop_visible)
	var interface = _xr_interface()
	if interface:
		interface.set_submit_projection_layer(false)
		interface.environment_blend_mode = XRInterface.XR_ENV_BLEND_MODE_OPAQUE
	main.get_viewport().transparent_bg = false

# Several screens are drawn by the legacy compositor, whose viewports keep
# rendering while the scene is hidden. Stop them while the game is in front
# and let the normal presentation path rebuild them when the desktop returns.
var _screens_paused := false
var _armed_screen_count := 0

func _set_screen_rendering(on: bool) -> void:
	if on == not _screens_paused: return
	if on:
		_screens_paused = false
		if main.get("settings_controller") != null: main.settings_controller.apply_stereo()
		return
	if main.screens.size() < 2: return
	_screens_paused = true
	for scr in main.screens:
		for viewport in [scr.comp_viewport, scr.comp_viewport_left, scr.comp_viewport_right]:
			if viewport: viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED

func _queue_layer_drain(token: int) -> void:
	# Keep all desktop swapchains alive while the compositor retires their use.
	for _frame in 3:
		await main.get_tree().process_frame
		if token != generation or state != State.DRAINING: return
		_enforce_hidden()
		await RenderingServer.frame_post_draw
		if token != generation or state != State.DRAINING: return
	_layers_drained(token)

func _layers_drained(token: int) -> void:
	if token != generation or state != State.DRAINING: return
	if not _control or _control.state != "prepared" or _control.session != _lease or _control.runtime_status.get("phase", "") != "session_active":
		request_stop("XR application ended before activation")
		return
	if not main.pcvr_test.standby_active or not main.pcvr_test.backend.activate_meteor_projection():
		request_stop("XR activation rejected")
		return
	state = State.ACTIVE
	main._log("[XR-HANDOVER] XR projection activated")

func _restore_workspace() -> void:
	# Native idle is the prerequisite. Never force welcome, reconnect Sunshine,
	# restore stale stream state, or overwrite any saved user setting here.
	var saved := _saved.duplicate()
	_saved.clear()
	desktop_visible = false
	if _watch: _watch.reset()
	main.native_xr_renderer.set_presentation_suspended(saved.native_suspended)
	var interface = _xr_interface()
	if interface:
		interface.set_submit_projection_layer(saved.projection)
		interface.environment_blend_mode = saved.blend
	main.get_viewport().transparent_bg = saved.transparent
	main.visible = saved.visible
	_set_screen_rendering(true)
	main.audio_player.stream_paused = saved.audio_paused or not main.is_streaming
	main.meteor_microphone.set_paused(saved.mic_paused or main.pcvr_test.application_paused)
	if main.has_method("reset_primary_shortcuts"): main.reset_primary_shortcuts()
	main._log("[XR-HANDOVER] Native XR drained; previous workspace restored")

func _reset() -> void:
	state = State.IDLE
	if main.get("settings_controller") != null: main.settings_controller.end_pcvr_timing()
	_control = null
	_lease = ""

func _xr_interface():
	return XRServer.find_interface("OpenXR")
