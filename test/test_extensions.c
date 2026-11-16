#define CGLTF_IMPLEMENTATION
#include "../cgltf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Extension combination matrix.
 *
 * Axes asserted for every case:
 *   - classification: known (typed support in cgltf) vs unknown (vendor) name
 *   - declaration:    absent / extensionsUsed / extensionsRequired
 *   - placement:      root object / material / mesh primitive / texture view
 *
 * Required unknown extensions must fail with
 * cgltf_result_unknown_required_extension; everything else must parse
 * successfully and unknown extensions must be preserved verbatim as
 * cgltf_extension entries carrying the raw JSON fragment.
 */

static int num_checks = 0;
static int num_errors = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                               \
		++num_checks;                                                                  \
		if (!(cond)) {                                                                 \
			++num_errors;                                                              \
			fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (message));       \
		}                                                                              \
	} while (0)

/* A minimal but well-formed glTF: one material (with a texture view), one mesh
 * with a single primitive, so extensions can be attached to every object kind.
 * The tokens below are substituted by the individual matrix cases. */
static const char* gltf_template =
"{"
"  \"asset\": {\"version\": \"2.0\"},"
"  \"extensionsUsed\": %s,"
"  \"extensionsRequired\": %s,"
"  \"extensions\": {%s},"
"  \"materials\": [{"
"    \"name\": \"mat\","
"    \"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0, \"extensions\": {%s}}},"
"    \"extensions\": {%s}"
"  }],"
"  \"textures\": [{\"source\": 0}],"
"  \"images\": [{\"uri\": \"x.png\"}],"
"  \"meshes\": [{"
"    \"name\": \"mesh\","
"    \"primitives\": [{\"attributes\": {\"POSITION\": 0}, \"extensions\": {%s}}]"
"  }],"
"  \"accessors\": [{\"componentType\": 5126, \"count\": 1, \"type\": \"VEC3\"}],"
"  \"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 12}],"
"  \"buffers\": [{\"byteLength\": 12}]"
"}";

static cgltf_result parse_string(const char* json, cgltf_data** out_data)
{
	cgltf_options options;
	memset(&options, 0, sizeof(options));
	return cgltf_parse(&options, json, strlen(json), out_data);
}

static int find_extension(const cgltf_extension* extensions, cgltf_size count, const char* name)
{
	for (cgltf_size i = 0; i < count; ++i)
	{
		if (strcmp(extensions[i].name, name) == 0)
		{
			return (int)i;
		}
	}
	return -1;
}

static void assert_raw_fragment(const cgltf_extension* extension, const char* expected_key)
{
	CHECK(extension->data != NULL, "preserved extension keeps raw JSON data");
	CHECK(extension->data[0] == '{', "raw fragment is the extension JSON object");
	CHECK(strstr(extension->data, expected_key) != NULL, "raw fragment contains original payload");
}

/* Wrap the given declaration fragments into a full glTF document. */
static cgltf_result parse_case(
	const char* used,
	const char* required,
	const char* root_ext,
	const char* texture_view_ext,
	const char* material_ext,
	const char* primitive_ext,
	cgltf_data** out_data)
{
	char json[4096];
	snprintf(json, sizeof(json), gltf_template, used, required, root_ext,
		texture_view_ext, material_ext, primitive_ext);
	return parse_string(json, out_data);
}

/* Case 1: no extensions at all parses and exposes empty declaration arrays. */
static void case_no_extensions(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case("[]", "[]", "", "", "", "", &data);

	CHECK(result == cgltf_result_success, "plain glTF without extensions parses");
	if (result == cgltf_result_success)
	{
		CHECK(data->extensions_used_count == 0, "no extensionsUsed entries");
		CHECK(data->extensions_required_count == 0, "no extensionsRequired entries");
		CHECK(data->materials_count == 1 && data->meshes_count == 1, "base document intact");
		cgltf_free(data);
	}
}

/* Case 2: unknown extension in extensionsUsed only -> success, raw data kept at
 * every object kind it is attached to. */
