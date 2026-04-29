extends Node


func _ready():
	print("--- TESTING SINGLETON ---")

	# Since we registered it as a singleton, we can access it via Engine
	if ResonanceServerAccess.has_server():
		var server = ResonanceServerAccess.get_server()
		print("Singleton found!")
		print("Status: ", server.get_version())
		print("Is Initialized: ", server.is_initialized())
	else:
		print("ERROR: Singleton 'ResonanceServer' not found.")

	print("--- END TEST ---")
