// SPDX-License-Identifier: GPL-3.0-or-later
// Render an out-of-bounds duplicate-motion case through the production warp.
#define main corpus_main
#include "../engine/kfg_vktest.c"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    kfg_vk vk;
    if (kfg_vk_init(&vk, argv[1], false)) return 2;
    engine e;
    if (engine_init(&e, &vk, 64, 48, VK_FORMAT_R8G8B8A8_UNORM)) return 2;
    uint8_t a[64*48*4], b[sizeof(a)], out[sizeof(a)];
    uint16_t field[16*12*4], stats[16] = {0};
    for (int i=0; i<64*48; i++) {
        for (int c=0; c<3; c++) { a[4*i+c]=51; b[4*i+c]=77; }
        a[4*i+3] = b[4*i+3] = 255;
    }
    for (int i=0; i<16*12; i++) {
        field[4*i] = field[4*i+1] = 0;
        field[4*i+2] = float_to_half(.5f);
        field[4*i+3] = float_to_half(.1f);
    }
    // Pixel (2,22), t=.5: its own s00 and neighbour s10 both have
    // v=(64,0), but residuals .19 and .01. A falls outside, B remains inside.
    int own = 5*16, neighbour = own+1;
    field[4*own] = field[4*neighbour] = float_to_half(64);
    field[4*own+2] = float_to_half(.9f);
    field[4*own+3] = float_to_half(.19f);
    field[4*neighbour+2] = float_to_half(.1f);
    field[4*neighbour+3] = float_to_half(.01f);
    if (kfg_vk_upload_rgba8(&vk, &e.color[0], a) ||
        kfg_vk_upload_rgba8(&vk, &e.color[1], b) ||
        kfg_vk_upload_raw(&vk, &e.stats[1], stats, sizeof(stats)) ||
        kfg_vk_upload_raw(&vk, &e.pair_select[1], field, sizeof(field))) return 2;
    kfg_plan p;
    kfg_plan_generate(&p, 64, 48, 0, 1, 1, .5f, 0);
    if (run_plan(&e, &p, NULL, NULL) || kfg_vk_download_rgba8(&vk, &e.output, out)) return 2;
    int actual = out[4*(22*64+2)];
    int expected = !strcmp(argv[2], "fixed") ? 77 : 64;
    printf("edge pixel: actual=%d expected=%d\n", actual, expected);
    engine_destroy(&e); kfg_vk_destroy(&vk);
    return abs(actual-expected) <= 1 ? 0 : 1;
}