static void case_unknown_used_all_placements(void)
{
	cgltf_data* data = NULL;
	const char* ext = "VENDOR_custom_thing";
	cgltf_result result = parse_case(
		"[\"VENDOR_custom_thing\"]",
		"[]",
		"\"VENDOR_custom_thing\": {\"rootMarker\": 1}",
		"\"VENDOR_custom_thing\": {\"tvMarker\": 2}",
		"\"VENDOR_custom_thing\": {\"matMarker\": 3}",
		"\"VENDOR_custom_thing\": {\"primMarker\": 4}",
		&data);

	CHECK(result == cgltf_result_success, "unknown used-only extension does not fail parsing");
	if (result != cgltf_result_success)
	{
		return;
	}

	CHECK(data->extensions_used_count == 1, "extensionsUsed retains the declaration");
	CHECK(strcmp(data->extensions_used[0], ext) == 0, "extensionsUsed keeps the exact extension name");

	int idx = find_extension(data->data_extensions, data->data_extensions_count, ext);
	CHECK(idx >= 0, "unknown root extension preserved in data_extensions");
	if (idx >= 0) assert_raw_fragment(&data->data_extensions[idx], "rootMarker");

	CHECK(data->materials_count == 1, "material still available");
	idx = find_extension(data->materials[0].extensions, data->materials[0].extensions_count, ext);
	CHECK(idx >= 0, "unknown material extension preserved");
	if (idx >= 0) assert_raw_fragment(&data->materials[0].extensions[idx], "matMarker");

	idx = find_extension(data->materials[0].pbr_metallic_roughness.base_color_texture.extensions,
		data->materials[0].pbr_metallic_roughness.base_color_texture.extensions_count, ext);
	CHECK(idx >= 0, "unknown texture-view extension preserved (previously silently dropped)");
	if (idx >= 0)
	{
		assert_raw_fragment(&data->materials[0].pbr_metallic_roughness.base_color_texture.extensions[idx], "tvMarker");
	}

	CHECK(data->meshes[0].primitives_count == 1, "primitive still available");
	idx = find_extension(data->meshes[0].primitives[0].extensions,
		data->meshes[0].primitives[0].extensions_count, ext);
	CHECK(idx >= 0, "unknown primitive extension preserved");
	if (idx >= 0) assert_raw_fragment(&data->meshes[0].primitives[0].extensions[idx], "primMarker");

	cgltf_free(data);
}

/* Case 3: unknown extension in extensionsRequired -> dedicated error, and the
 * offending name remains reachable in extensions_required for diagnostics. */
static void case_unknown_required_fails(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"VENDOR_custom_thing\"]",
		"[\"VENDOR_custom_thing\"]",
		"\"VENDOR_custom_thing\": {\"rootMarker\": 1}",
		"",
		"",
		"",
		&data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"unregistered required extension returns unknown_required_extension");
	CHECK(data == NULL, "parse failure hands back no data object");
	if (data) cgltf_free(data);
}

/* Case 4: required + used + present unknown extension still fails: presence of
 * payload data does not make an unsupported required extension acceptable. */
static void case_unknown_required_with_payload_fails(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"VENDOR_custom_thing\"]",
		"[\"VENDOR_custom_thing\"]",
		"",
		"\"VENDOR_custom_thing\": {\"tvMarker\": 2}",
		"",
		"",
		&data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"required unknown extension fails even when its payload is present");
	CHECK(data == NULL, "no data object on failure");
	if (data) cgltf_free(data);
}

/* Case 5: known extension declared in both arrays -> success, typed fields are
 * populated and no raw duplicate is stored for the typed occurrence. */
static void case_known_required_succeeds(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"KHR_materials_unlit\"]",
		"[\"KHR_materials_unlit\"]",
		"",
		"",
		"\"KHR_materials_unlit\": {}",
		"",
		&data);

	CHECK(result == cgltf_result_success, "supported required extension parses");
	if (result != cgltf_result_success)
	{
		return;
	}

	CHECK(data->materials[0].unlit == 1, "KHR_materials_unlit still drives the typed field");
	CHECK(find_extension(data->materials[0].extensions, data->materials[0].extensions_count,
		"KHR_materials_unlit") < 0, "typed extension is not duplicated as raw data");
	CHECK(data->extensions_required_count == 1, "extensionsRequired declaration retained");
	cgltf_free(data);
}

/* Case 6: mix of known and unknown extensions. Unknown in used only is kept;
 * adding the same unknown name to required flips the whole parse into the
 * dedicated error while the known extension remains harmless. */
