/*
 * ogg-reencode: re-encodes an Ogg Vorbis file at a given VBR quality (host tool for
 * make-assets.sh; built from the pinned libogg/libvorbis sources).
 *
 *   ogg-reencode <in.ogg> <out.ogg> <quality -0.1..1.0>
 *
 * Channels, sample rate and Vorbis comments (STK reads loop points from them) are kept.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

static int write_page(FILE *out, const ogg_page *og)
{
    return fwrite(og->header, 1, og->header_len, out) == (size_t)og->header_len &&
           fwrite(og->body, 1, og->body_len, out) == (size_t)og->body_len;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s in.ogg out.ogg quality\n", argv[0]);
        return 2;
    }
    OggVorbis_File vf;
    if (ov_fopen(argv[1], &vf) != 0) {
        fprintf(stderr, "%s: not an Ogg Vorbis file\n", argv[1]);
        return 1;
    }
    const vorbis_info *in_info = ov_info(&vf, -1);
    const vorbis_comment *in_comment = ov_comment(&vf, -1);
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        return 1;
    }

    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, in_info->channels, in_info->rate, (float)atof(argv[3])) != 0) {
        fprintf(stderr, "%s: encoder init failed\n", argv[1]);
        return 1;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    for (int i = 0; i < in_comment->comments; i++)
        vorbis_comment_add(&vc, in_comment->user_comments[i]);

    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op, header, header_comm, header_code;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    srand((unsigned)time(NULL));
    ogg_stream_init(&os, rand());
    vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &header_comm);
    ogg_stream_packetin(&os, &header_code);
    while (ogg_stream_flush(&os, &og))
        write_page(out, &og);

    int eos = 0, section = 0;
    while (!eos) {
        float **pcm;
        long n = ov_read_float(&vf, &pcm, 1024, &section);
        if (n < 0)
            continue; /* hole in the stream: skip */
        if (n == 0) {
            vorbis_analysis_wrote(&vd, 0);
        } else {
            float **buf = vorbis_analysis_buffer(&vd, (int)n);
            for (int c = 0; c < in_info->channels; c++)
                for (long i = 0; i < n; i++)
                    buf[c][i] = pcm[c][i];
            vorbis_analysis_wrote(&vd, (int)n);
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (!eos && ogg_stream_pageout(&os, &og)) {
                    write_page(out, &og);
                    if (ogg_page_eos(&og))
                        eos = 1;
                }
            }
        }
        if (n == 0)
            break;
    }
    while (ogg_stream_flush(&os, &og))
        write_page(out, &og);
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    ov_clear(&vf);
    return fclose(out) == 0 ? 0 : 1;
}
