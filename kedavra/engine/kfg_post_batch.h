// SPDX-License-Identifier: GPL-3.0-or-later
// Test-harness I/O only; never linked into Gamescope. Manifest: width height
// spec input.rgba output.rgba repetitions. Paths cannot contain whitespace.
static int post_batch(kfg_vk *vk, const char *manifest)
{
	FILE *f = fopen(manifest, "r");
	if (!f) { perror(manifest); return 1; }
	char line[4096], spec[512], input[1024], output[1024];
	int status = 0;
	post_env e = {0};
	unsigned w, h, count;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "%u %u %511s %1023s %1023s %u", &w, &h, spec, input, output, &count) != 6 ||
		    !w || !h || w > 8192 || h > 8192 || count < 1 || count > 100000) {
			fprintf(stderr, "invalid batch row\n"); status = 1; break;
		}
		if (e.w != w || e.h != h) {
			if (e.vk) env_destroy(&e);
			if (env_init(&e, vk, w, h)) { status = 1; break; }
		}
		size_t bytes = (size_t)w*h*4;
		uint8_t *src = malloc(bytes), *dst = malloc(bytes);
		FILE *in = fopen(input, "rb");
		if (!src || !dst || !in) { if (in) fclose(in); free(src); free(dst); status = 1; break; }
		int valid = fread(src, 1, bytes, in) == bytes && fgetc(in) == EOF;
		fclose(in);
		if (!valid) { free(src); free(dst); status = 1; break; }
		// Warm-up is excluded from timestamp samples. Every iteration reads the same input.
		for (unsigned k = 0; k < count+5; k++) {
			memcpy(dst, src, bytes);
			double ns = 0;
			if (run_post(&e, !strcmp(spec, "off") ? "" : spec, 0.0f, dst, &ns)) { status = 1; break; }
			if (k >= 5) printf("SAMPLE\t%s\t%s\t%u\t%.6f\n", input, spec, k-5, ns/1e6);
		}
		FILE *out = status ? NULL : fopen(output, "wb");
		if (!out) status = 1;
		else { if (fwrite(dst, 1, bytes, out) != bytes) status = 1; if (fclose(out)) status = 1; }
		free(src); free(dst);
		if (status) break;
	}
	if (ferror(f)) status = 1;
	fclose(f);
	if (e.vk) env_destroy(&e);
	return status;
}