static void case_mixed_matrix(void)
{
	/* 6a: known required + unknown used-only -> success, raw data preserved. */
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"KHR_materials_unlit\", \"VENDOR_custom_thing\"]",
		"[\"KHR_materials_unlit\"]",
		"",
		"",
		"\"KHR_materials_unlit\": {}, \"VENDOR_custom_thing\": {\"matMarker\": 3}",
		"",
		&data);

	CHECK(result == cgltf_result_success, "mixed declaration with unknown used-only parses");
	if (result == cgltf_result_success)
	{
		CHECK(data->materials[0].unlit == 1, "known extension behavior unchanged in mixed case");
		int idx = find_extension(data->materials[0].extensions,
			data->materials[0].extensions_count, "VENDOR_custom_thing");
		CHECK(idx >= 0, "unknown extension still preserved alongside typed one");
		if (idx >= 0) assert_raw_fragment(&data->materials[0].extensions[idx], "matMarker");
		cgltf_free(data);
	}

	/* 6b: both required -> failure, error is the extension-specific code rather
	 * than a generic invalid_gltf. */
	data = NULL;
	result = parse_case(
		"[\"KHR_materials_unlit\", \"VENDOR_custom_thing\"]",
		"[\"KHR_materials_unlit\", \"VENDOR_custom_thing\"]",
		"",
		"",
		"\"KHR_materials_unlit\": {}, \"VENDOR_custom_thing\": {\"matMarker\": 3}",
		"",
		&data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"one unsupported required extension fails the mixed document");
	CHECK(data == NULL, "no data object on mixed failure");
	if (data) cgltf_free(data);
}

/* Case 7: multiple unknown required extensions -> same dedicated error. */
static void case_multiple_unknown_required_fails(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"VENDOR_a\", \"VENDOR_b\"]",
		"[\"VENDOR_a\", \"VENDOR_b\"]",
		"",
		"",
		"",
		"",
		&data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"any unsupported required extension yields the dedicated error");
	CHECK(data == NULL, "no data object on failure");
	if (data) cgltf_free(data);
}

/* Case 8: KHR_texture_transform behavior is unchanged while a sibling unknown
 * extension on the same texture view is preserved as raw data. */
static void case_texture_transform_kept_and_unknown_preserved(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[\"KHR_texture_transform\", \"VENDOR_tv_thing\"]",
		"[]",
		"",
		"\"KHR_texture_transform\": {\"offset\": [0.25, 0.5]}, \"VENDOR_tv_thing\": {\"tvMarker\": 7}",
		"",
		"",
		&data);

	CHECK(result == cgltf_result_success, "texture view with known and unknown extensions parses");
	if (result != cgltf_result_success)
	{
		return;
	}

	cgltf_texture_view* view = &data->materials[0].pbr_metallic_roughness.base_color_texture;
	CHECK(view->has_transform == 1, "KHR_texture_transform still sets has_transform");
	CHECK(view->transform.offset[0] > 0.24f && view->transform.offset[0] < 0.26f,
		"KHR_texture_transform offset parsed into typed field");
	CHECK(view->transform.offset[1] > 0.49f && view->transform.offset[1] < 0.51f,
		"KHR_texture_transform second offset component parsed");

	int idx = find_extension(view->extensions, view->extensions_count, "VENDOR_tv_thing");
	CHECK(idx >= 0, "unknown sibling extension on texture view preserved");
	if (idx >= 0) assert_raw_fragment(&view->extensions[idx], "tvMarker");
	CHECK(find_extension(view->extensions, view->extensions_count, "KHR_texture_transform") < 0,
		"typed KHR_texture_transform is not duplicated as raw data");

	cgltf_free(data);
}

/* Case 9: the required-extension gate applies to the GLB container too, not
 * only to plain JSON glTF. */
static void case_glb_unknown_required_fails(void)
{
	const char* json =
		"{"
		"  \"asset\": {\"version\": \"2.0\"},"
		"  \"extensionsUsed\": [\"VENDOR_glb_thing\"],"
		"  \"extensionsRequired\": [\"VENDOR_glb_thing\"]"
		"}";

	cgltf_size json_len = strlen(json);
	cgltf_size padding = (4 - (json_len % 4)) % 4;
	cgltf_size chunk_len = json_len + padding;

	unsigned char glb[1024];
	memset(glb, 0, sizeof(glb));
	cgltf_size offset = 0;

	memcpy(glb + offset, "glTF", 4); offset += 4;             /* magic */
	uint32_t version = 2;
	memcpy(glb + offset, &version, 4); offset += 4;           /* version */
	uint32_t total = 12u + 8u + (uint32_t)chunk_len;
	memcpy(glb + offset, &total, 4); offset += 4;             /* total length */
	memcpy(glb + offset, &chunk_len, 4); offset += 4;         /* chunk length */
	memcpy(glb + offset, "JSON", 4); offset += 4;             /* chunk type */
	memcpy(glb + offset, json, json_len); offset += json_len; /* chunk data (0-padded) */

	cgltf_options options;
	memset(&options, 0, sizeof(options));
	cgltf_data* data = NULL;
	cgltf_result result = cgltf_parse(&options, glb, total, &data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"GLB container honors the required-extension gate");
	CHECK(data == NULL, "no data object on GLB failure");
	if (data) cgltf_free(data);
}

