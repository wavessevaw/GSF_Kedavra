// SPDX-License-Identifier: GPL-3.0-or-later
// Executes production SPIR-V, including R8 sampling and RGBA16F storage.
#define main corpus_main
#include "../engine/kfg_vktest.c"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const bool legacy = strcmp(argv[2], "legacy") == 0;
    kfg_vk vk;
    if (kfg_vk_init(&vk, argv[1], false)) return 2;
    engine e;
    if (engine_init(&e, &vk, 64, 48, VK_FORMAT_R8G8B8A8_UNORM)) return 2;
    uint8_t a[32 * 24] = {0}, b[32 * 24];
    uint16_t field[16 * 12 * 4], stats[16] = {0};
    float out[16 * 12 * 4];
    const int center = 6 * 16 + 8;
    int failures = 0, cases = 0;
    const int levels[] = {13, 38, 64};
    for (int pair = 0; pair < 2; pair++)
    for (int phase = 1; phase <= 3; phase++)
    for (int zcase = 1; zcase <= 3; zcase++)
    for (int level = 0; level < 3; level++)
    for (int unchanged = 0; unchanged < 2; unchanged++) {
        float t = phase == 3 ? 0.5f : phase / 3.0f;
        float z = zcase * 0.25f;
        memset(b, levels[level], sizeof(b));
        for (int i = 0; i < 16 * 12; i++) {
            field[4*i] = field[4*i+1] = 0;
            field[4*i+2] = float_to_half(z);
            field[4*i+3] = float_to_half(0.6f);
        }
        field[4*center] = float_to_half(unchanged ? 0.0f : 8.0f);
        if (kfg_vk_upload_raw(&vk, &e.luma[0][1], a, sizeof(a)) ||
            kfg_vk_upload_raw(&vk, &e.luma[1][1], b, sizeof(b)) ||
            kfg_vk_upload_raw(&vk, &e.select[0], field, sizeof(field)) ||
            kfg_vk_upload_raw(&vk, &e.stats[1], stats, sizeof(stats))) return 2;
        kfg_plan p = {0};
        kfg_pass *x = kfg_plan_add(&p, KFG_SHADER_SELECT,
            kfg_ref(KFG_ROLE_PAIR_SELECT, 1, 0), "repair-contract");
        x->slots[0] = kfg_ref(KFG_ROLE_LUMA, 0, 1);
        x->slots[1] = kfg_ref(KFG_ROLE_LUMA, 1, 1);
        x->slots[2] = kfg_ref(KFG_ROLE_SELECT, 0, 0);
        x->slots[3] = kfg_ref(KFG_ROLE_STATS, 1, 0);
        x->nslots = 4;
        x->params.src_size[0] = 32; x->params.src_size[1] = 24;
        x->params.full_size[0] = 64; x->params.full_size[1] = 48;
        x->params.t = t;
        x->params.flags = KFG_FLAG_REPAIR | (pair ? KFG_FLAG_PAIR : 0);
        const uint32_t bs[2] = {16, 12};
        kfg_set_domain(x, bs, 8, 8);
        if (run_plan(&e, &p, NULL, NULL) ||
            kfg_vk_download_rgba16f(&vk, &e.pair_select[1], out)) return 2;
        float error = levels[level] / 255.0f;
        float s = fminf(1, fmaxf(0, (error - 0.1f) / 0.1f));
        s = s*s*(3-2*s);
        float expected = z;
        if (!unchanged && (!pair || legacy)) expected += ((t < .5f ? 0 : 1) - z)*s;
        bool ok = isfinite(out[4*center+2]) && fabsf(out[4*center+2]-expected) < .002f &&
            fabsf(out[4*center]) < .001f && fabsf(out[4*center+1]) < .001f &&
            fabsf(out[4*center+3] - (unchanged ? half_round(.6f) : error)) < .002f;
        cases++; failures += !ok;
        if (!ok) fprintf(stderr, "pair=%d t=%g z=%g level=%d unchanged=%d actual=%g expected=%g vector=%g error=%g\n",
            pair,t,z,levels[level],unchanged,out[4*center+2],expected,out[4*center],out[4*center+3]);
    }
    engine_destroy(&e); kfg_vk_destroy(&vk);
    printf("{\"cases\":%d,\"failures\":%d,\"legacy\":%s}\n", cases, failures, legacy ? "true" : "false");
    return failures ? 1 : 0;
}
