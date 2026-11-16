extends SceneTree
const Refresh = preload("res://src/pcvr_refresh_control.gd")
const Video = preload("res://src/pcvr_stream_settings.gd")
const Timing = preload("res://src/pcvr_timing_policy.gd")
class ControlState extends RefCounted:
	signal changed
	var encoder_capabilities := {"refresh_rates": [20.0, 60.0, 72.0, 90.0]}
class Harness extends Refresh:
	var rates := [72.0, 90.0]
	func _display_rates() -> Array: return rates
class UIState extends RefCounted:
	var _pcvr_page
	var _pcvr_resolution_btn
class DelayedOwner extends Node:
	var ui_controller

func _initialize() -> void:
	var owner := Node.new()
	root.add_child(owner)
	var path := "user://pcvr-refresh-control-unit-test.cfg"
	var settings := Video.new()
	settings.resolution_percent = 75
	settings.bitrate_mbps = 32
	assert(settings.save_to(path) == OK)
	var control := ControlState.new()
	var widget := Harness.new()
	widget.setup(owner, control, path)
	owner.add_child(widget)
	assert(widget.picker.get_item_text(1) == "90 Hz")
	assert(not widget.picker.is_item_disabled(1))
	widget._selected(1)
	settings.load_from(path)
	assert(settings.stream_fps == 90 and settings.resolution_percent == 75 and settings.bitrate_mbps == 32)
	# Other video controls load/save the shared preferences without dropping FPS.
	settings.bitrate_mbps = 48
	assert(settings.save_to(path) == OK)
	control.changed.emit()
	assert(widget.picker.selected == 1)
	control.encoder_capabilities = {"refresh_rates": [20, 60, 72]}
	control.changed.emit()
	assert(widget.picker.is_item_disabled(1))
	widget._selected(0)
	settings.load_from(path)
	assert(settings.stream_fps == 72 and settings.bitrate_mbps == 48)
	control.encoder_capabilities = {"refresh_rates": [20, 60, 72, 90]}
	widget.rates = [72.0]
	widget._refresh()
	assert(widget.picker.is_item_disabled(1), "Do not offer 90 Hz on a display that cannot run it")
	var timing := Timing.new()
	assert(timing.begin(90, 120))
	assert(timing.display_hz == 90 and timing.effective_fps(60) == 90)
	assert(not timing.begin(72, 90))
	timing.end()
	assert(timing.effective_fps(60) == 60)
	assert(DirAccess.remove_absolute(path) == OK)
	owner.free()
	var delayed := DelayedOwner.new()
	root.add_child(delayed)
	Refresh.install.call_deferred(delayed, control)
	await process_frame
	delayed.ui_controller = UIState.new()
	delayed.ui_controller._pcvr_page = VBoxContainer.new()
	var row := HBoxContainer.new()
	delayed.ui_controller._pcvr_page.add_child(row)
	delayed.ui_controller._pcvr_resolution_btn = Button.new()
	delayed.ui_controller._pcvr_resolution_btn.custom_minimum_size = Vector2(256, 134)
	row.add_child(delayed.ui_controller._pcvr_resolution_btn)
	delayed.add_child(delayed.ui_controller._pcvr_page)
	await process_frame
	await process_frame
	assert(delayed.ui_controller._pcvr_page.find_child("PCVRRefreshControl", true, false) != null)
	Refresh.install(delayed, control)
	assert(delayed.ui_controller._pcvr_page.get_child_count() == 1, "Install once after delayed UI creation")
	assert(row.get_child_count() == 2 and row.get_combined_minimum_size().y == 134, "Use the existing video row without clipping lower controls")
	delayed.free()
	print("All pcvr_refresh_control tests passed")
	quit()
