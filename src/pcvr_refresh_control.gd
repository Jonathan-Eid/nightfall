extends VBoxContainer
## Shared PCVR control, also usable by clients retaining the existing desktop UI.
const Video = preload("res://src/pcvr_stream_settings.gd")
var main
var control
var config_path := Video.PATH
var picker := OptionButton.new()
var note := Label.new()

static func install(owner, xr_control) -> void:
	# XR control can be constructed before asynchronous main/UI initialization
	# completes. Wait for its page, and stop if the owner leaves the tree.
	while is_instance_valid(owner) and owner.is_inside_tree():
		var ui = owner.get("ui_controller")
		var page = ui.get("_pcvr_page") if ui != null else null
		if page is Control:
			if page.find_child("PCVRRefreshControl", true, false) != null: return
			var widget = load("res://src/pcvr_refresh_control.gd").new()
			widget.name = "PCVRRefreshControl"
			widget.setup(owner, xr_control)
			var resolution = ui.get("_pcvr_resolution_btn")
			if resolution is Control and resolution.get_parent() is HBoxContainer:
				widget.custom_minimum_size = resolution.custom_minimum_size
				resolution.get_parent().add_child(widget)
			else:
				page.add_child(widget)
				page.move_child(widget, mini(1, page.get_child_count() - 1))
			return
		await owner.get_tree().process_frame

func setup(owner: Node, xr_control, path: String = Video.PATH) -> void:
	main = owner
	control = xr_control
	config_path = path
	alignment = BoxContainer.ALIGNMENT_CENTER
	add_theme_constant_override("separation", 4)
	var label := Label.new()
	label.text = "Refresh rate"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_size_override("font_size", 24)
	add_child(label)
	picker.name = "PCVRRefreshRate"
	picker.custom_minimum_size = Vector2(230, 52)
	picker.add_theme_font_size_override("font_size", 24)
	picker.tooltip_text = "Target video and headset refresh rate. Higher rates need more rendering and encoding capacity. Applies next VR session."
	for rate in Video.REFRESH_RATES: picker.add_item("%d Hz" % rate, rate)
	picker.item_selected.connect(_selected)
	add_child(picker)
	note.add_theme_font_size_override("font_size", 18)
	note.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	add_child(note)
	visibility_changed.connect(_refresh)
	if control.has_signal("changed"): control.changed.connect(_refresh)
	_refresh()

func _display_rates() -> Array:
	var interface = XRServer.find_interface("OpenXR")
	return interface.get_available_display_refresh_rates() if interface else [72.0]

func _refresh() -> void:
	var settings := Video.new()
	settings.load_from(config_path)
	picker.select(Video.REFRESH_RATES.find(settings.stream_fps))
	var host_rates := Video.host_refresh_rates(control.encoder_capabilities)
	var display_rates := _display_rates()
	for i in picker.item_count:
		var rate := picker.get_item_id(i)
		var supported := rate in host_rates and display_rates.any(func(hz): return absf(float(hz) - rate) < 0.6)
		picker.set_item_disabled(i, not supported)
	note.text = "Applies next session"
	if picker.is_item_disabled(picker.selected): note.text = "Selected rate unavailable"

func _selected(index: int) -> void:
	if index < 0 or index >= picker.item_count or picker.is_item_disabled(index): return
	var settings := Video.new()
	settings.load_from(config_path)
	settings.stream_fps = picker.get_item_id(index)
	if settings.save_to(config_path) != OK:
		note.text = "Could not save refresh rate"
		return
	_refresh()