/* Case 10: a valid GLB with an unknown used-only extension parses. */
static void case_glb_unknown_used_succeeds(void)
{
	const char* json =
		"{"
		"  \"asset\": {\"version\": \"2.0\"},"
		"  \"extensionsUsed\": [\"VENDOR_glb_thing\"],"
		"  \"extensions\": {\"VENDOR_glb_thing\": {\"glbMarker\": 9}}"
		"}";

	cgltf_size json_len = strlen(json);
	cgltf_size padding = (4 - (json_len % 4)) % 4;
	cgltf_size chunk_len = json_len + padding;

	unsigned char glb[1024];
	memset(glb, 0, sizeof(glb));
	cgltf_size offset = 0;

	memcpy(glb + offset, "glTF", 4); offset += 4;
	uint32_t version = 2;
	memcpy(glb + offset, &version, 4); offset += 4;
	uint32_t total = 12u + 8u + (uint32_t)chunk_len;
	memcpy(glb + offset, &total, 4); offset += 4;
	memcpy(glb + offset, &chunk_len, 4); offset += 4;
	memcpy(glb + offset, "JSON", 4); offset += 4;
	memcpy(glb + offset, json, json_len); offset += json_len;

	cgltf_options options;
	memset(&options, 0, sizeof(options));
	cgltf_data* data = NULL;
	cgltf_result result = cgltf_parse(&options, glb, total, &data);

	CHECK(result == cgltf_result_success, "GLB with unknown used-only extension parses");
	if (result == cgltf_result_success)
	{
		int idx = find_extension(data->data_extensions, data->data_extensions_count, "VENDOR_glb_thing");
		CHECK(idx >= 0, "GLB unknown root extension preserved as raw data");
		if (idx >= 0) assert_raw_fragment(&data->data_extensions[idx], "glbMarker");
		cgltf_free(data);
	}
}

/* Case 11: every supported extension name passes the required gate. This locks
 * the supported-set list against accidental typos / omissions. */
static void case_all_supported_required_pass(void)
{
	static const char* const supported[] = {
		"EXT_mesh_gpu_instancing",
		"EXT_meshopt_compression",
		"EXT_texture_webp",
		"KHR_draco_mesh_compression",
		"KHR_lights_punctual",
		"KHR_materials_anisotropy",
		"KHR_materials_clearcoat",
		"KHR_materials_diffuse_transmission",
		"KHR_materials_dispersion",
		"KHR_materials_emissive_strength",
		"KHR_materials_ior",
		"KHR_materials_iridescence",
		"KHR_materials_pbrSpecularGlossiness",
		"KHR_materials_sheen",
		"KHR_materials_specular",
		"KHR_materials_transmission",
		"KHR_materials_unlit",
		"KHR_materials_variants",
		"KHR_materials_volume",
		"KHR_meshopt_compression",
		"KHR_texture_basisu",
		"KHR_texture_transform"
	};

	for (cgltf_size i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
	{
		char json[512];
		snprintf(json, sizeof(json),
			"{\"asset\": {\"version\": \"2.0\"},"
			" \"extensionsUsed\": [\"%s\"],"
			" \"extensionsRequired\": [\"%s\"]}",
			supported[i], supported[i]);

		cgltf_data* data = NULL;
		cgltf_result result = parse_string(json, &data);
		char message[160];
		snprintf(message, sizeof(message), "supported extension %s accepted as required", supported[i]);
		CHECK(result == cgltf_result_success, message);
		if (data) cgltf_free(data);
	}
}

/* Case 12: unknown extension listed in required but not in used is still
 * rejected (the required list alone is authoritative for the gate). */
static void case_unknown_required_without_used_fails(void)
{
	cgltf_data* data = NULL;
	cgltf_result result = parse_case(
		"[]",
		"[\"VENDOR_required_only\"]",
		"",
		"",
		"",
		"",
		&data);

	CHECK(result == cgltf_result_unknown_required_extension,
		"required-only unknown extension is rejected without a used entry");
	CHECK(data == NULL, "no data object on failure");
	if (data) cgltf_free(data);
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	case_no_extensions();
	case_unknown_used_all_placements();
	case_unknown_required_fails();
	case_unknown_required_with_payload_fails();
	case_known_required_succeeds();
	case_mixed_matrix();
	case_multiple_unknown_required_fails();
	case_texture_transform_kept_and_unknown_preserved();
	case_glb_unknown_required_fails();
	case_glb_unknown_used_succeeds();
	case_all_supported_required_pass();
	case_unknown_required_without_used_fails();

	printf("extension matrix: %d checks, %d errors\n", num_checks, num_errors);
	return num_errors == 0 ? 0 : 1;
}
