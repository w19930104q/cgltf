/* Boundary-validation regression runner.
 *
 * Usage: test_malformed <file> <ok|error>
 *
 * Runs the full parse -> load_buffers -> validate pipeline on the given
 * (possibly malformed) glTF file. Malformed inputs must be rejected with a
 * cgltf error code instead of crashing or reading out of bounds (the binary
 * is built with AddressSanitizer, so any out-of-bounds access aborts the
 * process and fails the test). Valid inputs must keep parsing successfully.
 *
 * Exit code 0 means the actual outcome matched the expectation.
 */

#define CGLTF_IMPLEMENTATION
#include "../cgltf.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
	if (argc < 3 || (strcmp(argv[2], "ok") != 0 && strcmp(argv[2], "error") != 0))
	{
		fprintf(stderr, "usage: %s <file> <ok|error>\n", argv[0]);
		return 2;
	}

	int expect_ok = strcmp(argv[2], "ok") == 0;

	cgltf_options options;
	memset(&options, 0, sizeof(cgltf_options));
	cgltf_data* data = NULL;
	cgltf_result result = cgltf_parse_file(&options, argv[1], &data);

	if (result == cgltf_result_success)
		result = cgltf_load_buffers(&options, data, argv[1]);

	if (result == cgltf_result_success)
		result = cgltf_validate(data);

	printf("%s: result %d (%s), expected %s\n", argv[1], (int)result,
		result == cgltf_result_success ? "ok" : "error", argv[2]);

	cgltf_free(data);

	if (expect_ok != (result == cgltf_result_success))
	{
		printf("MISMATCH\n");
		return 1;
	}

	return 0;
}
