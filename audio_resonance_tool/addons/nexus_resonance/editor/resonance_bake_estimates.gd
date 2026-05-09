extends Object
class_name ResonanceBakeEstimates

## Rough probe counts and bake duration strings for the bake UI (no editor dependency).

const BAKE_RAY_BASE_SEC_PER_PROBE := 0.001
const BAKE_RAY_BASE_COUNT := 4096


static func estimate_probe_count(vol: Node) -> int:
	if not vol or not vol.has_method("get") or not ("region_size" in vol and "spacing" in vol):
		return -1
	var extents: Vector3 = vol.get("region_size") * 0.5
	var spacing: float = vol.get("spacing")
	if spacing <= 0:
		return -1
	var gen_type: int = vol.get("generation_type") if "generation_type" in vol else 1
	var nx := int(ceil(extents.x * 2 / spacing))
	var nz := int(ceil(extents.z * 2 / spacing))
	if gen_type == 1:
		return nx * nz
	var ny := int(ceil(extents.y * 2 / spacing))
	return nx * ny * nz


## [param bc] must be a [ResonanceBakeConfig]-like resource with bake_num_rays, pathing_enabled, etc.
static func estimate_bake_time(vol: Node, bc: Resource) -> String:
	if bc == null:
		return ""
	var count := estimate_probe_count(vol)
	if count < 0:
		return ""
	var rays = bc.bake_num_rays
	var bounces = bc.bake_num_bounces
	var threads = bc.bake_num_threads
	var pathing = bc.pathing_enabled
	var sec_per_probe: float = (
		BAKE_RAY_BASE_SEC_PER_PROBE * (rays / float(BAKE_RAY_BASE_COUNT)) * bounces / max(1, threads)
	)
	var total = count * sec_per_probe
	if pathing:
		total *= 2.0
	if total < 60:
		return "~%d s" % int(ceil(total))
	if total < 3600:
		return "~%d min" % int(ceil(total / 60))
	return "~%.1f h" % (total / 3600)
